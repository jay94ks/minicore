// memset/memcpy/memmove/memcmp — 컴파일러가 구조체 zero-init(`T{}`)이나
// 큰 복사에 대해 freestanding 여부와 무관하게 이 심벌들을 호출하는
// 코드를 낼 수 있어(-ffreestanding이어도), libc가 없는 이 커널에서는
// 링크 실패를 막으려면 직접 정의해야 한다. <cstring>은 ADR-010 허용
// 목록에 없으므로 쓰지 않는다.
//
// 이 파일은 -fno-builtin으로 컴파일된다(kernel/CMakeLists.txt) — 그렇지
// 않으면 컴파일러가 아래 루프를 다시 memcpy/memset 호출로 "최적화"해
// 무한 재귀를 만들 수 있다(잘 알려진 freestanding 구현 함정).
//
// M3(libk) 착수 시 정식 위치(libk 또는 별도 컴파일러 지원 라이브러리)로
// 옮기는 것을 검토한다 — 지금은 M2가 boot_info_x86_64.cpp에서 당장
// 필요해 kernel/core에 최소 구현만 둔다.
#include <cstddef>

extern "C" {

void* memset(void* dest, int value, size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    auto v = static_cast<unsigned char>(value);
    for (size_t i = 0; i < count; ++i) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count) {
    auto* d = static_cast<unsigned char*>(dest);
    const auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
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

int memcmp(const void* a, const void* b, size_t count) {
    const auto* pa = static_cast<const unsigned char*>(a);
    const auto* pb = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < count; ++i) {
        if (pa[i] != pb[i]) {
            return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
        }
    }
    return 0;
}

}  // extern "C"
