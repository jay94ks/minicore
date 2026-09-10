// x86_64 페이지테이블 조작 API (docs/plan/kernel-bootstrap.md M4,
// docs/spec/virtual-memory-layout.md). 임의 주소공간(PML4)에 대한
// 매핑/해제/권한 변경 — M1의 paging_setup.cpp는 커널 자신의 higher-half
// 매핑만 부팅 시점에 한 번 구성했지만, 이 API는 이후 마일스톤(M8
// initrun 등)이 새 주소공간을 만들 때 쓸 범용 도구다.
//
// COW(ADR-016)에 대하여: kernel-bootstrap.md M4 시점에는 "COW 복제
// 프리미티브의 자료구조 골격만 준비"했다(페이지 폴트 핸들러도 프레임별
// 참조 카운트 저장소도 없었다). 실제 구현은 system-servers-bringup.md
// §M12(ADR-140, kernel-memory.md)에서 완료했다 — `page_perm::cow`
// 소프트웨어 비트(map_page/protect_page/query_page가 그대로 통과시킴),
// `clone_address_space_cow()`(아래), 그리고 쓰기 폴트 시의 실제 분기는
// page_fault.cpp가 담당한다.
#pragma once

#include <cstdint>

#include <libk/result.hpp>

namespace arch_x86_64 {

enum class map_error : uint32_t {
    out_of_memory,
    already_mapped,
    not_mapped,
};

enum class page_perm : uint32_t {
    none = 0,
    write = 1u << 0,
    exec = 1u << 1,
    user = 1u << 2,

    // M12(system-servers-bringup.md §M12, ADR-016) — 하드웨어가 무시하는
    // PTE 소프트웨어 비트(x86_64 4단계 페이징의 bit 9, Intel SDM Vol.3
    // §4.5의 "ignored" 비트 중 하나)에 그대로 실어 map_page/protect_page/
    // query_page를 그대로 통과하게 만든 표시. write 없이 이 비트만 있는
    // 페이지는 "권한이 없어서 못 쓰는 게 아니라 COW라 복사가 필요할
    // 뿐"이라는 뜻 — page_fault.cpp가 진짜 권한 위반과 구분하는 유일한
    // 근거다. COW 클론(같은 파일::clone_address_space_cow)이 부모/자식
    // 양쪽 PTE에 이 비트를 세우고 kern::mm::frame_add_ref를 부른다.
    cow = 1u << 3,
};

inline page_perm operator|(page_perm a, page_perm b) {
    return static_cast<page_perm>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_perm(page_perm p, page_perm bit) {
    return (static_cast<uint32_t>(p) & static_cast<uint32_t>(bit)) != 0;
}

// 새 PML4를 하나 만들고 커널 higher-half 매핑(physmap + 커널 이미지,
// M1의 pml4[256]/pml4[511])과 저지대 항등 매핑(pml4[0] — GDT가 사는
// .boot 섹션, M8에서 추가: IRETQ가 새 CS/SS 디스크립터를 읽으려면
// GDT 자체가 어느 CR3에서도 접근 가능해야 한다)을 그대로 복사해
// 넣는다 — 모든 주소공간이 이 세 엔트리를 공유해야 CR3를 바꿔도
// 커널 코드/데이터/GDT가 계속 접근 가능하다. 유저 영역(하위 절반의
// 나머지)은 비어 있다.
result<uint64_t, map_error> create_address_space_root();

result<void, map_error> map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                                  page_perm perm);
result<void, map_error> unmap_page(uint64_t pml4_phys, uint64_t virt);
result<void, map_error> protect_page(uint64_t pml4_phys, uint64_t virt, page_perm new_perm);

struct page_query_result {
    bool present;
    uint64_t phys;
    page_perm perm;
};
page_query_result query_page(uint64_t pml4_phys, uint64_t virt);

// M12(ADR-016) — src_pml4_phys의 유저 영역(pml4 index 1~255 — index 0의
// 저지대 GDT 항등 매핑과 256 이상의 커널/physmap은 create_address_space_root와
// 같은 정책으로 그대로 공유한다)에 present인 모든 리프 페이지를 새
// 주소공간에 COW로 복제한다: 양쪽 다 write를 떼고 page_perm::cow를
// 세운 뒤 kern::mm::frame_add_ref(phys)를 한 번 부른다(0→1, "실소유자가
// 이제 둘"이라는 뜻 — page_allocator.hpp의 값 의미 참고). 실제 쓰기
// 시점의 진짜 분기(그대로 쓰기 재개 vs 새 프레임에 복사)는
// page_fault.cpp가 담당한다.
result<uint64_t, map_error> clone_address_space_cow(uint64_t src_pml4_phys);

}  // namespace arch_x86_64
