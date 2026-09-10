// sys_process_spawn/sys_fork/sys_exec 구현. process_ops.hpp 상단 주석
// 참고.
#include "process_ops.hpp"

#include "elf_loader.hpp"
#include "page_table.hpp"
#include "tss.hpp"

#include <new>

#include <klog.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <mm/slab.hpp>
#include <object/handle_table.hpp>
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

#include <uapi.hpp>

// usermode.S(M8) — 유저모드로 직접 진입한다(IRETQ). sys_exec가 성공
// 경로에서 곧바로 이걸 부른다(create_user_thread류의 "다음에 스케줄될
// 때" 방식이 아니라, 지금 이 스레드가 바로 새 이미지로 뛰어드는
// 것이므로).
extern "C" [[noreturn]] void enter_usermode(uint64_t rip, uint64_t rsp, uint64_t arg0);

namespace arch_x86_64 {

namespace {

// process_spawn/exec_current가 공유하는 조립 — 새 주소공간 하나에
// ELF+유저스택(16KiB)+argv(선택, 한 페이지 이내)를 갖춘다.
// kernel_main.cpp::setup_initrun_process(M8)와 같은 고정 유저
// 가상주소대를 그대로 쓴다 — 서로 다른 주소공간이라 겹칠 일이 없다.
struct built_process {
    kern::object::address_space* space = nullptr;
    uint64_t entry_rip = 0;
    uint64_t user_rsp = 0;
    uint64_t arg0 = 0;  // argv 매핑 주소, 없으면 0.
};

constexpr uint64_t k_user_stack_top = 0x0000700000000000ull;
constexpr uint32_t k_user_stack_pages = 4;
constexpr uint64_t k_argv_user_vaddr = 0x0000700000002000ull;

// M12(ADR-147) — sys_alloc_dma_buffer가 매핑하는 고정 가상주소. 다른
// M12 임시 배선(k_m12_self_elf_user_vaddr=0x...3000,
// k_argv_user_vaddr=0x...2000, k_m12_self_info_user_vaddr=0x...10000)
// 과 겹치지 않는, 충분히 위쪽인 자리.
constexpr uint64_t k_dma_buffer_user_vaddr = 0x0000700000200000ull;

// M14(ADR-007/038/039) — sys_map_phys가 매핑하는 고정 가상주소.
// k_dma_buffer_user_vaddr 다음 슬롯 — 두 syscall이 서로 다른 물리
// 출처(할당 vs 기존 하드웨어 영역)를 다루므로 동시에 살아 있어도
// 겹치지 않게 자리를 분리해 둔다.
constexpr uint64_t k_mmio_user_vaddr = 0x0000700000300000ull;

// M24(general-purpose-completion.md §M24, ADR-180) — sys_brk의 힙
// 영역. kernel-memory.md ADR-160의 슬롯 표를 따른다 — 슬롯 4
// (k_ipc_mapped_pages_user_vaddr=0x...400000, kernel/core/ipc/message.hpp)
// 다음, 슬롯 5(k_user_stack_top + 5*1MiB). 다른 슬롯과 달리 이
// 영역은 "가변 크기로 계속 자라는" 용도라 슬롯 하나(1MiB) 전체를
// 예산으로 쓴다 — M24가 요구하는 검증(셸이 malloc 몇 번 쓰는 정도)
// 에는 차고 넘친다. 더 큰 힙이 필요해지면 이 상수 하나만 넓히면
// 된다(다음 슬롯이 아직 비어 있다, ADR-160 §결정2의 "5+: 예약").
constexpr uint64_t k_heap_user_vaddr = 0x0000700000500000ull;
constexpr uint64_t k_heap_region_size = 0x100000ull;  // 1MiB.

process_spawn_error build_process(const uint8_t* elf_data, uint64_t elf_size,
                                   const uint8_t* argv_blob, uint64_t argv_size, bool trusted,
                                   built_process& out) {
    if (argv_size > kern::mm::k_page_size) {
        return process_spawn_error::invalid_argument;
    }

    auto root = create_address_space_root();
    if (!root.is_ok()) {
        return process_spawn_error::out_of_memory;
    }
    uint64_t pml4_phys = root.value();

    void* space_mem = kern::mm::slab_alloc(sizeof(kern::object::address_space));
    if (space_mem == nullptr) {
        return process_spawn_error::out_of_memory;
    }
    auto* space = new (space_mem) kern::object::address_space();
    space->trusted = trusted;
    space->page_table_root = pml4_phys;

    auto load_result = load_elf(pml4_phys, elf_data, elf_size);
    if (!load_result.is_ok()) {
        kern::mm::slab_free(space, sizeof(kern::object::address_space));
        return process_spawn_error::elf_load_failed;
    }

    for (uint32_t i = 0; i < k_user_stack_pages; ++i) {
        auto page = kern::mm::alloc_pages(0, 0);
        if (!page.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
        uint64_t vaddr = k_user_stack_top - (k_user_stack_pages - i) * kern::mm::k_page_size;
        auto mapped =
            map_page(pml4_phys, vaddr, page.value(), page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
    }

    uint64_t arg0 = 0;
    if (argv_size > 0 && argv_blob != nullptr) {
        auto argv_page = kern::mm::alloc_pages(0, 0);
        if (!argv_page.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
        void* argv_virt = kern::mm::phys_to_virt(argv_page.value());
        __builtin_memset(argv_virt, 0, kern::mm::k_page_size);
        __builtin_memcpy(argv_virt, argv_blob, argv_size);
        auto argv_mapped = map_page(pml4_phys, k_argv_user_vaddr, argv_page.value(),
                                     page_perm::user);  // 읽기전용(write 비트 없음).
        if (!argv_mapped.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
        arg0 = k_argv_user_vaddr;
    }

    // M12(uapi.hpp::k_m12_self_info_user_vaddr 주석) — kernel_main.cpp::
    // setup_initrun_process가 initrun의 최초 스폰에서만 하던 "원본 ELF
    // 바이트를 새 주소공간에도 복사해 self_info로 알려 준다"를 여기
    // build_process()로 일반화한다 — process_spawn/exec_current로 만드는
    // **모든** 프로세스가 다 이걸 받는다. procsrv도 initrun의
    // sys_process_spawn으로 만들어지는 이상 이 경로를 그대로 타므로,
    // initrun과 똑같은 방식(uapi::k_m12_self_info_user_vaddr을 읽어
    // sys_fork+sys_exec)으로 "자기 자신을 fork/exec"할 수 있다 — M12
    // QEMU 목표(procsrv 자기 자신 fork/exec)가 요구하는 조건이 이것뿐.
    // ADR-160(kernel-memory.md, 슬롯 0의 예산 검증) — self_elf 복사가
    // 이웃 슬롯(self_info, uapi::k_m12_self_info_user_vaddr)을 침범하기
    // 전에 명시적으로 거부한다. ADR-149가 겪은 버그(간격을 넘은 ELF가
    // already_mapped로만 우회 발견됨)를 재발 방지한다.
    if (elf_size > uapi::k_m12_self_info_user_vaddr - uapi::k_m12_self_elf_user_vaddr) {
        kern::mm::slab_free(space, sizeof(kern::object::address_space));
        return process_spawn_error::capability_slot_overflow;
    }

    uint64_t self_elf_pages = (elf_size + kern::mm::k_page_size - 1) / kern::mm::k_page_size;
    for (uint64_t i = 0; i < self_elf_pages; ++i) {
        auto page = kern::mm::alloc_pages(0, 0);
        if (!page.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
        void* virt = kern::mm::phys_to_virt(page.value());
        uint64_t offset = i * kern::mm::k_page_size;
        uint64_t remaining = elf_size - offset;
        uint64_t copy_len = remaining < kern::mm::k_page_size ? remaining : kern::mm::k_page_size;
        __builtin_memset(virt, 0, kern::mm::k_page_size);
        __builtin_memcpy(virt, elf_data + offset, copy_len);
        auto mapped = map_page(pml4_phys, uapi::k_m12_self_elf_user_vaddr + offset, page.value(),
                                page_perm::user);
        if (!mapped.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
    }

    auto info_page = kern::mm::alloc_pages(0, 0);
    if (!info_page.is_ok()) {
        kern::mm::slab_free(space, sizeof(kern::object::address_space));
        return process_spawn_error::out_of_memory;
    }
    auto* self_info = static_cast<uapi::m12_self_info*>(kern::mm::phys_to_virt(info_page.value()));
    self_info->elf_addr = uapi::k_m12_self_elf_user_vaddr;
    self_info->elf_size = elf_size;
    auto info_mapped = map_page(pml4_phys, uapi::k_m12_self_info_user_vaddr, info_page.value(),
                                 page_perm::user);
    if (!info_mapped.is_ok()) {
        kern::mm::slab_free(space, sizeof(kern::object::address_space));
        return process_spawn_error::out_of_memory;
    }

    out.space = space;
    out.entry_rip = load_result.value();
    out.user_rsp = k_user_stack_top;
    out.arg0 = arg0;
    return process_spawn_error::ok;
}

}  // namespace

process_spawn_error process_spawn(const uint8_t* elf_data, uint64_t elf_size,
                                   const uint8_t* argv_blob, uint64_t argv_size,
                                   bool grant_trusted, bool create_endpoint,
                                   const uapi::handle_transfer* inherited_handles,
                                   uint32_t inherited_handle_count,
                                   uint32_t& out_endpoint_proxy_handle,
                                   uint32_t& out_thread_handle) {
    built_process built;
    auto err = build_process(elf_data, elf_size, argv_blob, argv_size, grant_trusted, built);
    if (err != process_spawn_error::ok) {
        return err;
    }

    kern::object::handle_table* handles = kern::object::create_handle_table();
    if (handles == nullptr) {
        kern::mm::slab_free(built.space, sizeof(kern::object::address_space));
        return process_spawn_error::out_of_memory;
    }

    // M13(ADR-151) — 스폰 시점 캐패빌리티 주입. 등록/탐색 서비스가
    // 없는 지금(OPEN-59), 서로 다른 서버가 서로를 IPC로 부르려면
    // 스폰하는 쪽(대개 initrun)이 핸들을 직접 물려주는 것이 유일한
    // 방법이다 — handle_table::create_owner/create_proxy는 순서대로
    // 다음 빈 슬롯을 배정하므로(handle_table.cpp::allocate_slot), 아래
    // 순서(endpoint 먼저, 그다음 inherited_handles)가 그대로 새
    // 프로세스의 handle 1, 2, 3, ...이 된다 — kernel_main.cpp의 boot
    // endpoint(handle 1) 관례와 일치.
    if (create_endpoint) {
        void* ep_mem = kern::mm::slab_alloc(sizeof(kern::object::endpoint));
        if (ep_mem == nullptr) {
            kern::mm::slab_free(built.space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
        auto* ep = new (ep_mem) kern::object::endpoint();
        auto owner = handles->create_owner(
            kern::object::object_kind::endpoint, kern::object::k_right_can_send | kern::object::k_right_can_recv, ep);
        if (!owner.is_ok()) {
            kern::mm::slab_free(built.space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }

        kern::object::thread* caller = kern::sched::current();
        if (caller != nullptr && caller->handles != nullptr) {
            auto proxy = handles->create_proxy(owner.value(), kern::object::k_right_can_send,
                                                *caller->handles, 0, false);
            if (proxy.is_ok()) {
                out_endpoint_proxy_handle = proxy.value();
            }
        }
    }

    if (inherited_handle_count > uapi::k_max_spawn_inherited_handles) {
        kern::mm::slab_free(built.space, sizeof(kern::object::address_space));
        return process_spawn_error::invalid_argument;
    }
    kern::object::thread* caller = kern::sched::current();
    if (inherited_handle_count > 0 && (caller == nullptr || caller->handles == nullptr)) {
        kern::mm::slab_free(built.space, sizeof(kern::object::address_space));
        return process_spawn_error::not_a_user_process;
    }
    for (uint32_t i = 0; i < inherited_handle_count; ++i) {
        // 부분 실패(예: 호출자가 이미 닫힌 핸들을 넘김)는 이 항목만
        // 건너뛴다 — deliver_message의 handles[] 처리와 같은 정신
        // (objects.md §4 3단계).
        caller->handles->create_proxy(inherited_handles[i].src_handle,
                                       inherited_handles[i].rights_mask, *handles, 0, false);
    }

    kern::object::thread* t =
        kern::sched::create_user_thread(built.entry_rip, built.user_rsp, built.arg0, built.space, handles);
    if (t == nullptr) {
        kern::mm::slab_free(built.space, sizeof(kern::object::address_space));
        return process_spawn_error::out_of_memory;
    }

    kern::sched::enqueue(*t);
    kern::klog::printf("[process] spawn ok entry=0x%lx trusted=%u\n",
                 static_cast<unsigned long>(built.entry_rip), grant_trusted);

    // M22(ADR-178) — out_endpoint_proxy_handle과 같은 "호출자 소유"
    // 자리에 새 스레드를 가리키는 kill 전용 핸들을 만들어 둔다.
    // caller는 위 236번째 줄에서 이미 구한 값(같은 스레드, 그 사이
    // g_current가 바뀔 일이 없다)을 그대로 재사용한다.
    if (caller != nullptr && caller->handles != nullptr) {
        auto thread_owner =
            caller->handles->create_owner(kern::object::object_kind::thread, kern::object::k_right_can_kill, t);
        if (thread_owner.is_ok()) {
            out_thread_handle = thread_owner.value();
        }
    }
    return process_spawn_error::ok;
}

uint64_t fork_current(uint64_t saved_user_rip, uint64_t saved_user_rflags,
                       uint64_t saved_user_rsp, uint64_t saved_rbx, uint64_t saved_rbp,
                       uint64_t saved_r12, uint64_t saved_r13, uint64_t saved_r14,
                       uint64_t saved_r15) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return static_cast<uint64_t>(process_spawn_error::not_a_user_process);
    }

    auto child_root = clone_address_space_cow(self->owner_space->page_table_root);
    if (!child_root.is_ok()) {
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }

    void* space_mem = kern::mm::slab_alloc(sizeof(kern::object::address_space));
    if (space_mem == nullptr) {
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }
    auto* child_space = new (space_mem) kern::object::address_space();
    child_space->trusted = self->owner_space->trusted;
    child_space->confinement = self->owner_space->confinement;
    child_space->page_table_root = child_root.value();

    // M23(general-purpose-completion.md §M23, ADR-179) — procsrv.md
    // §3.6/§4.1이 그리는 "procsrv가 각 소유 서버에 IPC로 fd 복제를
    // 요청"하는 완전한 fd 진실 공급원 프로토콜은 이번 라운드에도
    // 구현하지 않는다(범위 좁힘, ADR-179 참고). 대신 커널이
    // handle_table 전체를 그대로 프록시로 복제한다 — 이전에는(M12
    // ~M22) 자식이 완전히 빈 테이블로 시작했다. VFS/FS 서버들의
    // "열린 파일" 상태(예: memfs의 read_cursor/write_cursor)는
    // open_file_id로만 식별되고 커널 핸들·발신자 신원과 무관하게
    // 서버 쪽에 남아 있으므로(servers/fs/memfs/main.cpp), 부모·자식이
    // 같은 open_file_id를 계속 쓰기만 하면 파일 오프셋 공유까지
    // 별도 프로토콜 없이 저절로 성립한다.
    kern::object::handle_table* child_handles = kern::object::create_handle_table();
    if (child_handles == nullptr) {
        kern::mm::slab_free(child_space, sizeof(kern::object::address_space));
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }
    for (uint32_t h = 1; h < kern::object::k_max_handles; ++h) {
        const kern::object::handle_entry* e = self->handles->debug_entry(static_cast<kern::object::handle>(h));
        if (e == nullptr || !e->valid) {
            continue;
        }
        // rights_mask=e->rights(축소 없음)+has_badge_override=false(부모
        // 것을 그대로 상속, handle_table.cpp::create_proxy 주석 참고) —
        // "완전히 같은 fd 테이블의 복사본"이라는 POSIX fork() 의미론
        // 그대로다. 실패(테이블 가득 참 등)는 objects.md §4 3단계와
        // 같은 정신으로 이 항목만 건너뛴다.
        self->handles->create_proxy(static_cast<kern::object::handle>(h), e->rights, *child_handles, 0,
                                     false);
    }

    kern::object::thread* child =
        kern::sched::create_forked_thread(saved_user_rip, saved_user_rflags, saved_user_rsp, saved_rbx,
                                     saved_rbp, saved_r12, saved_r13, saved_r14, saved_r15,
                                     child_space, child_handles);
    if (child == nullptr) {
        kern::mm::slab_free(child_space, sizeof(kern::object::address_space));
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }

    // ADR-154 §결정5 — 자식은 부모가 활성화해 둔 I/O 포트 범위를
    // 그대로 물려받는다(trusted 상속과 같은 정신). 제한하는 fork
    // 변형은 아직 없다(YAGNI).
    child->io_port_base = self->io_port_base;
    child->io_port_count = self->io_port_count;

    kern::sched::enqueue(*child);
    kern::klog::printf("[process] fork ok child_pml4=0x%lx\n",
                 static_cast<unsigned long>(child_root.value()));
    return 1;  // 부모 관점: 성공.
}

process_spawn_error exec_current(const uint8_t* elf_data, uint64_t elf_size,
                                  const uint8_t* argv_blob, uint64_t argv_size) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }

    // procsrv.md §4 4단계 — pid/identity/trusted/confinement는 그대로
    // 유지된다. trusted는 기존 address_space에서 그대로 물려받는다
    // (exec는 새 신원 부여가 아니다).
    built_process built;
    auto err =
        build_process(elf_data, elf_size, argv_blob, argv_size, self->owner_space->trusted, built);
    if (err != process_spawn_error::ok) {
        return err;
    }

    // **알려진 단순화**(process_ops.hpp 참고): 이전 address_space와 그
    // 페이지테이블/프레임은 회수하지 않고 버려둔다 — M12는 "실행
    // 이미지 교체가 실제로 동작한다"만 보이면 충분하고, 회수는 이후
    // 마일스톤이다.
    self->owner_space = built.space;

    // 지금 이 스레드로 CR3를 직접 전환한다 — 다음 kern::sched::yield/block
    // 없이 곧바로 새 이미지로 뛰어들 것이므로, 스케줄러의 일반
    // next_pml4_phys() 경로(다음 context switch 시점에만 전환)를 거치지
    // 않는다.
    asm volatile("mov %0, %%cr3" : : "r"(built.space->page_table_root) : "memory");

    kern::klog::printf("[process] exec ok entry=0x%lx\n", static_cast<unsigned long>(built.entry_rip));
    enter_usermode(built.entry_rip, built.user_rsp, built.arg0);
}

process_spawn_error alloc_dma_buffer(uint32_t order, uint64_t& out_virt_addr,
                                      uint64_t& out_phys_addr) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }
    if (!self->owner_space->trusted) {
        return process_spawn_error::not_a_user_process;
    }
    if (order > kern::mm::k_max_order) {
        return process_spawn_error::invalid_argument;
    }

    auto page = kern::mm::alloc_pages(order, 0);
    if (!page.is_ok()) {
        return process_spawn_error::out_of_memory;
    }
    uint64_t phys = page.value();
    uint64_t size = static_cast<uint64_t>(kern::mm::k_page_size) << order;

    for (uint64_t off = 0; off < size; off += kern::mm::k_page_size) {
        auto mapped = map_page(self->owner_space->page_table_root, k_dma_buffer_user_vaddr + off,
                                phys + off, page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            kern::mm::free_pages(phys, order);
            return process_spawn_error::out_of_memory;
        }
    }

    out_virt_addr = k_dma_buffer_user_vaddr;
    out_phys_addr = phys;
    return process_spawn_error::ok;
}

process_spawn_error map_phys(uint64_t phys_addr, uint64_t size, uint64_t& out_virt_addr) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }
    if (!self->owner_space->trusted) {
        return process_spawn_error::not_a_user_process;
    }
    if (size == 0 || size > uapi::k_max_mmio_map_bytes) {
        return process_spawn_error::invalid_argument;
    }

    uint64_t phys_base = phys_addr & ~(static_cast<uint64_t>(kern::mm::k_page_size) - 1);
    uint64_t offset_in_page = phys_addr - phys_base;
    uint64_t map_size = offset_in_page + size;
    map_size = (map_size + kern::mm::k_page_size - 1) & ~(static_cast<uint64_t>(kern::mm::k_page_size) - 1);

    for (uint64_t off = 0; off < map_size; off += kern::mm::k_page_size) {
        // 이전 sys_map_phys 호출이 이 슬롯의 일부를 이미 다른 물리주소로
        // 채워 뒀을 수 있다(고정 슬롯 재사용, 이 파일 상단 주석) —
        // map_page가 already_mapped로 실패하지 않도록 먼저 지운다.
        // 애초에 매핑된 적이 없으면 not_mapped로 조용히 실패하는데,
        // 그 결과는 무시해도 안전하다.
        (void)unmap_page(self->owner_space->page_table_root, k_mmio_user_vaddr + off);
        auto mapped = map_page(self->owner_space->page_table_root, k_mmio_user_vaddr + off,
                                phys_base + off, page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            return process_spawn_error::out_of_memory;
        }
    }

    out_virt_addr = k_mmio_user_vaddr + offset_in_page;
    return process_spawn_error::ok;
}

process_spawn_error io_activate(uint16_t io_base, uint16_t count) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }
    if (!self->owner_space->trusted) {
        return process_spawn_error::not_a_user_process;
    }
    self->io_port_base = io_base;
    self->io_port_count = count;
    sync_io_permission(*self);  // 지금 실행 중인 스레드 — 다음 스위치까지 기다리지 않는다.
    return process_spawn_error::ok;
}

process_spawn_error io_deactivate() {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }
    self->io_port_base = 0;
    self->io_port_count = 0;
    sync_io_permission(*self);
    return process_spawn_error::ok;
}

// M22(general-purpose-completion.md §M22, ADR-178) — endpoint.cpp::
// resolve_endpoint()와 정확히 같은 패턴(debug_entry로 핸들 검사)을
// object_kind::thread에 재사용한다.
process_kill_error process_kill(kern::object::handle_table& caller_handles, uint32_t h) {
    const kern::object::handle_entry* e = caller_handles.debug_entry(h);
    if (e == nullptr || !e->valid) {
        return process_kill_error::invalid_handle;
    }
    if (e->kind != kern::object::object_kind::thread) {
        return process_kill_error::wrong_object_type;
    }
    if ((e->rights & kern::object::k_right_can_kill) == 0) {
        return process_kill_error::permission_denied;
    }
    kern::sched::request_kill(*static_cast<kern::object::thread*>(e->object));
    return process_kill_error::ok;
}

// M24(general-purpose-completion.md §M24, ADR-180) — sys_brk. 지연
// 초기화(heap_top==0이면 이 프로세스의 첫 호출) 후, increment>0이면
// heap_mapped_top부터 새 heap_top까지 필요한 페이지를 4KiB 단위로
// 새로 매핑한다(이미 매핑된 페이지는 다시 건드리지 않는다 —
// heap_mapped_top이 "지금까지 실제로 매핑된 경계"를 정확히 추적하는
// 이유). increment==0은 순수 조회 — 아무것도 매핑하지 않고
// out_old_top만 채운다.
process_spawn_error brk(int64_t increment, uint64_t& out_old_top) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }
    kern::object::address_space& space = *self->owner_space;

    if (space.heap_top == 0) {
        space.heap_top = k_heap_user_vaddr;
        space.heap_mapped_top = k_heap_user_vaddr;
    }

    uint64_t old_top = space.heap_top;
    out_old_top = old_top;
    if (increment == 0) {
        return process_spawn_error::ok;
    }
    if (increment < 0) {
        // 힙 축소는 이번 라운드 범위 밖(general-purpose-completion.md
        // §M24 — "익명 페이지를 늘리는 최소 기능이면 충분하다").
        return process_spawn_error::invalid_argument;
    }

    uint64_t new_top = old_top + static_cast<uint64_t>(increment);
    if (new_top < old_top || new_top > k_heap_user_vaddr + k_heap_region_size) {
        // 오버플로 또는 슬롯 예산(1MiB) 초과.
        return process_spawn_error::out_of_memory;
    }

    while (space.heap_mapped_top < new_top) {
        auto page = kern::mm::alloc_pages(0, 0);
        if (!page.is_ok()) {
            return process_spawn_error::out_of_memory;
        }
        auto mapped = map_page(space.page_table_root, space.heap_mapped_top, page.value(),
                                page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            return process_spawn_error::out_of_memory;
        }
        space.heap_mapped_top += kern::mm::k_page_size;
    }

    space.heap_top = new_top;
    return process_spawn_error::ok;
}

}  // namespace arch_x86_64
