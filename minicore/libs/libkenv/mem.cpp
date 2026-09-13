#include "mem.h"

// 최적화/성능은 나중 문제 - v1은 바이트 단위 단순 구현으로 정확성만
// 확보한다(byte-wise, 프리스탠딩이라 <string.h> 대체용).

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
