// x86_64 페이지테이블 조작 API (docs/plan/kernel-bootstrap.md M4,
// docs/spec/virtual-memory-layout.md). 임의 주소공간(PML4)에 대한
// 매핑/해제/권한 변경 — M1의 paging_setup.cpp는 커널 자신의 higher-half
// 매핑만 부팅 시점에 한 번 구성했지만, 이 API는 이후 마일스톤(M8
// initrun 등)이 새 주소공간을 만들 때 쓸 범용 도구다.
//
// COW(ADR-016)에 대하여: kernel-bootstrap.md M4는 "COW 복제 프리미티브의
// 자료구조 골격만 준비"를 요구한다. 실제 COW(쓰기 폴트 처리, 프레임
// 참조 카운트)는 페이지 폴트 핸들러(IDT, M4 범위 밖)와 프레임별 참조
// 카운트 저장소(mm이 아직 갖고 있지 않음 — kernel-bootstrap-m3.md 참고)
// 둘 다 필요해 이번에 구현하지 않는다. 이 파일이 제공하는
// map_page/protect_page가 그 프리미티브가 실제로 쓰일 자리다 — 예를
// 들어 자식 주소공간에 같은 물리 페이지를 read-only로 map_page하고
// 부모 쪽도 protect_page로 read-only로 낮추는 식으로 미래의 COW clone이
// 구현될 것이다.
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
};

inline page_perm operator|(page_perm a, page_perm b) {
    return static_cast<page_perm>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_perm(page_perm p, page_perm bit) {
    return (static_cast<uint32_t>(p) & static_cast<uint32_t>(bit)) != 0;
}

// 새 PML4를 하나 만들고 커널 higher-half 매핑(physmap + 커널 이미지,
// M1의 pml4[256]/pml4[511])을 그대로 복사해 넣는다 — 모든 주소공간이
// 이 두 엔트리를 공유해야 CR3를 바꿔도 커널 코드/데이터가 계속
// 접근 가능하다. 유저 영역(하위 절반)은 비어 있다.
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

}  // namespace arch_x86_64
