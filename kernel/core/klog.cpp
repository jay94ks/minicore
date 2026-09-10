// kern::klog::write/printf — arch 독립 구현. kern::klog::putc(arch 구현)만 호출한다
// (ADR-002 HAL 경계, docs/spec/debug-console.md §3).
//
// 여러 코어가 동시에 로그를 남길 수 있으므로 전역 스핀락 1개로
// 직렬화한다 — debug-console.md §3이 명시한 ADR-033 "필요한 곳만
// 최소 락" 원칙의 예외. 인터럽트 핸들러(향후 M9+)도 로그를 남길 수
// 있으므로 irq_safe로 감싼다.
#include "klog.hpp"

#include <cstdint>

#include <k/irq_safe.hpp>
#include <k/spinlock.hpp>

namespace kern::klog {

namespace {

irq_safe<spinlock> g_log_lock;

void print_unsigned(uint64_t value, uint32_t base, bool uppercase) {
    constexpr size_t k_max_digits = 20;  // uint64_t max in base 10 = 20자리
    char digits[k_max_digits];
    size_t count = 0;

    const char* alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (value == 0) {
        putc('0');
        return;
    }

    while (value != 0 && count < k_max_digits) {
        digits[count++] = alphabet[value % base];
        value /= base;
    }

    while (count > 0) {
        putc(digits[--count]);
    }
}

void print_signed(int64_t value) {
    if (value < 0) {
        putc('-');
        // INT64_MIN 절댓값은 uint64_t 범위에서만 표현 가능하므로 부호 있는
        // 연산으로 뒤집지 않는다.
        print_unsigned(static_cast<uint64_t>(-(value + 1)) + 1, 10, false);
        return;
    }
    print_unsigned(static_cast<uint64_t>(value), 10, false);
}

}  // namespace

void write(const char* s, size_t len) {
    scoped_lock<irq_safe<spinlock>> guard(g_log_lock);
    for (size_t i = 0; i < len; ++i) {
        putc(s[i]);
    }
}

void vprintf(const char* fmt, va_list args) {
    scoped_lock<irq_safe<spinlock>> guard(g_log_lock);
    for (const char* p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            putc(*p);
            continue;
        }

        ++p;

        // 'l' 길이 수식어(%ld/%lu/%lx) — 64비트 값(물리주소, 페이지테이블
        // 엔트리 등)을 다루는 boot_info 덤프(M2)부터 필요해졌다. long은
        // 이 타깃(SysV x86-64)에서 64비트다.
        bool is_long = false;
        if (*p == 'l') {
            is_long = true;
            ++p;
        }

        switch (*p) {
            case 'd': {
                int64_t value = is_long ? va_arg(args, long) : va_arg(args, int);
                print_signed(value);
                break;
            }
            case 'u': {
                uint64_t value =
                    is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                print_unsigned(value, 10, false);
                break;
            }
            case 'x': {
                uint64_t value =
                    is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                print_unsigned(value, 16, false);
                break;
            }
            case 'p': {
                auto value = reinterpret_cast<uintptr_t>(va_arg(args, void*));
                putc('0');
                putc('x');
                print_unsigned(value, 16, false);
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (s == nullptr) {
                    s = "(null)";
                }
                while (*s != '\0') {
                    putc(*s++);
                }
                break;
            }
            case 'c': {
                // va_arg의 char는 int로 승격되어 전달된다.
                putc(static_cast<char>(va_arg(args, int)));
                break;
            }
            case '%': {
                putc('%');
                break;
            }
            case '\0': {
                // 형식 문자열이 '%'(l)로 끝나는 비정상 입력 — 그대로 종료.
                return;
            }
            default: {
                // 알 수 없는 지정자는 그대로 출력해 디버깅을 돕는다.
                putc('%');
                if (is_long) {
                    putc('l');
                }
                putc(*p);
                break;
            }
        }
    }
}

void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

}  // namespace kern::klog
