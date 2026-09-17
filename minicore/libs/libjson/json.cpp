#include "json.h"

#include "libutf8/utf8.h"

namespace {

struct ParserState {
    const char* text;
    size_t length;
    size_t pos;
    const json::Callbacks* cb;
    char* scratch;
    size_t scratchCapacity;
    void* userData;
    unsigned int depth;
    bool failed;
};

void kFail(ParserState& st, json::ErrorCode code) {
    st.failed = true;
    if (st.cb->onError) {
        st.cb->onError(code, st.pos, st.userData);
    }
}

char kPeek(const ParserState& st) { return st.pos < st.length ? st.text[st.pos] : '\0'; }

void kSkipWhitespace(ParserState& st) {
    while (st.pos < st.length) {
        const char c = st.text[st.pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++st.pos;
        } else {
            break;
        }
    }
}

bool kExpect(ParserState& st, char c) {
    if (kPeek(st) != c) {
        kFail(st, json::ErrorCode::UnexpectedCharacter);
        return false;
    }
    ++st.pos;
    return true;
}

int kHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool kParseHex4(ParserState& st, unsigned int* out) {
    if (st.pos + 4 > st.length) {
        kFail(st, json::ErrorCode::InvalidEscape);
        return false;
    }
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
        const int d = kHexDigit(st.text[st.pos + i]);
        if (d < 0) {
            kFail(st, json::ErrorCode::InvalidEscape);
            return false;
        }
        value = (value << 4) | static_cast<unsigned int>(d);
    }
    st.pos += 4;
    *out = value;
    return true;
}

// st.pos가 여는 따옴표를 가리킨 상태로 호출한다 - 성공하면 st.pos를
// 닫는 따옴표 다음으로 옮기고 *outStr/*outLen을 채운다(이스케이프
// 없으면 원본 zero-copy, 있으면 st.scratch에 디코딩).
bool kParseStringRaw(ParserState& st, const char** outStr, size_t* outLen) {
    if (!kExpect(st, '"')) {
        return false;
    }
    const size_t start = st.pos;
    bool hasEscape = false;
    size_t scan = st.pos;
    while (scan < st.length && st.text[scan] != '"') {
        if (st.text[scan] == '\\') {
            hasEscape = true;
            ++scan;
            if (scan >= st.length) {
                break;
            }
        }
        ++scan;
    }
    if (scan >= st.length) {
        st.pos = scan;
        kFail(st, json::ErrorCode::UnterminatedString);
        return false;
    }

    if (!hasEscape) {
        *outStr = st.text + start;
        *outLen = scan - start;
        st.pos = scan + 1;  // 닫는 따옴표까지 소비
        return true;
    }

    size_t outPos = 0;
    st.pos = start;
    while (st.pos < st.length && st.text[st.pos] != '"') {
        const char c = st.text[st.pos];
        if (c != '\\') {
            if (outPos >= st.scratchCapacity) {
                kFail(st, json::ErrorCode::ScratchBufferTooSmall);
                return false;
            }
            st.scratch[outPos++] = c;
            ++st.pos;
            continue;
        }
        ++st.pos;  // consume backslash
        if (st.pos >= st.length) {
            kFail(st, json::ErrorCode::InvalidEscape);
            return false;
        }
        const char e = st.text[st.pos];
        if (e != 'u') {
            char simple;
            switch (e) {
                case '"':
                    simple = '"';
                    break;
                case '\\':
                    simple = '\\';
                    break;
                case '/':
                    simple = '/';
                    break;
                case 'b':
                    simple = '\b';
                    break;
                case 'f':
                    simple = '\f';
                    break;
                case 'n':
                    simple = '\n';
                    break;
                case 'r':
                    simple = '\r';
                    break;
                case 't':
                    simple = '\t';
                    break;
                default:
                    kFail(st, json::ErrorCode::InvalidEscape);
                    return false;
            }
            if (outPos >= st.scratchCapacity) {
                kFail(st, json::ErrorCode::ScratchBufferTooSmall);
                return false;
            }
            st.scratch[outPos++] = simple;
            ++st.pos;
            continue;
        }

        // \uXXXX(SP-CCACB192 §4, QU-8E75915F 확정 - 서로게이트 페어 포함).
        ++st.pos;  // consume 'u'
        unsigned int cp1;
        if (!kParseHex4(st, &cp1)) {
            return false;
        }
        long codePoint = static_cast<long>(cp1);
        if (cp1 >= 0xD800U && cp1 <= 0xDBFFU) {
            if (st.pos + 2 > st.length || st.text[st.pos] != '\\' || st.text[st.pos + 1] != 'u') {
                kFail(st, json::ErrorCode::InvalidEscape);
                return false;
            }
            st.pos += 2;
            unsigned int cp2;
            if (!kParseHex4(st, &cp2)) {
                return false;
            }
            codePoint = utf8::combineSurrogatePair(cp1, cp2);
            if (codePoint < 0) {
                kFail(st, json::ErrorCode::InvalidEscape);
                return false;
            }
        } else if (cp1 >= 0xDC00U && cp1 <= 0xDFFFU) {
            kFail(st, json::ErrorCode::InvalidEscape);  // 단독 low surrogate
            return false;
        }
        unsigned char utf8Bytes[4];
        const unsigned int n = utf8::encode(codePoint, utf8Bytes);
        if (n == 0) {
            kFail(st, json::ErrorCode::InvalidEscape);
            return false;
        }
        if (outPos + n > st.scratchCapacity) {
            kFail(st, json::ErrorCode::ScratchBufferTooSmall);
            return false;
        }
        for (unsigned int i = 0; i < n; ++i) {
            st.scratch[outPos++] = static_cast<char>(utf8Bytes[i]);
        }
    }
    if (st.pos >= st.length) {
        kFail(st, json::ErrorCode::UnterminatedString);
        return false;
    }
    ++st.pos;  // consume closing quote
    *outStr = st.scratch;
    *outLen = outPos;
    return true;
}

// [정정, 착수 중 실측 발견, json.h 상단 주석 참고 - QU-6A72AFE6]
// SP-CCACB192 §3이 원래 요구한 "double까지 계산"은 이 커널 빌드의
// `-mgeneral-regs-only` 제약상 불가능하다(SSE/x87 자체가 꺼져 있어
// `double` 산술이 있는 함수는 컴파일이 안 됨, 실측 확인). 그래서 이
// 함수는 정수가 아닌 숫자를 double로 계산하지 않고 **문법만 검증한
// 뒤 원본 텍스트 구간을 그대로** `onNumber`에 넘긴다 - 정수 판정/
// 변환(`intPart`)은 순수 정수 산술이라 그대로 수행한다.
bool kParseNumber(ParserState& st) {
    const size_t start = st.pos;
    bool negative = false;
    if (kPeek(st) == '-') {
        negative = true;
        ++st.pos;
    }
    if (!(kPeek(st) >= '0' && kPeek(st) <= '9')) {
        kFail(st, json::ErrorCode::InvalidNumber);
        return false;
    }
    long long intPart = 0;
    bool intOverflow = false;
    if (kPeek(st) == '0') {
        ++st.pos;
    } else {
        while (kPeek(st) >= '0' && kPeek(st) <= '9') {
            const int d = kPeek(st) - '0';
            if (intPart > (0x7FFFFFFFFFFFFFFFLL - d) / 10) {
                intOverflow = true;
            }
            intPart = intPart * 10 + d;
            ++st.pos;
        }
    }

    bool isFloat = false;
    if (kPeek(st) == '.') {
        isFloat = true;
        ++st.pos;
        if (!(kPeek(st) >= '0' && kPeek(st) <= '9')) {
            kFail(st, json::ErrorCode::InvalidNumber);
            return false;
        }
        while (kPeek(st) >= '0' && kPeek(st) <= '9') {
            ++st.pos;
        }
    }

    if (kPeek(st) == 'e' || kPeek(st) == 'E') {
        isFloat = true;
        ++st.pos;
        if (kPeek(st) == '+' || kPeek(st) == '-') {
            ++st.pos;
        }
        if (!(kPeek(st) >= '0' && kPeek(st) <= '9')) {
            kFail(st, json::ErrorCode::InvalidNumber);
            return false;
        }
        while (kPeek(st) >= '0' && kPeek(st) <= '9') {
            ++st.pos;
        }
    }

    const bool isInteger = !isFloat && !intOverflow;
    const long long intValue = negative ? -intPart : intPart;
    if (st.cb->onNumber) {
        st.cb->onNumber(st.text + start, st.pos - start, isInteger, isInteger ? intValue : 0, st.userData);
    }
    return true;
}

bool kParseValue(ParserState& st) {
    kSkipWhitespace(st);
    const char c = kPeek(st);

    if (c == '{') {
        if (st.depth >= json::kMaxNestingDepth) {
            kFail(st, json::ErrorCode::NestingTooDeep);
            return false;
        }
        ++st.pos;
        ++st.depth;
        if (st.cb->onObjectStart) {
            st.cb->onObjectStart(st.userData);
        }
        kSkipWhitespace(st);
        if (kPeek(st) == '}') {
            ++st.pos;
        } else {
            for (;;) {
                kSkipWhitespace(st);
                if (kPeek(st) != '"') {
                    kFail(st, json::ErrorCode::UnexpectedCharacter);
                    return false;
                }
                const char* keyStr;
                size_t keyLen;
                if (!kParseStringRaw(st, &keyStr, &keyLen)) {
                    return false;
                }
                if (st.cb->onKey) {
                    st.cb->onKey(keyStr, keyLen, st.userData);
                }
                kSkipWhitespace(st);
                if (!kExpect(st, ':')) {
                    return false;
                }
                if (!kParseValue(st)) {
                    return false;
                }
                kSkipWhitespace(st);
                const char n = kPeek(st);
                if (n == ',') {
                    ++st.pos;
                    continue;
                }
                if (n == '}') {
                    ++st.pos;
                    break;
                }
                kFail(st, json::ErrorCode::UnexpectedCharacter);
                return false;
            }
        }
        --st.depth;
        if (st.cb->onObjectEnd) {
            st.cb->onObjectEnd(st.userData);
        }
        return true;
    }

    if (c == '[') {
        if (st.depth >= json::kMaxNestingDepth) {
            kFail(st, json::ErrorCode::NestingTooDeep);
            return false;
        }
        ++st.pos;
        ++st.depth;
        if (st.cb->onArrayStart) {
            st.cb->onArrayStart(st.userData);
        }
        kSkipWhitespace(st);
        if (kPeek(st) == ']') {
            ++st.pos;
        } else {
            for (;;) {
                if (!kParseValue(st)) {
                    return false;
                }
                kSkipWhitespace(st);
                const char n = kPeek(st);
                if (n == ',') {
                    ++st.pos;
                    continue;
                }
                if (n == ']') {
                    ++st.pos;
                    break;
                }
                kFail(st, json::ErrorCode::UnexpectedCharacter);
                return false;
            }
        }
        --st.depth;
        if (st.cb->onArrayEnd) {
            st.cb->onArrayEnd(st.userData);
        }
        return true;
    }

    if (c == '"') {
        const char* s;
        size_t len;
        if (!kParseStringRaw(st, &s, &len)) {
            return false;
        }
        if (st.cb->onString) {
            st.cb->onString(s, len, st.userData);
        }
        return true;
    }

    if (c == 't') {
        if (st.pos + 4 <= st.length && st.text[st.pos + 1] == 'r' && st.text[st.pos + 2] == 'u' &&
            st.text[st.pos + 3] == 'e') {
            st.pos += 4;
            if (st.cb->onBool) {
                st.cb->onBool(true, st.userData);
            }
            return true;
        }
        kFail(st, json::ErrorCode::UnexpectedCharacter);
        return false;
    }

    if (c == 'f') {
        if (st.pos + 5 <= st.length && st.text[st.pos + 1] == 'a' && st.text[st.pos + 2] == 'l' &&
            st.text[st.pos + 3] == 's' && st.text[st.pos + 4] == 'e') {
            st.pos += 5;
            if (st.cb->onBool) {
                st.cb->onBool(false, st.userData);
            }
            return true;
        }
        kFail(st, json::ErrorCode::UnexpectedCharacter);
        return false;
    }

    if (c == 'n') {
        if (st.pos + 4 <= st.length && st.text[st.pos + 1] == 'u' && st.text[st.pos + 2] == 'l' &&
            st.text[st.pos + 3] == 'l') {
            st.pos += 4;
            if (st.cb->onNull) {
                st.cb->onNull(st.userData);
            }
            return true;
        }
        kFail(st, json::ErrorCode::UnexpectedCharacter);
        return false;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        return kParseNumber(st);
    }

    kFail(st, json::ErrorCode::UnexpectedCharacter);
    return false;
}

}  // namespace

namespace json {

bool parse(const char* text, size_t length, const Callbacks& callbacks, char* scratchBuffer, size_t scratchCapacity,
           void* userData) {
    ParserState st{text, length, 0, &callbacks, scratchBuffer, scratchCapacity, userData, 0, false};
    if (!kParseValue(st)) {
        return false;
    }
    kSkipWhitespace(st);
    if (st.pos != st.length) {
        kFail(st, ErrorCode::UnexpectedCharacter);
        return false;
    }
    return !st.failed;
}

JsonWriter::JsonWriter(char* buffer, size_t capacity) : _buffer(buffer), _capacity(capacity) {}

bool JsonWriter::kAppendChar(char c) {
    if (_overflowed || _length >= _capacity) {
        _overflowed = true;
        return false;
    }
    _buffer[_length++] = c;
    return true;
}

bool JsonWriter::kAppend(const char* s, size_t n) {
    if (_overflowed || _length + n > _capacity) {
        _overflowed = true;
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        _buffer[_length++] = s[i];
    }
    return true;
}

bool JsonWriter::kBeforeValue() {
    if (_afterKey) {
        _afterKey = false;
        return true;  // key: 뒤에 바로 오는 값 - key() 자신이 이미 콤마/needComma를 처리했다.
    }
    if (_depth > 0) {
        if (_needComma[_depth - 1]) {
            if (!kAppendChar(',')) {
                return false;
            }
        }
        _needComma[_depth - 1] = true;
    }
    return true;
}

bool JsonWriter::beginObject() {
    if (!kBeforeValue()) {
        return false;
    }
    if (!kAppendChar('{')) {
        return false;
    }
    if (_depth >= kMaxNestingDepth) {
        _overflowed = true;
        return false;
    }
    _needComma[_depth] = false;
    ++_depth;
    return true;
}

bool JsonWriter::endObject() {
    if (_depth == 0) {
        return false;
    }
    --_depth;
    return kAppendChar('}');
}

bool JsonWriter::beginArray() {
    if (!kBeforeValue()) {
        return false;
    }
    if (!kAppendChar('[')) {
        return false;
    }
    if (_depth >= kMaxNestingDepth) {
        _overflowed = true;
        return false;
    }
    _needComma[_depth] = false;
    ++_depth;
    return true;
}

bool JsonWriter::endArray() {
    if (_depth == 0) {
        return false;
    }
    --_depth;
    return kAppendChar(']');
}

bool JsonWriter::kAppendEscapedString(const char* s, size_t n) {
    if (!kAppendChar('"')) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':
                if (!kAppend("\\\"", 2)) return false;
                break;
            case '\\':
                if (!kAppend("\\\\", 2)) return false;
                break;
            case '\b':
                if (!kAppend("\\b", 2)) return false;
                break;
            case '\f':
                if (!kAppend("\\f", 2)) return false;
                break;
            case '\n':
                if (!kAppend("\\n", 2)) return false;
                break;
            case '\r':
                if (!kAppend("\\r", 2)) return false;
                break;
            case '\t':
                if (!kAppend("\\t", 2)) return false;
                break;
            default:
                if (c < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    const char buf[6] = {'\\', 'u', '0', '0', kHex[(c >> 4) & 0xF], kHex[c & 0xF]};
                    if (!kAppend(buf, 6)) return false;
                } else {
                    if (!kAppendChar(static_cast<char>(c))) return false;
                }
        }
    }
    return kAppendChar('"');
}

bool JsonWriter::key(const char* name, size_t nameLen) {
    if (!kBeforeValue()) {
        return false;
    }
    if (!kAppendEscapedString(name, nameLen)) {
        return false;
    }
    if (!kAppendChar(':')) {
        return false;
    }
    _afterKey = true;
    return true;
}

bool JsonWriter::value(const char* str, size_t len) {
    if (!kBeforeValue()) {
        return false;
    }
    return kAppendEscapedString(str, len);
}

bool JsonWriter::kAppendInt(long long n) {
    char buf[24];
    int pos = 0;
    const bool negative = n < 0;
    // INT64_MIN 절댓값 오버플로 방지 - unsigned로 옮겨서 처리(표준 관용구).
    unsigned long long u = negative ? (static_cast<unsigned long long>(-(n + 1)) + 1ULL) : static_cast<unsigned long long>(n);
    if (u == 0) {
        buf[pos++] = '0';
    }
    while (u > 0) {
        buf[pos++] = static_cast<char>('0' + (u % 10));
        u /= 10;
    }
    if (negative) {
        buf[pos++] = '-';
    }
    for (int i = 0, j = pos - 1; i < j; ++i, --j) {
        const char t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
    }
    return kAppend(buf, static_cast<size_t>(pos));
}

bool JsonWriter::value(long long n) {
    if (!kBeforeValue()) {
        return false;
    }
    return kAppendInt(n);
}

// [정정, 착수 중 실측 발견, json.h 상단 주석 참고] double을 직접
// 포맷하는 대신 이미 유효한 숫자 텍스트를 그대로 삽입한다 - 이
// 라이브러리는 `double` 산술을 전혀 하지 않는다(이 커널 빌드의
// `-mgeneral-regs-only` 제약). 문법 검증은 호출부 책임.
bool JsonWriter::rawNumber(const char* text, size_t len) {
    if (!kBeforeValue()) {
        return false;
    }
    return kAppend(text, len);
}

bool JsonWriter::value(bool b) {
    if (!kBeforeValue()) {
        return false;
    }
    return b ? kAppend("true", 4) : kAppend("false", 5);
}

bool JsonWriter::nullValue() {
    if (!kBeforeValue()) {
        return false;
    }
    return kAppend("null", 4);
}

}  // namespace json
