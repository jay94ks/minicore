#ifndef MINICORE_LIBS_LIBJSON_JSON_H
#define MINICORE_LIBS_LIBJSON_JSON_H

#include <stddef.h>

// libjson: JSON 파서/직렬화(SP-CCACB192, SP-B071E628 §4) - pubreg
// (PN-185406F6)의 tool 등록/조회 프로토콜을 위한 커널/유저 공용
// 라이브러리. libcpio(SP-CCACB192 §1이 인용하는 직접 선례)와 동일한
// 관례 - DOM/트리를 만들지 않고 원본 버퍼를 그 자리에서 훑으며 콜백을
// 부르는 SAX 스타일, 동적 할당 전혀 없음, kernel:: 네임스페이스나
// freestanding 전용 타입 의존 없음.
//
// **zero-copy 문자열**: 이스케이프가 없는 문자열 값/키는 원본 `text`
// 버퍼 안 포인터+길이를 그대로 콜백에 넘긴다(복사 없음). 이스케이프가
//있으면 호출부가 제공한 `scratchBuffer`에 디코딩한 결과를 담아 넘긴다
// (그래서 콜백 안에서 그 문자열을 쓸 때까지만 유효 - 다음 문자열을
// 파싱하면 scratch가 재사용된다).
//
// **[구현 세부 판단, 착수 시 정정]** SP-CCACB192 §6은 "libelf류 매크로
// 게이팅 패턴"을 지시했으나, `libutf8`과 동일한 이유(순수 계산 로직이라
// 커널 전용 API를 전혀 안 씀)로 매크로 게이팅이 필요 없어 `libcpio`와
// 같은 무조건부 스타일을 택했다 - 관찰 가능한 결과(커널/유저 양쪽에서
// 그대로 컴파일)는 원안과 동일.
namespace json {

enum class ErrorCode {
    None,
    UnexpectedCharacter,
    UnterminatedString,
    InvalidEscape,
    InvalidNumber,
    NestingTooDeep,
    ScratchBufferTooSmall,
};

// 재귀 하강 파서의 콜스택 보호용 중첩 상한(SP-CCACB192 §2) - 커널
// 전용 스택(예: kAsyncTaskStackSize=4096) 위에서도 안전하게 동작해야
// 하므로 초과 시 파싱을 중단한다.
constexpr unsigned int kMaxNestingDepth = 32;

// [정정, 착수 중 실측 발견 - QU-6A72AFE6로 SP-CCACB192 §3 재확인 요청]
// 이 커널의 모든 코드는 `-mgeneral-regs-only`로 컴파일된다(freestanding
// 커널 전역 관례, cmake/toolchain-x86_64.cmake) - 이 플래그는 컴파일러가
// SSE/x87 레지스터를 아예 못 쓰게 막아, `double` 산술(리턴값이든 단순
// 덧셈/곱셈이든)이 있는 함수는 **컴파일 자체가 실패한다**("SSE register
// return with SSE disabled"로 직접 확인). `PN-F258698E`(FPU 지연
// 컨텍스트)는 이 문제를 풀어주지 않는다 - 그건 Task 전환 시 FPU
// 레지스터 "상태"를 저장/복원하는 스케줄러 메커니즘이지, 컴파일러가
// 일반 C++ 코드에서 SSE 명령어를 내도 되는지와는 완전히 별개다. 즉
// SP-CCACB192 §3이 확정한 "double까지 지원"은 **이 툴체인 제약상
// 커널 빌드에서 그대로 구현할 수 없다** - 이 라이브러리가 정확히
// "커널/유저 공용"이라 커널 빌드가 실제 제약이 된다.
//
// v1은 그래서 정수가 아닌 숫자(소수점/지수 포함)를 double로 변환하지
// 않고 **원본 텍스트 그대로**(문자열 값과 동일하게 zero-copy) 넘긴다 -
// 실제 double 변환이 필요한 호출부는 이 제약이 없는 유저랜드 코드에서
// 직접 한다. 정수 전용 경로(`isInteger`+`intValue`)는 순수 정수
// 산술이라 이 문제와 무관하게 그대로 지원한다.
struct Callbacks {
    void (*onObjectStart)(void* userData) = nullptr;
    void (*onObjectEnd)(void* userData) = nullptr;
    void (*onArrayStart)(void* userData) = nullptr;
    void (*onArrayEnd)(void* userData) = nullptr;
    void (*onKey)(const char* str, size_t len, void* userData) = nullptr;
    void (*onString)(const char* str, size_t len, void* userData) = nullptr;
    // isInteger==true면 intValue가 원래 텍스트를 정확히 표현한 int64_t
    // 값(오버플로/소수점/지수 없음) - text/textLen도 항상 채워지므로
    // (원본 숫자 텍스트, zero-copy) isInteger==false일 때는 호출부가
    // text/textLen을 직접 파싱(예: 유저랜드에서 strtod류)해야 한다.
    void (*onNumber)(const char* text, size_t textLen, bool isInteger, long long intValue, void* userData) = nullptr;
    void (*onBool)(bool value, void* userData) = nullptr;
    void (*onNull)(void* userData) = nullptr;
    void (*onError)(ErrorCode code, size_t offset, void* userData) = nullptr;
};

// text[0..length)를 파싱하며 콜백을 호출한다. scratchBuffer/
// scratchCapacity는 이스케이프된 문자열을 디코딩할 스크래치 공간(가장
// 긴 이스케이프 문자열 하나를 담을 만큼 커야 함 - 부족하면 그 자리에서
// onError(ScratchBufferTooSmall)로 중단). 파싱이 끝까지 정상 종료되고
// 후행 문자가 공백뿐이면 true, 도중에 실패했으면(onError 호출됨) false.
bool parse(const char* text, size_t length, const Callbacks& callbacks, char* scratchBuffer,
           size_t scratchCapacity, void* userData);

// 동적 할당 없이 호출부가 제공한 고정 버퍼+용량에 append하는 직렬화
// 빌더(SP-CCACB192 §5). 구조적 유효성을 전부 강제하지는 않는다(예:
// key() 없이 바로 endObject() 등 - 최소 스코프, 정상 사용 패턴만
// 지원) - 버퍼 용량 초과만 안전하게 감지한다.
class JsonWriter {
public:
    JsonWriter(char* buffer, size_t capacity);

    bool beginObject();
    bool endObject();
    bool beginArray();
    bool endArray();
    // 반드시 beginObject() 안에서만 호출 - 바로 뒤에 오는 값 하나와
    // 짝을 이룬다(그 사이엔 콤마가 안 붙음).
    bool key(const char* name, size_t nameLen);
    bool value(const char* str, size_t len);
    bool value(long long n);
    // 이미 유효한 JSON number 텍스트(예: 유저랜드에서 직접 포맷한
    // "3.14"/"1e10")를 따옴표 없이 그대로 삽입한다 - `double` 산술은
    // 이 라이브러리 자신이 하지 않는다(json.h 상단 주석의 SSE 제약
    // 참고). 호출부가 문법을 보장해야 한다(이 메서드는 검증하지 않음).
    bool rawNumber(const char* text, size_t len);
    bool value(bool b);
    bool nullValue();

    // 지금까지 쓴 바이트 수.
    size_t length() const { return _length; }
    // 버퍼 용량을 넘는 쓰기 시도가 한 번이라도 있었는지 - 그 이후의
    // 모든 호출은 조용히 실패(false)만 반환하고 버퍼는 더 안 늘어난다.
    bool overflowed() const { return _overflowed; }

private:
    bool kBeforeValue();
    bool kAppendChar(char c);
    bool kAppend(const char* s, size_t n);
    bool kAppendEscapedString(const char* s, size_t n);
    bool kAppendInt(long long n);

    char* _buffer;
    size_t _capacity;
    size_t _length = 0;
    bool _overflowed = false;
    // 현재 열린 컨테이너(object/array)마다 "이미 항목이 하나 있었는지"
    // - 다음 항목 앞에 콤마를 찍을지 결정한다.
    bool _needComma[kMaxNestingDepth] = {};
    unsigned int _depth = 0;
    // key() 직후 true - 바로 뒤따르는 value류 호출 한 번은 콤마/
    // needComma 갱신을 건너뛴다(key: value가 한 항목이므로).
    bool _afterKey = false;
};

}  // namespace json

#endif  // MINICORE_LIBS_LIBJSON_JSON_H
