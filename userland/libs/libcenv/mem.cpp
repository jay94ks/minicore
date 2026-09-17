#include "mem.h"

// minicore/libs/libkenv/mem.cpp(커널 쪽)와 완전히 동일한 구현 - 유저
// 프리스탠딩 실행 파일은 그 라이브러리를 링크할 수 없어(커널 전용
// 빌드 트리) 여기서 그대로 복제한다. 최적화/성능은 나중 문제 - v1은
// 바이트 단위 단순 구현으로 정확성만 확보.

extern "C" void* memset(void* dest, int value, size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto v = static_cast<unsigned char>(value);
    for (size_t i = 0; i < count; ++i) {
        d[i] = v;
    }
    return dest;
}

extern "C" void* memcpy(void* dest, const void* src, size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

extern "C" void* memmove(void* dest, const void* src, size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
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

extern "C" int memcmp(const void* lhs, const void* rhs, size_t count) {
    const auto* a = static_cast<const unsigned char*>(lhs);
    const auto* b = static_cast<const unsigned char*>(rhs);
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return static_cast<int>(a[i]) - static_cast<int>(b[i]);
        }
    }
    return 0;
}
