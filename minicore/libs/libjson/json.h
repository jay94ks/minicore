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

// [정정, 2026-09-17, QU-6A72AFE6 설계자 답변 반영] 착수 당시
// `-mgeneral-regs-only`(SSE와 x87을 둘 다 막는 플래그)라 `double`
// 산술이 있는 함수는 컴파일 자체가 실패했었다 - 설계자가 커널
// 툴체인을 `-mno-mmx -mno-sse -mno-sse2`(+ `-mcmodel=large`)로
// 교체하도록 지시해 해소됐다(cmake/toolchain-x86_64.cmake).
// **단, x86-64 SysV ABI는 `double`을 항상 XMM0 레지스터로 반환하도록
// 고정돼 있어 SSE가 꺼진 상태에서는 "함수가 double을 값으로 반환"하는
// 것 자체가 여전히 불가능하다**(실측 확인 - `-mno-sse`를 켜도 이
// 경우만은 "SSE register return with SSE disabled"로 계속 실패).
// `double`을 **매개변수로 받거나 함수 내부에서 계산하는 것은 전혀
// 문제없다**(x87 스택 명령어로 컴파일됨, 실측 확인) - 그래서 이
// 콜백들처럼 가장 자연스러운 우회는 "반환값 대신 매개변수로 넘기기"
// 다(이 파일 전체가 그 관례를 따른다 - `double`을 리턴하는 함수는
// 이 라이브러리 어디에도 없다).
struct Callbacks {
    void (*onObjectStart)(void* userData) = nullptr;
    void (*onObjectEnd)(void* userData) = nullptr;
    void (*onArrayStart)(void* userData) = nullptr;
    void (*onArrayEnd)(void* userData) = nullptr;
    void (*onKey)(const char* str, size_t len, void* userData) = nullptr;
    void (*onString)(const char* str, size_t len, void* userData) = nullptr;
    // isInteger==true면 intValue가 원래 텍스트를 정확히 표현한 int64_t
    // 값(오버플로/소수점/지수 없음) - isInteger==false일 때는 value가
    // 그 숫자의 double 근사값(직접 계산, SP-CCACB192 §3 원안 그대로).
    // text/textLen은 항상 채워지는 원본 숫자 텍스트(zero-copy) - 호출부가
    // 원한다면 완전한 정밀도가 필요할 때 직접 재파싱할 수 있게 남겨 둔다.
    void (*onNumber)(const char* text, size_t textLen, bool isInteger, long long intValue, double value,
                      void* userData) = nullptr;
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
    bool value(double n);
    // 이미 유효한 JSON number 텍스트(예: 특정 소수 자릿수/지수 표기를
    // 직접 제어하고 싶을 때)를 따옴표 없이 그대로 삽입한다 - 호출부가
    // 문법을 보장해야 한다(이 메서드는 검증하지 않음).
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
    // `double`을 매개변수로만 받고 절대 반환하지 않는다(위 클래스
    // 상단 주석의 ABI 제약 참고).
    bool kAppendDouble(double n);

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
