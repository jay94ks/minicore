// MCPACK v1 파서 구현. mcpack.hpp 상단 주석 참고.
#include "initrd/mcpack.hpp"

namespace initrd {

namespace {

constexpr uint32_t k_magic = 0x4D43504Bu;  // "MCPK" (boot.md §5)
constexpr uint32_t k_version = 1;
constexpr size_t k_name_len = 60;

struct mcpack_header {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t _pad;
};

struct mcpack_entry {
    char name[k_name_len];
    uint64_t offset;
    uint64_t size;
};

// e.name(최대 k_name_len바이트, NUL로 끝날 수도 안 끝날 수도 있음)이
// NUL 종료 문자열 name과 같은지 본다. __builtin_strncmp를 쓰지 않는
// 이유: 이 freestanding 빌드엔 strncmp 구현이 없어(freestanding_mem.cpp가
// memcpy/memset류만 제공한다) 컴파일러가 라이브러리 호출로 lowering하면
// 링크에 실패한다 — 직접 루프를 돈다.
bool name_matches(const char (&entry_name)[k_name_len], const char* name) {
    for (size_t i = 0; i < k_name_len; ++i) {
        char a = entry_name[i];
        char b = name[i];
        if (a != b) {
            return false;
        }
        if (a == '\0') {
            return true;  // 둘 다 이 지점에서 끝났다 — 일치.
        }
    }
    return true;  // k_name_len바이트 전부 일치(entry_name이 NUL 없이 꽉 찬 경우).
}

}  // namespace

result<entry_span, mcpack_error> find_entry(const uint8_t* image, uint64_t image_size,
                                             const char* name) {
    if (image_size < sizeof(mcpack_header)) {
        return result<entry_span, mcpack_error>::err(mcpack_error::truncated);
    }
    mcpack_header header;
    __builtin_memcpy(&header, image, sizeof(header));

    if (header.magic != k_magic) {
        return result<entry_span, mcpack_error>::err(mcpack_error::bad_magic);
    }
    if (header.version != k_version) {
        return result<entry_span, mcpack_error>::err(mcpack_error::bad_version);
    }

    uint64_t entries_bytes = static_cast<uint64_t>(header.entry_count) * sizeof(mcpack_entry);
    if (image_size < sizeof(mcpack_header) + entries_bytes) {
        return result<entry_span, mcpack_error>::err(mcpack_error::truncated);
    }

    const uint8_t* entries_base = image + sizeof(mcpack_header);
    for (uint32_t i = 0; i < header.entry_count; ++i) {
        mcpack_entry e;
        __builtin_memcpy(&e, entries_base + i * sizeof(mcpack_entry), sizeof(e));

        if (name_matches(e.name, name)) {
            if (image_size < e.offset || image_size - e.offset < e.size) {
                return result<entry_span, mcpack_error>::err(mcpack_error::truncated);
            }
            return result<entry_span, mcpack_error>::ok(entry_span{image + e.offset, e.size});
        }
    }

    return result<entry_span, mcpack_error>::err(mcpack_error::not_found);
}

}  // namespace initrd
