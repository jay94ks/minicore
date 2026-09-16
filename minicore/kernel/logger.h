#ifndef MINICORE_KERNEL_LOGGER_H
#define MINICORE_KERNEL_LOGGER_H

#include "libkenv/types.h"

namespace kernel {

// 커널 로깅 인프라 추상화(SP-DF89897F, PN-32696F0F) - 로그 한
// 줄(레벨 프리픽스까지 포맷 완료)을 실제로 어딘가에 내보내는 백엔드.
// v1은 정확히 2개(§3.3 - SerialLoggingDriver/BufferedFileLoggingDriver)
// 만 존재한다 - 임의 확장 가능한 범용 레지스트리가 아니다.
class LoggingDriver {
public:
    virtual ~LoggingDriver() = default;
    virtual void writeLine(const char* line) = 0;
};

// Verbose는 기존 "Debug" 개념을 포함한다(설계자 지시, 별도 Debug
// 레벨을 안 둔다). Fatal/Panic은 레벨 필터와 무관하게 항상 출력.
enum class LogLevel : uint32_t { Verbose, Info, Warn, Error, Fatal, Panic };

// 호출부가 구체적 백엔드를 몰라도 되는 파사드(SP-DF89897F §3.3, `Log`
// 에서 개칭). 부팅 극초반(Slab/힙 준비 전)부터 안전하게 동작해야
// 하므로 전부 정적 인스턴스/스택 버퍼만 쓴다 - 동적 할당 없음.
class Logger {
public:
    // Serial::init() 직후 호출 - 0번 슬롯에 SerialLoggingDriver(정적
    // 인스턴스)를 등록한다.
    static void init();

    // 기본값 Info(§4 답변 - 기존 체감과 동일 유지). Fatal/Panic은 이
    // 필터를 무시하고 항상 출력된다.
    static void setMinLevel(LogLevel level);

    // slot 0/1 두 자리뿐이다(§3.3 - 드라이버는 정확히 2개). 나중에
    // 1번 드라이버(BufferedFileLoggingDriver 등)를 교체/등록하는
    // 용도로도 쓴다.
    static void setDriver(unsigned int slot, LoggingDriver* driver);

    // 전부 freestanding 전체 지원 포맷터(logger.cpp의 kFormatLine) -
    // %s/%c/%d/%i/%u/%x/%X/%%, 폭/0-패딩, l/ll 길이 변경자(64비트)
    // 지원. 내부 버퍼는 고정 크기 스택 배열 - 힙 할당 없음.
    static void verbose(const char* fmt, ...);
    static void info(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void error(const char* fmt, ...);
    static void fatal(const char* fmt, ...);
    // panic() 전용 - 레벨 필터 완전히 무시, 항상 출력(§4 답변 - panic
    // 시에도 포맷팅 인자를 쓸 수 있어야 한다는 지시 반영).
    static void panic(const char* fmt, ...);
};

// 부팅 단계 기본 드라이버(§3.3-1) - kernel::Serial을 직접 감싸는
// 텍스트 드라이버. 부팅 극초반부터 존재, 동적 할당 없음(정적
// 인스턴스). "이걸 대체하는 뭔가가 생기면 그 타이밍에 Logger::
// setDriver()로 교체" - 정확히 무엇으로 언제 대체되는지는 SP-DF89897F
// 범위 밖(미정).
class SerialLoggingDriver : public LoggingDriver {
public:
    void writeLine(const char* line) override;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LOGGER_H
