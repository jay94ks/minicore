#include "utf8.h"

namespace utf8 {

long combineSurrogatePair(unsigned int high, unsigned int low) {
    if (high < 0xD800U || high > 0xDBFFU || low < 0xDC00U || low > 0xDFFFU) {
        return -1;
    }
    return 0x10000L + static_cast<long>((high - 0xD800U) << 10) + static_cast<long>(low - 0xDC00U);
}

unsigned int encode(long codePoint, unsigned char* out) {
    if (codePoint < 0 || codePoint > 0x10FFFFL || (codePoint >= 0xD800L && codePoint <= 0xDFFFL)) {
        return 0;  // 음수/범위 초과/단독 서로게이트 - 전부 무효한 스칼라 값.
    }
    if (codePoint < 0x80L) {
        out[0] = static_cast<unsigned char>(codePoint);
        return 1;
    }
    if (codePoint < 0x800L) {
        out[0] = static_cast<unsigned char>(0xC0U | (codePoint >> 6));
        out[1] = static_cast<unsigned char>(0x80U | (codePoint & 0x3FL));
        return 2;
    }
    if (codePoint < 0x10000L) {
        out[0] = static_cast<unsigned char>(0xE0U | (codePoint >> 12));
        out[1] = static_cast<unsigned char>(0x80U | ((codePoint >> 6) & 0x3FL));
        out[2] = static_cast<unsigned char>(0x80U | (codePoint & 0x3FL));
        return 3;
    }
    out[0] = static_cast<unsigned char>(0xF0U | (codePoint >> 18));
    out[1] = static_cast<unsigned char>(0x80U | ((codePoint >> 12) & 0x3FL));
    out[2] = static_cast<unsigned char>(0x80U | ((codePoint >> 6) & 0x3FL));
    out[3] = static_cast<unsigned char>(0x80U | (codePoint & 0x3FL));
    return 4;
}

}  // namespace utf8
