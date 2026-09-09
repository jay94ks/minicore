// cpio(newc) 읽기전용 파서 (docs/plan/system-servers-bringup.md §M12,
// ADR-131 §결정4). initrun이 부트 디바이스(virtio-blk)를 마운트한
// 뒤 그 안의 서비스 바이너리/기동 파일을 찾는 유일한 용도 —
// M14~M16의 "진짜" virtio-blk/devmgr 구현과는 의도적으로 완전히
// 별개다(ADR-131 §근거: 부트스트랩 1회성 전용, 에러 복구·동시성을
// 신경 쓸 필요가 없어 범용 구현과 요구사항이 다르다).
//
// newc 포맷 개요(각 아카이브 엔트리): "070701" 매직(6바이트) + 13개
// 필드(각 8자리 ASCII 16진수, 총 104바이트) + 파일명(c_namesize
// 바이트, NUL 종료 포함) + 4바이트 경계로 패딩 + 파일 데이터
// (c_filesize 바이트) + 4바이트 경계로 패딩. 이름이 "TRAILER!!!"인
// 엔트리가 끝을 뜻한다(그 자체는 절대 반환/순회되지 않는다).
#pragma once

#include <cstdint>

#include <libk/result.hpp>

namespace cpio {

enum class cpio_error : uint32_t {
    bad_magic,
    truncated,
    not_found,
};

struct entry_span {
    const uint8_t* data;
    uint64_t size;
};

// image[0..image_size)에서 name과 정확히 일치하는 엔트리 하나를
// 찾는다(순차 선형 스캔 — 이 아카이브는 한 번 훑고 버리는 용도라
// 인덱싱이 필요 없다).
result<entry_span, cpio_error> find_entry(const uint8_t* image, uint64_t image_size,
                                           const char* name);

// fn(ctx, name, data, size)를 아카이브에 기록된 순서 그대로, 각
// 엔트리마다(TRAILER 제외) 호출한다. "lib/*.ini를 파일명순으로
// 나열"하는 실행 순서 규칙(ADR-131 §결정5)은 이 아카이브를 만드는
// tools/mkbootdisk.py가 이미 정렬해 써 두는 것으로 지켜지므로, 이
// 함수는 그냥 기록된 순서를 그대로 돌려주기만 하면 충분하다 —
// name은 NUL 종료 문자열이지만 fn 호출이 끝나면 더 이상 유효하지
// 않다고 가정한다(다음 엔트리 파싱이 같은 버퍼를 계속 읽어 나갈
// 뿐, 별도로 보존해 두지 않는다).
using visit_fn = void (*)(void* ctx, const char* name, const uint8_t* data, uint64_t size);
result<void, cpio_error> for_each_entry(const uint8_t* image, uint64_t image_size, visit_fn fn,
                                         void* ctx);

}  // namespace cpio
