// cpio(newc) 파서 구현. cpio_reader.hpp 상단 주석 참고.
#include "cpio_reader.hpp"

namespace cpio {

namespace {

constexpr uint64_t k_header_fixed_size = 110;  // "070701"(6) + 13개 필드 × 8자리 16진수.
constexpr uint64_t k_namesize_field_offset = 94;  // 매직(6) + 필드 11개(11*8=88) 뒤.
constexpr uint64_t k_filesize_field_offset = 54;  // 매직(6) + 필드 6개(6*8=48) 뒤.

uint64_t align_up4(uint64_t v) { return (v + 3) & ~static_cast<uint64_t>(3); }

// 8자리 ASCII 16진수(대소문자 무관) 하나를 uint32_t로 변환한다.
// 이 파서가 신뢰하지 않는 외부(디스크) 데이터이므로, 16진수가 아닌
// 문자가 섞여 있으면 truncated로 취급한다(호출자가 처리).
result<uint32_t, cpio_error> parse_hex8(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        uint8_t c = p[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return result<uint32_t, cpio_error>::err(cpio_error::truncated);
        }
        v = (v << 4) | digit;
    }
    return result<uint32_t, cpio_error>::ok(v);
}

bool name_equals(const uint8_t* name, uint64_t namesize_with_nul, const char* target) {
    uint64_t i = 0;
    for (; target[i] != '\0'; ++i) {
        if (i >= namesize_with_nul || name[i] != static_cast<uint8_t>(target[i])) {
            return false;
        }
    }
    return i < namesize_with_nul && name[i] == 0;
}

}  // namespace

result<void, cpio_error> for_each_entry(const uint8_t* image, uint64_t image_size, visit_fn fn,
                                         void* ctx) {
    uint64_t offset = 0;
    for (;;) {
        if (offset > image_size || image_size - offset < k_header_fixed_size) {
            return result<void, cpio_error>::err(cpio_error::truncated);
        }
        const uint8_t* header = image + offset;
        if (header[0] != '0' || header[1] != '7' || header[2] != '0' || header[3] != '7' ||
            header[4] != '0' || header[5] != '1') {
            return result<void, cpio_error>::err(cpio_error::bad_magic);
        }

        auto filesize_r = parse_hex8(header + k_filesize_field_offset);
        auto namesize_r = parse_hex8(header + k_namesize_field_offset);
        if (!filesize_r.is_ok() || !namesize_r.is_ok()) {
            return result<void, cpio_error>::err(cpio_error::truncated);
        }
        uint64_t filesize = filesize_r.value();
        uint64_t namesize = namesize_r.value();  // NUL 종료 포함.

        uint64_t name_start = offset + k_header_fixed_size;
        if (image_size - name_start < namesize) {
            return result<void, cpio_error>::err(cpio_error::truncated);
        }
        const uint8_t* name = image + name_start;

        uint64_t data_start = align_up4(name_start + namesize);
        if (data_start > image_size || image_size - data_start < filesize) {
            return result<void, cpio_error>::err(cpio_error::truncated);
        }
        const uint8_t* data = image + data_start;

        bool is_trailer = name_equals(name, namesize, "TRAILER!!!");
        if (is_trailer) {
            return result<void, cpio_error>::ok();
        }

        fn(ctx, reinterpret_cast<const char*>(name), data, filesize);

        offset = align_up4(data_start + filesize);
    }
}

namespace {

struct find_ctx {
    const char* target;
    entry_span result{nullptr, 0};
    bool found = false;
};

void find_visit(void* ctx_raw, const char* name, const uint8_t* data, uint64_t size) {
    auto* ctx = static_cast<find_ctx*>(ctx_raw);
    if (ctx->found) {
        return;
    }
    // name_equals가 필요로 하는 namesize_with_nul을 여기서는 모르므로
    // (visit_fn은 그 값을 넘기지 않는다), 이름 비교는 그냥 NUL 종료
    // 문자열 비교로 다시 한다 — for_each_entry가 이미 이름을 NUL
    // 종료 상태로 넘겨준다는 것만 알면 충분하다.
    const char* a = name;
    const char* b = ctx->target;
    while (*a != '\0' && *b != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    if (*a == '\0' && *b == '\0') {
        ctx->result = entry_span{data, size};
        ctx->found = true;
    }
}

}  // namespace

result<entry_span, cpio_error> find_entry(const uint8_t* image, uint64_t image_size,
                                           const char* name) {
    find_ctx ctx;
    ctx.target = name;
    auto walked = for_each_entry(image, image_size, &find_visit, &ctx);
    if (!walked.is_ok()) {
        return result<entry_span, cpio_error>::err(walked.error());
    }
    if (!ctx.found) {
        return result<entry_span, cpio_error>::err(cpio_error::not_found);
    }
    return result<entry_span, cpio_error>::ok(ctx.result);
}

}  // namespace cpio
