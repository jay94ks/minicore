#ifndef MINICORE_LIBS_LIBUTF8_UTF8_H
#define MINICORE_LIBS_LIBUTF8_UTF8_H

// libutf8: UTF-8 인코딩/유니코드 유틸(SP-CCACB192 §4/§6, 설계자 지시
// 2026-09-17 - "문자열 관련해서 유니코드 등은 minicore/libs/libutf8처럼
// 별도 라이브러리를 구성하도록 해. 이것들 역시 커널/유저랜드 공용이어야
// 해."). 최초(유일한) 소비자는 libjson의 `\uXXXX` 이스케이프 디코딩
// (코드 포인트 → UTF-8 바이트열, 서로게이트 페어 조합) - libcpio/libelf
// 와 동일하게 kernel:: 네임스페이스나 freestanding 전용 타입에 의존하지
// 않는다(표준 C++ 타입만 사용, RM-23F4B687 §3의 minicore/libs/<name>
// 배치 규칙).
//
// [구현 세부 판단, 착수 시 정정] SP-CCACB192 §4/§6은 "libelf류 매크로
// 게이팅 패턴"(MINICORE_LIBELF_KERNEL처럼 커널 전용 선언만 조건부로
// 노출)을 지시했으나, 실제로 필요한 기능(코드 포인트 인코딩/서로게이트
// 조합)은 순수 계산이라 커널 전용 API(Paging/GenericSlabAllocator 등)를
// 전혀 안 쓴다 - `libelf`가 그 매크로를 쓰는 이유(`loadIntoAddressSpace`
// 처럼 커널에서만 뜻이 있는 함수를 유저 빌드에서 숨기기 위함)에 해당하는
// 커널 전용 부분이 이 라이브러리엔 아예 없다. 그래서 `libcpio`(마찬가지로
// 순수 파서라 매크로가 필요 없었던 선례)와 동일하게 매크로 게이팅 없이
// 모든 함수를 양쪽 빌드에 동일하게 노출한다 - 관찰 가능한 결과(커널/유저
// 양쪽에서 그대로 컴파일)는 원안과 같고, 아무 효과가 없는 빈 매크로를
// 두지 않을 뿐이다(RM-23F4B687 §4 - 과설계 방지).
namespace utf8 {

// UTF-16 서로게이트 페어(high: 0xD800-0xDBFF, low: 0xDC00-0xDFFF)를
// 하나의 코드 포인트(U+10000..U+10FFFF)로 결합한다. 유효한 페어가
// 아니면 -1.
long combineSurrogatePair(unsigned int high, unsigned int low);

// codePoint(유니코드 스칼라 값, 서로게이트 자신은 무효)를 UTF-8
// 바이트열로 인코딩해 out에 쓴다(호출부가 최소 4바이트 공간을 보장).
// 반환값은 실제로 쓴 바이트 수(1~4) - codePoint가 무효(서로게이트
// 범위 0xD800-0xDFFF 단독, 또는 0x10FFFF 초과)면 0(아무것도 안 씀).
unsigned int encode(long codePoint, unsigned char* out);

}  // namespace utf8

#endif  // MINICORE_LIBS_LIBUTF8_UTF8_H
