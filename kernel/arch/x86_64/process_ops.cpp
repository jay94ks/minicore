// sys_process_spawn/sys_fork/sys_exec 구현. process_ops.hpp 상단 주석
// 참고.
#include "process_ops.hpp"

#include "elf_loader.hpp"
#include "page_table.hpp"

#include <new>

#include <klog.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <mm/slab.hpp>
#include <object/handle_table.hpp>
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

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
    object::address_space* space = nullptr;
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

process_spawn_error build_process(const uint8_t* elf_data, uint64_t elf_size,
                                   const uint8_t* argv_blob, uint64_t argv_size, bool trusted,
                                   built_process& out) {
    if (argv_size > mm::k_page_size) {
        return process_spawn_error::invalid_argument;
    }

    auto root = create_address_space_root();
    if (!root.is_ok()) {
        return process_spawn_error::out_of_memory;
    }
    uint64_t pml4_phys = root.value();

    void* space_mem = mm::slab_alloc(sizeof(object::address_space));
    if (space_mem == nullptr) {
        return process_spawn_error::out_of_memory;
    }
    auto* space = new (space_mem) object::address_space();
    space->trusted = trusted;
    space->page_table_root = pml4_phys;

    auto load_result = load_elf(pml4_phys, elf_data, elf_size);
    if (!load_result.is_ok()) {
        mm::slab_free(space, sizeof(object::address_space));
        return process_spawn_error::elf_load_failed;
    }

    for (uint32_t i = 0; i < k_user_stack_pages; ++i) {
        auto page = mm::alloc_pages(0, 0);
        if (!page.is_ok()) {
            mm::slab_free(space, sizeof(object::address_space));
            return process_spawn_error::out_of_memory;
        }
        uint64_t vaddr = k_user_stack_top - (k_user_stack_pages - i) * mm::k_page_size;
        auto mapped =
            map_page(pml4_phys, vaddr, page.value(), page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            mm::slab_free(space, sizeof(object::address_space));
            return process_spawn_error::out_of_memory;
        }
    }

    uint64_t arg0 = 0;
    if (argv_size > 0 && argv_blob != nullptr) {
        auto argv_page = mm::alloc_pages(0, 0);
        if (!argv_page.is_ok()) {
            mm::slab_free(space, sizeof(object::address_space));
            return process_spawn_error::out_of_memory;
        }
        void* argv_virt = mm::phys_to_virt(argv_page.value());
        __builtin_memset(argv_virt, 0, mm::k_page_size);
        __builtin_memcpy(argv_virt, argv_blob, argv_size);
        auto argv_mapped = map_page(pml4_phys, k_argv_user_vaddr, argv_page.value(),
                                     page_perm::user);  // 읽기전용(write 비트 없음).
        if (!argv_mapped.is_ok()) {
            mm::slab_free(space, sizeof(object::address_space));
            return process_spawn_error::out_of_memory;
        }
        arg0 = k_argv_user_vaddr;
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
                                   bool grant_trusted) {
    built_process built;
    auto err = build_process(elf_data, elf_size, argv_blob, argv_size, grant_trusted, built);
    if (err != process_spawn_error::ok) {
        return err;
    }

    object::handle_table* handles = object::create_handle_table();
    if (handles == nullptr) {
        mm::slab_free(built.space, sizeof(object::address_space));
        return process_spawn_error::out_of_memory;
    }

    object::thread* t =
        sched::create_user_thread(built.entry_rip, built.user_rsp, built.arg0, built.space, handles);
    if (t == nullptr) {
        mm::slab_free(built.space, sizeof(object::address_space));
        return process_spawn_error::out_of_memory;
    }

    sched::enqueue(*t);
    klog::printf("[process] spawn ok entry=0x%lx trusted=%u\n",
                 static_cast<unsigned long>(built.entry_rip), grant_trusted);
    return process_spawn_error::ok;
}

uint64_t fork_current(uint64_t saved_user_rip, uint64_t saved_user_rflags,
                       uint64_t saved_user_rsp, uint64_t saved_rbx, uint64_t saved_rbp,
                       uint64_t saved_r12, uint64_t saved_r13, uint64_t saved_r14,
                       uint64_t saved_r15) {
    object::thread* self = sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return static_cast<uint64_t>(process_spawn_error::not_a_user_process);
    }

    auto child_root = clone_address_space_cow(self->owner_space->page_table_root);
    if (!child_root.is_ok()) {
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }

    void* space_mem = mm::slab_alloc(sizeof(object::address_space));
    if (space_mem == nullptr) {
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }
    auto* child_space = new (space_mem) object::address_space();
    child_space->trusted = self->owner_space->trusted;
    child_space->confinement = self->owner_space->confinement;
    child_space->page_table_root = child_root.value();

    // procsrv.md §3.6 — fd 테이블 복제는 procsrv 자신의 몫이다(각
    // 소유 서버에 IPC로 "복제해 달라"고 요청). 커널은 빈 테이블로
    // 시작만 시켜 준다.
    object::handle_table* child_handles = object::create_handle_table();
    if (child_handles == nullptr) {
        mm::slab_free(child_space, sizeof(object::address_space));
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }

    object::thread* child =
        sched::create_forked_thread(saved_user_rip, saved_user_rflags, saved_user_rsp, saved_rbx,
                                     saved_rbp, saved_r12, saved_r13, saved_r14, saved_r15,
                                     child_space, child_handles);
    if (child == nullptr) {
        mm::slab_free(child_space, sizeof(object::address_space));
        return static_cast<uint64_t>(process_spawn_error::out_of_memory);
    }

    sched::enqueue(*child);
    klog::printf("[process] fork ok child_pml4=0x%lx\n",
                 static_cast<unsigned long>(child_root.value()));
    return 1;  // 부모 관점: 성공.
}

process_spawn_error exec_current(const uint8_t* elf_data, uint64_t elf_size,
                                  const uint8_t* argv_blob, uint64_t argv_size) {
    object::thread* self = sched::current();
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

    // 지금 이 스레드로 CR3를 직접 전환한다 — 다음 sched::yield/block
    // 없이 곧바로 새 이미지로 뛰어들 것이므로, 스케줄러의 일반
    // next_pml4_phys() 경로(다음 context switch 시점에만 전환)를 거치지
    // 않는다.
    asm volatile("mov %0, %%cr3" : : "r"(built.space->page_table_root) : "memory");

    klog::printf("[process] exec ok entry=0x%lx\n", static_cast<unsigned long>(built.entry_rip));
    enter_usermode(built.entry_rip, built.user_rsp, built.arg0);
}

process_spawn_error alloc_dma_buffer(uint32_t order, uint64_t& out_virt_addr,
                                      uint64_t& out_phys_addr) {
    object::thread* self = sched::current();
    if (self == nullptr || self->owner_space == nullptr) {
        return process_spawn_error::not_a_user_process;
    }
    if (!self->owner_space->trusted) {
        return process_spawn_error::not_a_user_process;
    }
    if (order > mm::k_max_order) {
        return process_spawn_error::invalid_argument;
    }

    auto page = mm::alloc_pages(order, 0);
    if (!page.is_ok()) {
        return process_spawn_error::out_of_memory;
    }
    uint64_t phys = page.value();
    uint64_t size = static_cast<uint64_t>(mm::k_page_size) << order;

    for (uint64_t off = 0; off < size; off += mm::k_page_size) {
        auto mapped = map_page(self->owner_space->page_table_root, k_dma_buffer_user_vaddr + off,
                                phys + off, page_perm::write | page_perm::user);
        if (!mapped.is_ok()) {
            mm::free_pages(phys, order);
            return process_spawn_error::out_of_memory;
        }
    }

    out_virt_addr = k_dma_buffer_user_vaddr;
    out_phys_addr = phys;
    return process_spawn_error::ok;
}

}  // namespace arch_x86_64
