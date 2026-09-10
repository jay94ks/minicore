// #PF COW 쓰기 폴트 처리 구현. page_fault.hpp 상단 주석 참고.
#include "page_fault.hpp"

#include "page_table.hpp"

#include <klog.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

namespace kern::arch::x86_64 {

namespace {

// Intel SDM Vol.3 §4.7 표 4-16 — #PF 에러코드 비트.
constexpr uint64_t k_pf_present = 1ull << 0;
constexpr uint64_t k_pf_write = 1ull << 1;

}  // namespace

bool try_handle_cow_write_fault_for(uint64_t pml4_phys, uint64_t fault_addr, uint64_t error_code) {
    // COW는 "이미 매핑돼 있는 페이지에 쓰려다 권한이 막혀서 난 폴트"만
    // 다룬다 — not-present 폴트(매핑 자체가 없음)나 읽기 폴트는 대상이
    // 아니다.
    if (!(error_code & k_pf_present) || !(error_code & k_pf_write)) {
        return false;
    }

    // kern::mm::k_page_size가 uint32_t라 그냥 ~(k_page_size-1)을 하면 32비트
    // 마스크가 돼(하위 비트 몇 개 세트만) fault_addr의 상위 비트가
    // 전부 잘려 나간다 — 실제로 처음 이 코드를 QEMU로 검증할 때
    // pml4 index 1(1<<39) 가상주소를 다뤄 보고 나서야 이 버그를
    // 발견했다(하위 4GiB 안의 주소만 다뤘다면 우연히 안 드러났을
    // 것이다). uint64_t로 먼저 올려야 한다.
    uint64_t page_virt = fault_addr & ~(static_cast<uint64_t>(kern::mm::k_page_size) - 1);

    page_query_result q = query_page(pml4_phys, page_virt);
    if (!q.present || has_perm(q.perm, page_perm::write) || !has_perm(q.perm, page_perm::cow)) {
        // COW 표시가 없는데 쓰기가 막혀 있다 — 진짜 권한 위반이다.
        return false;
    }

    page_perm final_perm = page_perm::write;
    if (has_perm(q.perm, page_perm::user)) {
        final_perm = final_perm | page_perm::user;
    }
    if (has_perm(q.perm, page_perm::exec)) {
        final_perm = final_perm | page_perm::exec;
    }

    if (kern::mm::frame_release(q.phys)) {
        // 나 말고는 없었다(다른 소유자가 이미 다 떨어져 나갔거나 처음부터
        // 공유되지 않았던 경우) — 복사 없이 같은 프레임에 쓰기 권한만
        // 다시 켠다.
        protect_page(pml4_phys, page_virt, final_perm);
        kern::klog::printf("[pf] cow fast-path (sole owner) virt=0x%lx phys=0x%lx\n",
                     static_cast<unsigned long>(page_virt), static_cast<unsigned long>(q.phys));
        return true;
    }

    // 아직 다른 소유자가 있다 — 새 프레임에 내용을 복사하고 나만 그쪽으로
    // 옮겨간다(frame_release가 이미 원래 프레임의 공유 카운트를 줄여
    // 뒀다).
    auto new_page = kern::mm::alloc_pages(0, 0);
    if (!new_page.is_ok()) {
        kern::klog::printf("[pf] cow copy failed: out of memory virt=0x%lx\n",
                     static_cast<unsigned long>(page_virt));
        return false;
    }
    uint64_t new_phys = new_page.value();
    __builtin_memcpy(kern::mm::phys_to_virt(new_phys), kern::mm::phys_to_virt(q.phys), kern::mm::k_page_size);

    unmap_page(pml4_phys, page_virt);
    map_page(pml4_phys, page_virt, new_phys, final_perm);
    kern::klog::printf("[pf] cow copy virt=0x%lx old_phys=0x%lx new_phys=0x%lx\n",
                 static_cast<unsigned long>(page_virt), static_cast<unsigned long>(q.phys),
                 static_cast<unsigned long>(new_phys));
    return true;
}

bool try_handle_cow_write_fault(uint64_t fault_addr, uint64_t error_code) {
    kern::object::thread* t = kern::sched::current();
    if (t == nullptr || t->owner_space == nullptr) {
        return false;  // 커널 스레드는 COW 대상 주소공간이 없다.
    }
    return try_handle_cow_write_fault_for(t->owner_space->page_table_root, fault_addr, error_code);
}

}  // namespace kern::arch::x86_64
