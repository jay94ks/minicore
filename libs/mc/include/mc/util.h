// libmc/include/mc/util.h — 이 커널의 모든 서버가 이미 각자 복제해
// 쓰던 최소 바이트 유틸(cstr_len/pack_bytes/bytes_equal)을 libmc
// 클라이언트에서도 재사용할 수 있게 한 곳에 둔다. 가변 길이
// __builtin_memcpy/memset은 이 freestanding 빌드에서 실제 libc
// 심볼 호출로 낮춰져 링크에 실패한다(servers/vfs/main.cpp 등과 같은
// 이유) — 전부 손으로 쓴 바이트 루프다.
#pragma once

#include <stdint.h>

static inline uint64_t mc_cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static inline void mc_pack_bytes(void* dst, uint64_t dst_bytes, const char* data, uint64_t len) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < dst_bytes; ++i) {
        d[i] = (i < len) ? (uint8_t)data[i] : 0;
    }
}

static inline int mc_bytes_equal(const void* a, const void* b, uint64_t len) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (uint64_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return 0;
        }
    }
    return 1;
}

static inline void mc_zero_bytes(void* dst, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; ++i) {
        d[i] = 0;
    }
}
