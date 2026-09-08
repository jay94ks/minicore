// x86_64 페이지테이블 조작 구현. page_table.hpp 상단 주석 참고.
#include "page_table.hpp"

#include "memory_layout.hpp"

#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>

extern "C" {
extern uint64_t pml4[512];  // boot.S가 .boot.bss에 정의한 커널 부트 PML4(물리주소로 직접 접근).
}

namespace arch_x86_64 {

namespace {

constexpr uint64_t k_pte_present = 1ull << 0;
constexpr uint64_t k_pte_writable = 1ull << 1;
constexpr uint64_t k_pte_user = 1ull << 2;
constexpr uint64_t k_pte_no_execute = 1ull << 63;
constexpr uint64_t k_pte_addr_mask = 0x000FFFFFFFFFF000ull;

uint64_t* table_virt(uint64_t table_phys) {
    return static_cast<uint64_t*>(mm::phys_to_virt(table_phys));
}

// table_phys가 가리키는 테이블의 index번 엔트리가 다음 레벨 테이블을
// 가리키게 한다 — 이미 있으면 그 물리주소를 반환하고, 없고 create가
// true면 새로 mm::alloc_pages(order 0)로 만들어 연결한다.
result<uint64_t, map_error> next_level(uint64_t table_phys, uint32_t index, bool create) {
    uint64_t* table = table_virt(table_phys);
    uint64_t entry = table[index];
    if (entry & k_pte_present) {
        return result<uint64_t, map_error>::ok(entry & k_pte_addr_mask);
    }
    if (!create) {
        return result<uint64_t, map_error>::err(map_error::not_mapped);
    }

    auto page = mm::alloc_pages(0, 0);
    if (!page.is_ok()) {
        return result<uint64_t, map_error>::err(map_error::out_of_memory);
    }
    uint64_t new_table_phys = page.value();
    __builtin_memset(table_virt(new_table_phys), 0, mm::k_page_size);
    // 중간 레벨은 항상 WRITABLE|USER로 열어 둔다 — 실제 접근 제어는
    // 리프(PT) 엔트리가 담당한다(x86_64 관례: 상위 레벨의 제한 비트는
    // 하위 레벨과 AND로 결합되므로, 중간에서 미리 좁히면 리프에서
    // 아무리 넓게 열어도 못 열린다).
    table[index] = new_table_phys | k_pte_present | k_pte_writable | k_pte_user;
    return result<uint64_t, map_error>::ok(new_table_phys);
}

uint64_t leaf_flags(page_perm perm) {
    uint64_t flags = k_pte_present;
    if (has_perm(perm, page_perm::write)) {
        flags |= k_pte_writable;
    }
    if (has_perm(perm, page_perm::user)) {
        flags |= k_pte_user;
    }
    if (!has_perm(perm, page_perm::exec)) {
        flags |= k_pte_no_execute;
    }
    return flags;
}

struct virt_indices {
    uint32_t pml4;
    uint32_t pdpt;
    uint32_t pd;
    uint32_t pt;
};

virt_indices split(uint64_t virt) {
    return virt_indices{
        static_cast<uint32_t>((virt >> 39) & 0x1FFull),
        static_cast<uint32_t>((virt >> 30) & 0x1FFull),
        static_cast<uint32_t>((virt >> 21) & 0x1FFull),
        static_cast<uint32_t>((virt >> 12) & 0x1FFull),
    };
}

// [pdpt, pd, pt] 세 레벨을 차례로 얻는다(root부터 pd 테이블까지) —
// map_page/unmap_page/protect_page/query_page가 공유하는 공통 경로.
result<uint64_t, map_error> walk_to_pt(uint64_t pml4_phys, const virt_indices& idx, bool create) {
    auto pdpt = next_level(pml4_phys, idx.pml4, create);
    if (!pdpt.is_ok()) {
        return pdpt;
    }
    auto pd = next_level(pdpt.value(), idx.pdpt, create);
    if (!pd.is_ok()) {
        return pd;
    }
    return next_level(pd.value(), idx.pd, create);
}

}  // namespace

result<uint64_t, map_error> create_address_space_root() {
    auto page = mm::alloc_pages(0, 0);
    if (!page.is_ok()) {
        return result<uint64_t, map_error>::err(map_error::out_of_memory);
    }
    uint64_t new_pml4_phys = page.value();
    uint64_t* table = table_virt(new_pml4_phys);
    __builtin_memset(table, 0, mm::k_page_size);

    constexpr uint32_t k_physmap_pml4_index =
        static_cast<uint32_t>((arch_mm::k_physmap_base >> 39) & 0x1FFull);
    constexpr uint32_t k_kernel_image_pml4_index =
        static_cast<uint32_t>((arch_x86_64::k_kernel_virt_offset >> 39) & 0x1FFull);

    table[k_physmap_pml4_index] = pml4[k_physmap_pml4_index];
    table[k_kernel_image_pml4_index] = pml4[k_kernel_image_pml4_index];

    // pml4[0] — boot.S가 구성한 저지대 항등 매핑(물리 [0, 8MiB)),
    // GDT(gdt64_start)를 포함한 .boot 섹션 전체가 여기 산다(link.ld:
    // .boot는 higher-half가 아니라 VMA==LMA로 낮은 물리주소에 그대로
    // 링크된다). M8에서 이 엔트리를 안 옮기면 CR3가 새 주소공간으로
    // 바뀐 뒤 GDT 자체가 매핑 밖이 되어, IRETQ가 새 CS/SS 디스크립터를
    // 읽으려는 순간 #PF가 난다(실제로 QEMU에서 CR2=GDT 안의 정확한
    // 오프셋으로 재현 확인함) — 모든 주소공간이 이 저지대 매핑도
    // physmap/커널 이미지와 똑같이 공유해야 한다.
    table[0] = pml4[0];

    return result<uint64_t, map_error>::ok(new_pml4_phys);
}

result<void, map_error> map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                                  page_perm perm) {
    virt_indices idx = split(virt);
    auto pt = walk_to_pt(pml4_phys, idx, /*create=*/true);
    if (!pt.is_ok()) {
        return result<void, map_error>::err(pt.error());
    }

    uint64_t* pt_table = table_virt(pt.value());
    if (pt_table[idx.pt] & k_pte_present) {
        return result<void, map_error>::err(map_error::already_mapped);
    }
    pt_table[idx.pt] = (phys & k_pte_addr_mask) | leaf_flags(perm);
    return result<void, map_error>::ok();
}

result<void, map_error> unmap_page(uint64_t pml4_phys, uint64_t virt) {
    virt_indices idx = split(virt);
    auto pt = walk_to_pt(pml4_phys, idx, /*create=*/false);
    if (!pt.is_ok()) {
        return result<void, map_error>::err(map_error::not_mapped);
    }

    uint64_t* pt_table = table_virt(pt.value());
    if (!(pt_table[idx.pt] & k_pte_present)) {
        return result<void, map_error>::err(map_error::not_mapped);
    }
    pt_table[idx.pt] = 0;
    // TLB 무효화(invlpg)는 아직 하지 않는다 — page_table.hpp 상단 주석:
    // M4는 이 주소공간을 실제로 활성화(CR3 전환)하지 않는다.
    return result<void, map_error>::ok();
}

result<void, map_error> protect_page(uint64_t pml4_phys, uint64_t virt, page_perm new_perm) {
    virt_indices idx = split(virt);
    auto pt = walk_to_pt(pml4_phys, idx, /*create=*/false);
    if (!pt.is_ok()) {
        return result<void, map_error>::err(map_error::not_mapped);
    }

    uint64_t* pt_table = table_virt(pt.value());
    if (!(pt_table[idx.pt] & k_pte_present)) {
        return result<void, map_error>::err(map_error::not_mapped);
    }
    uint64_t phys = pt_table[idx.pt] & k_pte_addr_mask;
    pt_table[idx.pt] = phys | leaf_flags(new_perm);
    return result<void, map_error>::ok();
}

page_query_result query_page(uint64_t pml4_phys, uint64_t virt) {
    virt_indices idx = split(virt);
    auto pt = walk_to_pt(pml4_phys, idx, /*create=*/false);
    if (!pt.is_ok()) {
        return page_query_result{false, 0, page_perm::none};
    }

    uint64_t entry = table_virt(pt.value())[idx.pt];
    if (!(entry & k_pte_present)) {
        return page_query_result{false, 0, page_perm::none};
    }

    page_perm perm = page_perm::none;
    if (entry & k_pte_writable) {
        perm = perm | page_perm::write;
    }
    if (entry & k_pte_user) {
        perm = perm | page_perm::user;
    }
    if (!(entry & k_pte_no_execute)) {
        perm = perm | page_perm::exec;
    }
    return page_query_result{true, entry & k_pte_addr_mask, perm};
}

}  // namespace arch_x86_64
