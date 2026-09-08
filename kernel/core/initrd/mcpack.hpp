// MCPACK v1 initrd 파서 (docs/spec/boot.md §5, docs/plan/kernel-bootstrap.md
// M8). tools/mkinitrd.py가 만드는 포맷을 그대로 읽는다 — 서드파티
// tar/cpio 파서를 쓰지 않는다는 ADR-006 그대로, 이 파일이 유일한 구현.
// arch 독립이다(그냥 바이트 배열을 읽을 뿐 페이지테이블/CR3를 모른다,
// ADR-002) — kernel/core에 둔다.
#pragma once

#include <cstddef>
#include <cstdint>

#include <libk/result.hpp>

namespace initrd {

enum class mcpack_error : uint32_t {
    bad_magic,
    bad_version,
    truncated,
    not_found,
};

struct entry_span {
    const uint8_t* data;
    uint64_t size;
};

// image가 가리키는 image_size바이트를 MCPACK v1 헤더로 해석해, name과
// 일치하는 엔트리의 데이터를 반환한다(boot.md §5의 mcpack_header/
// mcpack_entry 그대로). image는 이미 커널이 역참조 가능한 포인터여야
// 한다(물리주소가 아니라 가상주소 — 호출자가 mm::phys_to_virt 등으로
// 이미 변환해 둔 것을 넘긴다).
result<entry_span, mcpack_error> find_entry(const uint8_t* image, uint64_t image_size,
                                             const char* name);

}  // namespace initrd
