// 최소 INI 파서 (docs/plan/system-servers-bringup.md §M12, ADR-131
// §결정5). initrun이 부트 디바이스의 `lib/NNN-이름.ini` 기동 파일에서
// `exec=`/`args=` 값만 뽑아내는 용도 — 그 외 key=value는 initrun이
// 해석하지 않고 원본 바이트 그대로 대상 서비스에게 넘긴다(ADR-131
// §결정5, "그 서비스가 스스로 자기 섹션을 파싱"). 여러 섹션을 구분할
// 필요가 없다(이 아카이브의 각 .ini 파일은 섹션이 정확히 하나다) —
// 그래서 섹션 이름 자체는 검사하지 않고 그냥 파일 전체에서 key=value
// 줄만 찾는다.
#pragma once

#include <cstdint>

#include <k/result.hpp>
#include <k/span.hpp>

namespace ini {

enum class ini_error : uint32_t {
    not_found,
};

// data[0..size)에서 key와 정확히 일치하는 "key=value" 줄을 찾아 그
// value 부분(앞뒤 공백만 제거, 내부 공백은 보존)을 가리키는 span을
// 반환한다 — 원본 버퍼를 그대로 가리킬 뿐 복사하지 않으므로 NUL로
// 끝나지 않는다. `;`/`#`로 시작하는 줄과 `[섹션]` 줄은 건너뛴다.
result<span<const char>, ini_error> find_value(const uint8_t* data, uint64_t size,
                                                const char* key);

}  // namespace ini
