// memset/memcpy/memmove/memcmp — 유저랜드(initrun/servers) 전용 제공.
// (docs/design/foundations.md ADR-200 실행 중 발견 — kernel/core/
// freestanding_mem.cpp와 같은 이유: 컴파일러가 구조체 zero-init
// (`T{}`)이나 큰 복사에 대해 -ffreestanding이어도 이 심벌 호출을
// 낼 수 있어, libc가 없는 이 빌드에서는 직접 정의해야 한다.
//
// ADR-200으로 kernel/include/uapi.hpp의 구조체(message/
// process_spawn_request 등)가 mc/syscall.h의 순수 C 구조체
// (`mc_message` 등, 필드별 기본값이 없는 진짜 aggregate)로 흡수되면서
// 처음으로 이 문제가 유저랜드에서 실제로 드러났다 — 예전 `uapi::message`
// 는 필드마다 `= 0` 기본 멤버 초기화자가 있어 비트리비얼 생성자를
// 가졌고, `{}`로 값 초기화하면 그 생성자 본문(필드별 저장)으로
// 컴파일됐다. 순수 C 구조체는 트리비얼 타입이라 `{}`가 "통째로
// 0으로 채워라"로 인식되어, 이 구조체들이 충분히 커지자(수십~백
// 바이트대) 컴파일러가 인라인 저장 대신 `memset` 호출로 낮췄다 —
// initrun 링크가 `undefined symbol: memset`으로 즉시 드러냈다.
//
// kernel/core/freestanding_mem.cpp와 똑같이 -fno-builtin으로
// 컴파일된다(libs/mc/CMakeLists.txt) — 그렇지 않으면 아래 루프가
// 다시 memcpy/memset 호출로 최적화돼 무한 재귀가 된다. 커널은
// 자신의 freestanding_mem.cpp가 이미 있어 이 파일을 링크하지
// 않는다(중복 심벌 방지 — 이 파일은 오직 `minicore_libmc`를 링크하는
// 유저랜드 실행파일만 가져간다).
#include <stddef.h>

void* memset(void* dest, int value, size_t count) {
    unsigned char* d = (unsigned char*)dest;
    unsigned char v = (unsigned char)value;
    for (size_t i = 0; i < count; ++i) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || count == 0) {
        return dest;
    }
    if (d < s) {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t count) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < count; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}
