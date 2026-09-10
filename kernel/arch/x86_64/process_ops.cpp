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

#include <mc/syscall.h>

// usermode.S(M8) — 유저모드로 직접 진입한다(IRETQ). sys_exec가 성공
// 경로에서 곧바로 이걸 부른다(create_user_thread류의 "다음에 스케줄될
// 때" 방식이 아니라, 지금 이 스레드가 바로 새 이미지로 뛰어드는
// 것이므로).
extern "C" [[noreturn]] void enter_usermode(uint64_t rip, uint64_t rsp, uint64_t arg0);

namespace kern::arch::x86_64 {

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

// M30(real-libc-syscall-layer.md §M30) — sys_mmap_anon 전용 영역
// (ADR-160 슬롯 6, 위 슬롯 5 heap 바로 다음). sys_brk의 heap_top과
// 절대 공유하지 않는 이유는 kernel_objects.hpp::address_space::
// mmap_top 주석 참고.
constexpr uint64_t k_mmap_user_vaddr = 0x0000700000600000ull;
constexpr uint64_t k_mmap_region_size = 0x400000ull;  // 4MiB.

// M29(real-libc-syscall-layer.md §M29) — 인터프리터를 올릴 고정
// 베이스. 주 프로그램 베이스(0x10000000, build_process 상단
// INITRUN_BASE류 상수와 같은 계열)와 유저 스택(0x700000000000...)
// 사이 충분히 떨어진 자리 — musl의 ld-musl-x86_64.so.1은 수십~
// 수백 KiB뿐이라 이 간격이면 충분하다.
constexpr uint64_t k_interp_base = 0x0000000020000000ull;

process_spawn_error build_process(const uint8_t* elf_data, uint64_t elf_size,
                                   const uint8_t* argv_blob, uint64_t argv_size, bool trusted,
                                   bool linux_abi_stack, const uint8_t* interp_data,
                                   uint64_t interp_size, built_process& out) {
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

    // M29 — 인터프리터가 있으면 별도 베이스에 추가로 적재한다(ET_DYN,
    // load_elf()의 새 load_bias 인자 — elf_loader.hpp 참고). 실패해도
    // 주 프로그램 로드 자체는 이미 끝났으므로 여기서만 정리한다.
    bool has_interp = (interp_data != nullptr && interp_size > 0);
    uint64_t interp_entry = 0;
    if (has_interp) {
        auto interp_result = load_elf(pml4_phys, interp_data, interp_size, k_interp_base);
        if (!interp_result.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::elf_load_failed;
        }
        interp_entry = interp_result.value();
    }

    uint64_t user_rsp_value = k_user_stack_top;
    uint64_t top_stack_page_phys = 0;
    for (uint32_t i = 0; i < k_user_stack_pages; ++i) {
        auto page = kern::mm::alloc_pages(0, 0);
        if (!page.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
        if (i == k_user_stack_pages - 1) {
            top_stack_page_phys = page.value();  // 아래 linux_abi_stack용.
        }
        uint64_t vaddr = k_user_stack_top - (k_user_stack_pages - i) * kern::mm::k_page_size;
        auto mapped =
            map_page(pml4_phys, vaddr, page.value(), page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            kern::mm::slab_free(space, sizeof(kern::object::address_space));
            return process_spawn_error::out_of_memory;
        }
    }

    // M28(real-libc-syscall-layer.md §M28, ADR-183) — musl의 crt_arch.h
    // (`_start`)는 %rsp를 그대로 읽어 argc부터 해석한다(Linux ABI 초기
    // 스택 관례) — 이 커널의 기존 유저 스레드(initrun/서버들)는 전혀
    // 이 관례를 쓰지 않고 arg0(레지스터 하나)만 쓰므로, 이 레이아웃은
    // linux_abi_stack이 실제로 요청될 때만(musl 링크 프로그램) 유저
    // 스택 최상단에 덧써진다. 이번 라운드는 고정된 최소값만 채운다
    // (argv[0] 하나, envp 없음, __init_libc/__init_tls가 크래시 없이
    // 지나가는 데 필요한 auxv만) — 실제 인자 전달/일반 auxv 확장은
    // M29(동적 링킹, PT_INTERP 지원 시 AT_PHDR 등 추가)에서 다룬다.
    if (linux_abi_stack) {
        struct stack_layout {
            uint64_t argc;
            uint64_t argv0_ptr;
            uint64_t argv_null;
            uint64_t envp_null;
            uint64_t auxv[12][2];
            char argv0_str[32];
        };
        static_assert(sizeof(stack_layout) <= 512, "linux_abi_stack 레이아웃이 예약 공간을 넘는다");

        constexpr uint64_t k_layout_offset = kern::mm::k_page_size - 512;
        uint64_t layout_vaddr = k_user_stack_top - kern::mm::k_page_size + k_layout_offset;

        // top_stack_page_phys는 유저 쪽엔 (k_user_stack_top-k_page_size)에
        // 매핑돼 있다 — phys_to_virt는 같은 물리 페이지의 커널 자신의
        // 항등 매핑 뷰이므로, 여기서 그 뷰로 써 넣은 바이트가 그대로
        // 유저 쪽 layout_vaddr에서 읽힌다(argv_virt와 같은 관례, 위
        // 참고).
        auto* l = reinterpret_cast<stack_layout*>(
            static_cast<uint8_t*>(kern::mm::phys_to_virt(top_stack_page_phys)) + k_layout_offset);
        __builtin_memset(l, 0, sizeof(*l));
        constexpr const char k_argv0[] = "/bin/musl-hello";
        __builtin_memcpy(l->argv0_str, k_argv0, sizeof(k_argv0));

        l->argc = 1;
        l->argv0_ptr = layout_vaddr + __builtin_offsetof(stack_layout, argv0_str);
        l->argv_null = 0;
        l->envp_null = 0;
        // AT_PAGESZ=6, AT_UID=11, AT_EUID=12, AT_GID=13, AT_EGID=14,
        // AT_SECURE=23, AT_PHDR=3, AT_PHENT=4, AT_PHNUM=5, AT_ENTRY=9,
        // AT_BASE=7, AT_NULL=0(마지막) — musl/include/elf.h와 정확히
        // 같은 값. AT_UID==AT_EUID && AT_GID==AT_EGID && !AT_SECURE가
        // 전부 성립해야 __init_libc가 poll() 기반 stdio 보안 검사를
        // 건너뛴다(이 커널엔 SYS_poll이 없다) — 전부 0으로 둬서 이
        // 조건을 항상 만족시킨다.
        l->auxv[0][0] = 6;
        l->auxv[0][1] = kern::mm::k_page_size;
        l->auxv[1][0] = 11;
        l->auxv[1][1] = 0;
        l->auxv[2][0] = 12;
        l->auxv[2][1] = 0;
        l->auxv[3][0] = 13;
        l->auxv[3][1] = 0;
        l->auxv[4][0] = 14;
        l->auxv[4][1] = 0;
        l->auxv[5][0] = 23;
        l->auxv[5][1] = 0;

        if (has_interp) {
            // M29 — PT_INTERP 경로: AT_PHDR/AT_PHENT/AT_PHNUM은 **주
            // 프로그램**의 것(ld.so가 이미 커널이 매핑해 둔 주 프로그램의
            // phdr을 이 값으로 찾아간다), AT_ENTRY는 주 프로그램의 진짜
            // 진입점(load_result.value(), bias=0이라 그대로), AT_BASE는
            // 인터프리터의 로드 바이어스다. 표준 ELF64 헤더 레이아웃
            // (e_phoff@32, e_phentsize@54, e_phnum@56)을 직접 읽는다 —
            // elf_loader.cpp의 elf64_ehdr은 그 파일의 익명 네임스페이스
            // 안에만 있어 여기서 재사용할 수 없다(중복이 아니라 그
            // 파일의 로컬 세부로 남기는 편이 낫다는 판단, ADR-002와
            // 같은 최소 헤더 의존 정신).
            uint64_t e_phoff = 0;
            uint16_t e_phentsize = 0;
            uint16_t e_phnum = 0;
            __builtin_memcpy(&e_phoff, elf_data + 32, sizeof(e_phoff));
            __builtin_memcpy(&e_phentsize, elf_data + 54, sizeof(e_phentsize));
            __builtin_memcpy(&e_phnum, elf_data + 56, sizeof(e_phnum));

            l->auxv[6][0] = 3;  // AT_PHDR.
            l->auxv[6][1] = e_phoff;  // 주 프로그램 베이스(bias=0)라 파일 오프셋=가상주소.
            l->auxv[7][0] = 4;  // AT_PHENT.
            l->auxv[7][1] = e_phentsize;
            l->auxv[8][0] = 5;  // AT_PHNUM.
            l->auxv[8][1] = e_phnum;
            l->auxv[9][0] = 9;  // AT_ENTRY — 주 프로그램의 진짜 진입점.
            l->auxv[9][1] = load_result.value();
            l->auxv[10][0] = 7;  // AT_BASE — 인터프리터의 로드 바이어스.
            l->auxv[10][1] = k_interp_base;
            // auxv[11]은 memset으로 이미 (0,0) — AT_NULL 종료.
        } else {
            l->auxv[6][0] = 3;
            l->auxv[6][1] = 0;
            l->auxv[7][0] = 5;
            l->auxv[7][1] = 0;
            l->auxv[8][0] = 0;
            l->auxv[8][1] = 0;
        }

        user_rsp_value = layout_vaddr;
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

    // M12(mc/syscall.h::MC_M12_SELF_INFO_USER_VADDR 주석) — kernel_main.cpp::
    // setup_initrun_process가 initrun의 최초 스폰에서만 하던 "원본 ELF
    // 바이트를 새 주소공간에도 복사해 self_info로 알려 준다"를 여기
    // build_process()로 일반화한다 — process_spawn/exec_current로 만드는
    // **모든** 프로세스가 다 이걸 받는다. procsrv도 initrun의
    // sys_process_spawn으로 만들어지는 이상 이 경로를 그대로 타므로,
    // initrun과 똑같은 방식(MC_M12_SELF_INFO_USER_VADDR을 읽어
    // sys_fork+sys_exec)으로 "자기 자신을 fork/exec"할 수 있다 — M12
    // QEMU 목표(procsrv 자기 자신 fork/exec)가 요구하는 조건이 이것뿐.
    // ADR-160(kernel-memory.md, 슬롯 0의 예산 검증) — self_elf 복사가
    // 이웃 슬롯(self_info, MC_M12_SELF_INFO_USER_VADDR)을 침범하기
    // 전에 명시적으로 거부한다. ADR-149가 겪은 버그(간격을 넘은 ELF가
    // already_mapped로만 우회 발견됨)를 재발 방지한다.
    if (elf_size > MC_M12_SELF_INFO_USER_VADDR - MC_M12_SELF_ELF_USER_VADDR) {
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
        auto mapped = map_page(pml4_phys, MC_M12_SELF_ELF_USER_VADDR + offset, page.value(),
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
    auto* self_info = static_cast<mc_m12_self_info*>(kern::mm::phys_to_virt(info_page.value()));
    self_info->elf_addr = MC_M12_SELF_ELF_USER_VADDR;
    self_info->elf_size = elf_size;
    auto info_mapped = map_page(pml4_phys, MC_M12_SELF_INFO_USER_VADDR, info_page.value(),
                                 page_perm::user);
    if (!info_mapped.is_ok()) {
        kern::mm::slab_free(space, sizeof(kern::object::address_space));
        return process_spawn_error::out_of_memory;
    }

    out.space = space;
    // M29 — 인터프리터가 있으면 진짜 진입점은 인터프리터의 것이다(주
    // 프로그램은 매핑만 해 두고, ld.so가 auxv의 AT_ENTRY로 나중에
    // 직접 찾아간다 — 위 has_interp 블록 참고).
    out.entry_rip = has_interp ? interp_entry : load_result.value();
    out.user_rsp = user_rsp_value;
    out.arg0 = arg0;
    return process_spawn_error::ok;
}

}  // namespace

process_spawn_error process_spawn(const uint8_t* elf_data, uint64_t elf_size,
                                   const uint8_t* argv_blob, uint64_t argv_size,
                                   bool grant_trusted, bool create_endpoint,
                                   const mc_handle_transfer* inherited_handles,
                                   uint32_t inherited_handle_count,
                                   uint32_t& out_endpoint_proxy_handle,
                                   uint32_t& out_thread_handle, bool linux_abi_stack,
                                   const uint8_t* interp_data, uint64_t interp_size) {
    built_process built;
    auto err = build_process(elf_data, elf_size, argv_blob, argv_size, grant_trusted,
                              linux_abi_stack, interp_data, interp_size, built);
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

    if (inherited_handle_count > MC_MAX_SPAWN_INHERITED_HANDLES) {
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
                       uint64_t saved_r15, uint32_t& out_thread_handle) {
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

    // M32(real-libc-syscall-layer.md §M32) — fs_base(M28, TLS)도 같은
    // 이유로 물려받는다. create_forked_thread()가 만드는 새 thread
    // 객체는 fs_base=0(기본값)으로 시작하는데, 이걸 그대로 두면 musl
    // 프로그램(TLS로 errno/pthread_self를 읽는)의 자식이 fork() 직후
    // (아직 exec()하기 전, 여전히 부모와 같은 이미지를 실행하는 동안)
    // %fs 상대 접근에서 곧바로 페이지 폴트를 일으킨다 — 실제로 겪음
    // (2026-09-10, musl-hello의 fork() 자식이 _Fork.c::__post_Fork의
    // __pthread_self() 호출에서 크래시). fork()는 COW로 주소공간
    // 전체를 복제하므로, 부모의 fs_base가 가리키는 가상주소(TLS
    // 블록)도 자식 쪽에 그대로 유효한 COW 사본으로 존재한다 — 값만
    // 그대로 넘기면 된다(위 io_port_base/count와 완전히 같은 패턴).
    child->fs_base = self->fs_base;

    // M36(real-libc-syscall-layer.md §M36) — process_spawn()의 기존
    // out_thread_handle 자리(ADR-178)와 완전히 같은 패턴. self는
    // 여전히 부모다(이 함수 전체가 부모의 syscall 컨텍스트에서만
    // 실행된다 — 자식은 이 코드를 절대 거치지 않는다, 위 함수
    // 주석 참고).
    if (self->handles != nullptr) {
        auto thread_owner =
            self->handles->create_owner(kern::object::object_kind::thread, kern::object::k_right_can_signal, child);
        if (thread_owner.is_ok()) {
            out_thread_handle = thread_owner.value();
        }
    }

    kern::sched::enqueue(*child);
    kern::klog::printf("[process] fork ok child_pml4=0x%lx\n",
                 static_cast<unsigned long>(child_root.value()));
    return 1;  // 부모 관점: 성공.
}

process_spawn_error exec_current(const uint8_t* elf_data, uint64_t elf_size,
                                  const uint8_t* argv_blob, uint64_t argv_size,
                                  bool linux_abi_stack) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }

    // procsrv.md §4 4단계 — pid/identity/trusted/confinement는 그대로
    // 유지된다. trusted는 기존 address_space에서 그대로 물려받는다
    // (exec는 새 신원 부여가 아니다).
    built_process built;
    // M32(real-libc-syscall-layer.md §M32) — musl execve()가 여기로
    // 오면 linux_abi_stack=true(build_process()의 기존 M28 경로를
    // 그대로 재사용, 인터프리터는 여전히 없음 — ADR-203의 정적
    // 링킹 기준선). procsrv.md §4처럼 신원 유지만 다루는 기존
    // 호출자(M12 self-exec, M18 su-target)는 그대로 false를 넘긴다.
    auto err = build_process(elf_data, elf_size, argv_blob, argv_size,
                              self->owner_space->trusted, linux_abi_stack,
                              /*interp_data=*/nullptr, /*interp_size=*/0, built);
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
    if (size == 0 || size > MC_MAX_MMIO_MAP_BYTES) {
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

// M30(real-libc-syscall-layer.md §M30, ADR-183) — musl 자신의
// mallocng/lite_malloc이 요구하는 SYS_mmap(익명)/SYS_munmap 지원.
// kernel_objects.hpp::address_space::mmap_top 주석 참고 — sys_brk와
// 완전히 분리된 별도 영역(k_mmap_user_vaddr, 4MiB 예산)이다. 매
// 호출마다 요청 크기(페이지 정렬)만큼 새로 매핑해 반환하고, 이전
// 요청과 절대 겹치지 않는다(단순 범프 — 재사용/회수는 munmap이
// 담당하지 않는다, 아래 munmap_anon 참고).
process_spawn_error mmap_anon(uint64_t size, uint64_t& out_vaddr) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }
    kern::object::address_space& space = *self->owner_space;

    if (space.mmap_top == 0) {
        space.mmap_top = k_mmap_user_vaddr;
    }
    if (size == 0) {
        return process_spawn_error::invalid_argument;
    }

    uint64_t aligned_size =
        (size + kern::mm::k_page_size - 1) & ~(kern::mm::k_page_size - 1);
    uint64_t base = space.mmap_top;
    uint64_t new_top = base + aligned_size;
    if (new_top < base || new_top > k_mmap_user_vaddr + k_mmap_region_size) {
        return process_spawn_error::out_of_memory;
    }

    for (uint64_t vaddr = base; vaddr < new_top; vaddr += kern::mm::k_page_size) {
        auto page = kern::mm::alloc_pages(0, 0);
        if (!page.is_ok()) {
            return process_spawn_error::out_of_memory;
        }
        auto mapped =
            map_page(space.page_table_root, vaddr, page.value(), page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            return process_spawn_error::out_of_memory;
        }
    }

    space.mmap_top = new_top;
    out_vaddr = base;
    return process_spawn_error::ok;
}

// **알려진 단순화**(M24의 sys_brk 축소 미지원, M26의 realloc 미보존과
// 같은 정신) — 실제로 페이지를 회수하지 않는다. mmap_anon의 4MiB
// 예산 안에서라면 이 테스트 규모(musl-hello 하나의 malloc 왕복)는
// 계속 누적돼도 소진되지 않는다. 진짜 회수가 필요해지면
// mmap_top/매핑 해제를 별도로 추적해야 한다.
process_spawn_error munmap_anon(uint64_t addr, uint64_t size) {
    (void)addr;
    (void)size;
    return process_spawn_error::ok;
}

}  // namespace kern::arch::x86_64
