// x86_64 페이지테이블 조작 구현. page_table.hpp 상단 주석 참고.
#include "page_table.hpp"

#include "memory_layout.hpp"
#include "smp.hpp"

#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>

extern "C" {
extern uint64_t pml4[512];      // boot.S가 .boot.bss에 정의한 커널 부트 PML4(물리주소로 직접 접근).
extern uint64_t low_pdpt[512];  // boot.S — pml4[0]이 가리키는 PDPT(물리주소로 직접 접근).
extern uint64_t low_pd[512];    // boot.S — low_pdpt[0]이 가리키는 PD. entry 0~3만 실제로
                                 // 채워져 있다(각 2MiB 거대 페이지, 합쳐서 [0,8MiB) 항등 매핑).
}

namespace arch_x86_64 {

namespace {

constexpr uint64_t k_pte_present = 1ull << 0;
constexpr uint64_t k_pte_writable = 1ull << 1;
constexpr uint64_t k_pte_user = 1ull << 2;
// M12(ADR-016) — 소프트웨어 전용(하드웨어가 무시하는 bit 9). page_perm::cow 참고.
constexpr uint64_t k_pte_cow = 1ull << 9;
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
    if (has_perm(perm, page_perm::cow)) {
        flags |= k_pte_cow;
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
    //
    // M12(ADR-145) — 그렇다고 `table[0] = pml4[0]`처럼 pml4[0] **전체**
    // (low_pdpt 전체, 곧 [0,512GiB) 전체)를 공유해서는 안 된다.
    // boot.S는 low_pd의 앞 4개 엔트리(2MiB 거대 페이지 × 4 = 8MiB)만
    // 채워 뒀을 뿐, low_pdpt/low_pd 나머지 엔트리는 전부 비어 있는
    // "같은 물리 테이블"이다 — pml4[0]을 그대로 공유하면 그 빈
    // 엔트리들도 함께 공유되어, load_elf가 서로 다른 주소공간에서
    // 저지대 가상주소(예: initrun의 링크 주소 0x10000000, 여전히
    // pml4 index 0에 속한다)에 매핑할 때마다 **같은 물리 테이블**을
    // 채우게 된다 — 한 프로세스가 먼저 채운 엔트리를 다른 프로세스가
    // 그대로 보고 `already_mapped`로 충돌한다(sys_process_spawn으로
    // initrun의 두 번째 사본을 만들 때 실제로 재현 확인함). 그래서
    // 이 주소공간 전용 low_pdpt/low_pd를 새로 만들고, GDT가 실제로
    // 필요로 하는 [0,8MiB) 몫(low_pd[0..3])만 그 **내용**(물리
    // 프레임을 가리키는 거대 페이지 엔트리 값 자체)을 복사해 공유를
    // 유지한 채, 나머지(low_pd[4..511], low_pdpt[1..511])는 이
    // 주소공간만의 것으로 비워 둔다 — 그래야 pml4 index 0에 속하는
    // 나머지 저지대 전체(8MiB~512GiB)가 주소공간마다 독립적이다.
    constexpr uint32_t k_low_ident_pd_entries = 4;  // boot.S가 실제로 채운 [0,8MiB) 몫.

    auto low_pdpt_page = mm::alloc_pages(0, 0);
    if (!low_pdpt_page.is_ok()) {
        return result<uint64_t, map_error>::err(map_error::out_of_memory);
    }
    auto low_pd_page = mm::alloc_pages(0, 0);
    if (!low_pd_page.is_ok()) {
        return result<uint64_t, map_error>::err(map_error::out_of_memory);
    }
    uint64_t new_low_pdpt_phys = low_pdpt_page.value();
    uint64_t new_low_pd_phys = low_pd_page.value();
    uint64_t* new_low_pdpt = table_virt(new_low_pdpt_phys);
    uint64_t* new_low_pd = table_virt(new_low_pd_phys);
    __builtin_memset(new_low_pdpt, 0, mm::k_page_size);
    __builtin_memset(new_low_pd, 0, mm::k_page_size);
    for (uint32_t i = 0; i < k_low_ident_pd_entries; ++i) {
        new_low_pd[i] = low_pd[i];
    }
    new_low_pdpt[0] = new_low_pd_phys | k_pte_present | k_pte_writable | k_pte_user;
    table[0] = new_low_pdpt_phys | k_pte_present | k_pte_writable | k_pte_user;

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

    // M10(ADR-055) — 매핑이 바뀔 때마다 즉시 IPI 브로드캐스트. 이
    // 매핑은 방금 새로 생겼으니 다른 코어 TLB에 stale 엔트리가 있을 수는
    // 없지만(존재하지 않던 가상주소), 계획 §M10은 map_page도 명시적으로
    // 포함한다 — 나중에 같은 물리 프레임이 재사용될 때의 잠재적 위험을
    // 없애는 일관된 정책으로 유지한다.
    broadcast_tlb_shootdown(virt);
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

    // M10(ADR-055) — 이 주소공간이 실제로 여러 코어에서 활성화됐는지와
    // 무관하게(M1~M9와 같은 이유로 page_table.hpp 상단 주석은 여전히
    // 유효하다), 모든 매핑 변경마다 무조건 IPI를 브로드캐스트한다 —
    // "그 매핑을 볼 수 있는 다른 코어들"을 개별적으로 추적하지 않는
    // 단순한 v1 정책(계획 §M10)이다. AP가 없으면(기본) 이 호출은
    // 즉시 반환한다.
    broadcast_tlb_shootdown(virt);
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

    broadcast_tlb_shootdown(virt);  // M10(ADR-055) — unmap_page과 같은 정책.
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
    if (entry & k_pte_cow) {
        perm = perm | page_perm::cow;
    }
    return page_query_result{true, entry & k_pte_addr_mask, perm};
}

}  // namespace arch_x86_64

// M13(system-servers-bringup.md §M13) — kernel/core/ipc가 서로 다른
// 유저 주소공간에 있는 두 스레드 사이에서 IPC 메시지 구조체 자체를
// (label/regs/page_count/handle_count) 안전하게 주고받으려면, "이
// vaddr이 어느 물리 프레임에 매핑돼 있는가"를 arch별로 물어야 한다 —
// ADR-002(core는 arch 헤더를 include하지 않는다)를 지키면서 이 질문을
// 던지는 유일한 방법이 이 extern "C" 훅이다(core/sched/scheduler.cpp의
// arch_context_switch 등 기존 훅들과 같은 패턴, kernel-scheduler.md
// 참고) — core(kernel/core/ipc/endpoint.cpp)가 이 시그니처만 알고
// 직접 정의는 여기(arch)가 담당한다. query_page()를 그대로 감쌀 뿐이다.
extern "C" bool arch_translate_user_page(uint64_t page_table_root, uint64_t vaddr,
                                          uint64_t* out_phys) {
    auto q = arch_x86_64::query_page(page_table_root, vaddr);
    if (!q.present) {
        return false;
    }
    *out_phys = q.phys;
    return true;
}

// ADR-159/161(kernel-ipc-objects.md, OPEN-61 해소) — IPC pages[]를
// 유저 프로세스 수신자의 고정 슬롯(k_ipc_mapped_pages_user_vaddr)에
// 매핑/해제하는 훅. arch_translate_user_page와 같은 이유(ADR-002)로
// core(kernel/core/ipc/endpoint.cpp)가 시그니처만 알고 정의는 여기가
// 담당한다. map_page/unmap_page를 그대로 감쌀 뿐이다 — 프레임
// 참조 카운트(frame_add_ref/frame_release)는 core/mm 쪽이라 core가
// 직접 다룬다(여긴 페이지테이블 조작만).
extern "C" bool arch_map_ipc_page_readonly(uint64_t page_table_root, uint64_t vaddr,
                                            uint64_t phys) {
    auto mapped = arch_x86_64::map_page(page_table_root, vaddr, phys, arch_x86_64::page_perm::user);
    return mapped.is_ok();
}

extern "C" bool arch_unmap_ipc_page(uint64_t page_table_root, uint64_t vaddr) {
    auto unmapped = arch_x86_64::unmap_page(page_table_root, vaddr);
    return unmapped.is_ok();
}

namespace arch_x86_64 {

result<uint64_t, map_error> clone_address_space_cow(uint64_t src_pml4_phys) {
    auto new_root = create_address_space_root();
    if (!new_root.is_ok()) {
        return new_root;
    }
    uint64_t dst_pml4_phys = new_root.value();

    // 유저 영역(index 0~255) — 256 이상(커널/physmap)은
    // create_address_space_root가 이미 원본과 동일하게 채워 뒀다(모든
    // 주소공간이 공유). index<256이라 virt의 bit 47은 항상 0 — 별도
    // 부호 확장이 필요 없다.
    //
    // M12(ADR-145) — index 0도 반드시 순회해야 한다. index 0 전체가
    // "공유"였던 옛 설계와 달리, 이제 index 0 안에서도 [0,8MiB)(GDT,
    // low_pd[0..3])만 create_address_space_root가 공유해 두고 나머지
    // ([8MiB, 512GiB) — initrun 같은 유저 코드의 실제 링크 주소
    // 0x10000000이 여기 속한다)는 주소공간마다 독립적이다. 그
    // "나머지"에 있는 부모의 매핑(예: initrun 자신의 코드/데이터)도
    // COW로 복제해야 자식이 실제로 실행 가능하다 — 이걸 빠뜨리면
    // 자식이 sysret 직후 코드 페이지 자체가 없어 명령어 페치
    // 자체가 #PF(not-present)로 죽는다(실제로 이 버그 그대로
    // 재현했다). [0,8MiB) 몫만 건너뛴다(i4==0 && i3==0 && i2<4).
    constexpr uint32_t k_low_ident_pdpt_index = 0;
    constexpr uint32_t k_low_ident_pd_entries = 4;

    uint64_t* src_pml4 = table_virt(src_pml4_phys);
    for (uint32_t i4 = 0; i4 < 256; ++i4) {
        if (!(src_pml4[i4] & k_pte_present)) {
            continue;
        }
        uint64_t* src_pdpt = table_virt(src_pml4[i4] & k_pte_addr_mask);
        for (uint32_t i3 = 0; i3 < 512; ++i3) {
            if (!(src_pdpt[i3] & k_pte_present)) {
                continue;
            }
            uint64_t* src_pd = table_virt(src_pdpt[i3] & k_pte_addr_mask);
            uint32_t i2_start =
                (i4 == 0 && i3 == k_low_ident_pdpt_index) ? k_low_ident_pd_entries : 0;
            for (uint32_t i2 = i2_start; i2 < 512; ++i2) {
                if (!(src_pd[i2] & k_pte_present)) {
                    continue;
                }
                uint64_t* src_pt = table_virt(src_pd[i2] & k_pte_addr_mask);
                for (uint32_t i1 = 0; i1 < 512; ++i1) {
                    uint64_t entry = src_pt[i1];
                    if (!(entry & k_pte_present)) {
                        continue;
                    }
                    uint64_t phys = entry & k_pte_addr_mask;
                    uint64_t virt = (static_cast<uint64_t>(i4) << 39) |
                                    (static_cast<uint64_t>(i3) << 30) |
                                    (static_cast<uint64_t>(i2) << 21) |
                                    (static_cast<uint64_t>(i1) << 12);

                    page_perm cow_perm = page_perm::cow;
                    if (entry & k_pte_user) {
                        cow_perm = cow_perm | page_perm::user;
                    }
                    if (!(entry & k_pte_no_execute)) {
                        cow_perm = cow_perm | page_perm::exec;
                    }
                    // write는 일부러 빼 둔다 — COW의 핵심(page_fault.cpp가
                    // 쓰기 폴트에서 실제 분기를 담당).

                    src_pt[i1] = phys | leaf_flags(cow_perm);  // 부모도 다시 내려감.
                    broadcast_tlb_shootdown(virt);

                    auto map_ok = map_page(dst_pml4_phys, virt, phys, cow_perm);
                    if (!map_ok.is_ok()) {
                        return result<uint64_t, map_error>::err(map_ok.error());
                    }

                    mm::frame_add_ref(phys);
                }
            }
        }
    }

    return result<uint64_t, map_error>::ok(dst_pml4_phys);
}

}  // namespace arch_x86_64
