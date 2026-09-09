// 최소 INI 파서 구현. ini_parser.hpp 상단 주석 참고.
#include "ini_parser.hpp"

namespace ini {

namespace {

bool is_space(uint8_t c) { return c == ' ' || c == '\t'; }

// [begin, end) 줄에서 앞뒤 공백/개행을 걷어낸 범위를 돌려준다.
void trim(const uint8_t*& begin, const uint8_t*& end) {
    while (begin < end && (is_space(*begin))) {
        ++begin;
    }
    while (end > begin && (is_space(*(end - 1)) || *(end - 1) == '\r')) {
        --end;
    }
}

bool key_matches(const uint8_t* key_begin, const uint8_t* key_end, const char* key) {
    uint64_t i = 0;
    for (; key[i] != '\0'; ++i) {
        if (key_begin + i >= key_end || key_begin[i] != static_cast<uint8_t>(key[i])) {
            return false;
        }
    }
    return key_begin + i == key_end;
}

}  // namespace

result<span<const char>, ini_error> find_value(const uint8_t* data, uint64_t size,
                                                const char* key) {
    uint64_t line_start = 0;
    while (line_start < size) {
        uint64_t line_end = line_start;
        while (line_end < size && data[line_end] != '\n') {
            ++line_end;
        }

        const uint8_t* begin = data + line_start;
        const uint8_t* end = data + line_end;
        trim(begin, end);

        if (begin < end && *begin != ';' && *begin != '#' && *begin != '[') {
            // key=value 줄인지 확인 — '=' 앞까지가 key.
            const uint8_t* eq = begin;
            while (eq < end && *eq != '=') {
                ++eq;
            }
            if (eq < end) {
                const uint8_t* key_begin = begin;
                const uint8_t* key_end = eq;
                trim(key_begin, key_end);
                if (key_matches(key_begin, key_end, key)) {
                    const uint8_t* value_begin = eq + 1;
                    const uint8_t* value_end = end;
                    trim(value_begin, value_end);
                    return result<span<const char>, ini_error>::ok(
                        span<const char>(reinterpret_cast<const char*>(value_begin),
                                          static_cast<size_t>(value_end - value_begin)));
                }
            }
        }

        line_start = line_end + 1;
    }
    return result<span<const char>, ini_error>::err(ini_error::not_found);
}

}  // namespace ini
