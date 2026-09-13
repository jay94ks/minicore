#ifndef MINICORE_LIBS_LIBKENV_TYPES_H
#define MINICORE_LIBS_LIBKENV_TYPES_H

// libkenv: freestanding 환경엔 <cstdint> 같은 표준 헤더가 없어서(호스트
// libc에 의존), 표준 정수/부동소수점 자료형을 kernel 네임스페이스
// 안에 직접 정의해 둔다(QU-148B7262, 설계자 확정, 2026-09-14 - "정수형,
// 실수형, 문자열 등을 freestanding에서도 사용 할 수 있도록 수동으로
// kernel namespace에 정의"). 이름은 표준 <cstdint> 관례를 그대로 따라
// snake_case로 둔다 - RM-23F4B687 §1의 "타입은 PascalCase" 규칙의
// 의도적 예외다(mem.h의 memcpy/memset 등과 같은 이유 - 표준 이름
// 자체가 관례이자 요구사항).
//
// "문자열" 지원(std::string 대응)은 이 답변에 구체적인 형태가 없어
// 별도 설계 질의(QU-19B76E06)로 등록했고, 답변에 따라 string.h의
// kernel::string(참조 카운터 외부 주입형 뷰 타입)으로 구현했다.

namespace kernel {

using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long;

using int8_t = signed char;
using int16_t = short;
using int32_t = int;
using int64_t = long;

using float32_t = float;
using float64_t = double;

using size_t = decltype(sizeof(0));
using ptrdiff_t = decltype(static_cast<int*>(nullptr) - static_cast<int*>(nullptr));

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_TYPES_H
