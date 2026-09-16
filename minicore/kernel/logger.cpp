#include "logger.h"

#include <stdarg.h>

#include "serial.h"

namespace {

kernel::LogLevel gMinLevel = kernel::LogLevel::Info;  // §4 답변 - 기본값 Info
kernel::LoggingDriver* gDrivers[2] = {nullptr, nullptr};

kernel::uint32_t kStrLen(const char* s) {
    kernel::uint32_t n = 0;
    while (s[n]) {
        ++n;
    }
    return n;
}

void kAppendChar(char* buf, kernel::uint32_t bufSize, kernel::uint32_t& pos, char c) {
    // bufSize-1자리까지만 채운다 - 항상 buf[pos] == '\0'로 끝맺을 수
    // 있는 자리 하나를 남겨 둔다(초과분은 조용히 잘림, freestanding
    // vsnprintf류의 표준 동작과 동일한 절충 - 힙이 없어 재할당 불가).
    if (pos + 1 < bufSize) {
        buf[pos++] = c;
    }
}

void kAppendStr(char* buf, kernel::uint32_t bufSize, kernel::uint32_t& pos, const char* s) {
    while (*s) {
        kAppendChar(buf, bufSize, pos, *s++);
    }
}

void kAppendUnsigned(char* buf, kernel::uint32_t bufSize, kernel::uint32_t& pos, kernel::uint64_t value,
                      kernel::uint32_t base, bool uppercase, kernel::uint32_t width, bool zeroPad) {
    constexpr char kLower[] = "0123456789abcdef";
    constexpr char kUpper[] = "0123456789ABCDEF";
    const char* table = uppercase ? kUpper : kLower;
    char digits[24];  // 64비트 값의 2진 표현도 넉넉히 담는 여유
    kernel::uint32_t n = 0;
    if (value == 0) {
        digits[n++] = '0';
    }
    while (value) {
        digits[n++] = table[value % base];
        value /= base;
    }
    for (kernel::uint32_t i = n; i < width; ++i) {
        kAppendChar(buf, bufSize, pos, zeroPad ? '0' : ' ');
    }
    while (n) {
        kAppendChar(buf, bufSize, pos, digits[--n]);
    }
}

void kAppendSigned(char* buf, kernel::uint32_t bufSize, kernel::uint32_t& pos, kernel::int64_t value,
                    kernel::uint32_t width, bool zeroPad) {
    const bool neg = value < 0;
    // INT64_MIN을 그대로 음수화하면 UB - 비트패턴 반전+1(2의 보수
    // 정의 그대로)로 안전하게 크기를 구한다.
    const kernel::uint64_t mag = neg ? (~static_cast<kernel::uint64_t>(value) + 1) : static_cast<kernel::uint64_t>(value);
    char digits[24];
    kernel::uint32_t n = 0;
    kernel::uint64_t m = mag;
    if (m == 0) {
        digits[n++] = '0';
    }
    while (m) {
        digits[n++] = static_cast<char>('0' + (m % 10));
        m /= 10;
    }
    const kernel::uint32_t contentLen = n + (neg ? 1 : 0);
    const kernel::uint32_t padLen = (width > contentLen) ? (width - contentLen) : 0;
    if (zeroPad) {
        if (neg) {
            kAppendChar(buf, bufSize, pos, '-');
        }
        for (kernel::uint32_t i = 0; i < padLen; ++i) {
            kAppendChar(buf, bufSize, pos, '0');
        }
    } else {
        for (kernel::uint32_t i = 0; i < padLen; ++i) {
            kAppendChar(buf, bufSize, pos, ' ');
        }
        if (neg) {
            kAppendChar(buf, bufSize, pos, '-');
        }
    }
    while (n) {
        kAppendChar(buf, bufSize, pos, digits[--n]);
    }
}

// fmt를 args로 채워 buf[pos..]에 이어붙인다(freestanding 전체 지원
// 포맷터, SP-DF89897F §3.3) - %s/%c/%d/%i/%u/%x/%X/%%, 폭/0-패딩,
// l/ll 길이 변경자(64비트) 지원. 표준 vsnprintf 없이 새로 구현 -
// libc/STL 미사용, 힙 할당 없음(전부 호출부가 넘긴 고정 버퍼 안에서).
void kFormatInto(char* buf, kernel::uint32_t bufSize, kernel::uint32_t& pos, const char* fmt, va_list args) {
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            kAppendChar(buf, bufSize, pos, *p++);
            continue;
        }
        ++p;  // '%' 소비

        bool zeroPad = false;
        while (*p == '0') {
            zeroPad = true;
            ++p;
        }

        kernel::uint32_t width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + static_cast<kernel::uint32_t>(*p - '0');
            ++p;
        }

        int lengthMod = 0;  // 0=int/unsigned int, 1=long, 2=long long
        if (*p == 'l') {
            ++p;
            lengthMod = 1;
            if (*p == 'l') {
                ++p;
                lengthMod = 2;
            }
        }

        const char conv = *p ? *p++ : '\0';
        switch (conv) {
            case 'd':
            case 'i': {
                kernel::int64_t v;
                if (lengthMod == 2) {
                    v = va_arg(args, long long);
                } else if (lengthMod == 1) {
                    v = va_arg(args, long);
                } else {
                    v = va_arg(args, int);
                }
                kAppendSigned(buf, bufSize, pos, v, width, zeroPad);
                break;
            }
            case 'u': {
                kernel::uint64_t v;
                if (lengthMod == 2) {
                    v = va_arg(args, unsigned long long);
                } else if (lengthMod == 1) {
                    v = va_arg(args, unsigned long);
                } else {
                    v = va_arg(args, unsigned int);
                }
                kAppendUnsigned(buf, bufSize, pos, v, 10, false, width, zeroPad);
                break;
            }
            case 'x':
            case 'X': {
                kernel::uint64_t v;
                if (lengthMod == 2) {
                    v = va_arg(args, unsigned long long);
                } else if (lengthMod == 1) {
                    v = va_arg(args, unsigned long);
                } else {
                    v = va_arg(args, unsigned int);
                }
                kAppendUnsigned(buf, bufSize, pos, v, 16, conv == 'X', width, zeroPad);
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) {
                    s = "(null)";
                }
                const kernel::uint32_t len = kStrLen(s);
                for (kernel::uint32_t i = len; i < width; ++i) {
                    kAppendChar(buf, bufSize, pos, ' ');
                }
                kAppendStr(buf, bufSize, pos, s);
                break;
            }
            case 'c': {
                const char c = static_cast<char>(va_arg(args, int));
                kAppendChar(buf, bufSize, pos, c);
                break;
            }
            case '%': {
                kAppendChar(buf, bufSize, pos, '%');
                break;
            }
            case '\0': {
                break;  // 끝에서 잘린 '%' - 조용히 무시
            }
            default: {
                // 모르는 지시자 - 조용히 삼키지 않고 그대로 남겨 눈에
                // 띄게 한다(디버깅 편의 - RM-23F4B687 §4 취지).
                kAppendChar(buf, bufSize, pos, '%');
                kAppendChar(buf, bufSize, pos, conv);
                break;
            }
        }
    }
}

const char* kLevelPrefix(kernel::LogLevel level) {
    switch (level) {
        case kernel::LogLevel::Verbose:
            return "[V] ";
        case kernel::LogLevel::Info:
            return "[I] ";
        case kernel::LogLevel::Warn:
            return "[W] ";
        case kernel::LogLevel::Error:
            return "[E] ";
        case kernel::LogLevel::Fatal:
            return "[F] ";
        case kernel::LogLevel::Panic:
            return "[PANIC] ";
    }
    return "[?] ";
}

void kLogImpl(kernel::LogLevel level, const char* fmt, va_list args) {
    if (level < gMinLevel && level != kernel::LogLevel::Fatal && level != kernel::LogLevel::Panic) {
        return;  // Fatal/Panic은 필터 무시(§3.2/§4 확정)
    }

    constexpr kernel::uint32_t kBufSize = 256;  // §3.3 - 스택 고정 버퍼, 힙 없음
    char buf[kBufSize];
    kernel::uint32_t pos = 0;
    kAppendStr(buf, kBufSize, pos, kLevelPrefix(level));
    kFormatInto(buf, kBufSize, pos, fmt, args);
    if (pos == 0 || buf[pos - 1] != '\n') {
        kAppendChar(buf, kBufSize, pos, '\n');
    }
    buf[pos] = '\0';

    for (kernel::LoggingDriver* driver : gDrivers) {
        if (driver) {
            driver->writeLine(buf);
        }
    }
}

kernel::SerialLoggingDriver gSerialDriver;

}  // namespace

namespace kernel {

void SerialLoggingDriver::writeLine(const char* line) {
    // Serial::write() 자신이 이미 한 줄 전체를 Spinlock으로 감싼다
    // (serial.cpp의 gWriteLock) - 이 한 번의 호출로 목표 5(SMP 안전성,
    // 코어 간 줄 단위 비섞임)가 그대로 만족된다. 별도 락 불필요.
    Serial::write(line);
}

void Logger::init() {
    gDrivers[0] = &gSerialDriver;
}

void Logger::setMinLevel(LogLevel level) {
    gMinLevel = level;
}

void Logger::setDriver(unsigned int slot, LoggingDriver* driver) {
    if (slot < 2) {
        gDrivers[slot] = driver;
    }
}

void Logger::verbose(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kLogImpl(LogLevel::Verbose, fmt, args);
    va_end(args);
}

void Logger::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kLogImpl(LogLevel::Info, fmt, args);
    va_end(args);
}

void Logger::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kLogImpl(LogLevel::Warn, fmt, args);
    va_end(args);
}

void Logger::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kLogImpl(LogLevel::Error, fmt, args);
    va_end(args);
}

void Logger::fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kLogImpl(LogLevel::Fatal, fmt, args);
    va_end(args);
}

void Logger::panic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kLogImpl(LogLevel::Panic, fmt, args);
    va_end(args);
}

}  // namespace kernel
