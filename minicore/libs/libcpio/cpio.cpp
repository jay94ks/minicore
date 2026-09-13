#include "cpio.h"

namespace {

constexpr unsigned int kHeaderSize = 110;
constexpr char kMagic[6] = {'0', '7', '0', '7', '0', '1'};
constexpr char kTrailerName[] = "TRAILER!!!";

// 헤더 필드는 전부 8자리 16진수 ASCII다(부호 없음, 앞에 0 채움) -
// 표준 strtoul 대신 직접 파싱한다(freestanding에 <cstdlib>가 없음).
unsigned long kParseHex8(const char* field) {
    unsigned long value = 0;
    for (int i = 0; i < 8; ++i) {
        const char c = field[i];
        unsigned int digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned int>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned int>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned int>(c - 'A') + 10;
        } else {
            return value;  // 손상된 필드 - 지금까지 파싱한 값으로 방어적 반환
        }
        value = (value << 4) | digit;
    }
    return value;
}

unsigned long kAlignUp4(unsigned long value) { return (value + 3) & ~3UL; }

bool kNameEquals(const char* name, unsigned long nameLength, const char* expected) {
    unsigned long i = 0;
    for (; expected[i] != '\0'; ++i) {
        if (i >= nameLength || name[i] != expected[i]) {
            return false;
        }
    }
    return i == nameLength;
}

}  // namespace

namespace cpio {

unsigned long forEachEntry(const void* archive, unsigned long archiveSize, EntryCallback callback, void* userData) {
    const auto* base = static_cast<const unsigned char*>(archive);
    unsigned long offset = 0;
    unsigned long count = 0;

    while (offset + kHeaderSize <= archiveSize) {
        const char* header = reinterpret_cast<const char*>(base + offset);
        for (int i = 0; i < 6; ++i) {
            if (header[i] != kMagic[i]) {
                return count;  // 매직 불일치 - 여기서 멈춘다(손상 또는 아카이브 끝)
            }
        }

        const unsigned int mode = static_cast<unsigned int>(kParseHex8(header + 14));
        const unsigned long fileSize = kParseHex8(header + 54);
        const unsigned long nameSize = kParseHex8(header + 94);  // null 포함

        const unsigned long namePos = offset + kHeaderSize;
        if (namePos + nameSize > archiveSize || nameSize == 0) {
            return count;  // 손상 방어
        }
        const char* name = reinterpret_cast<const char*>(base + namePos);
        const unsigned long nameLength = nameSize - 1;  // null 제외

        const unsigned long dataPos = kAlignUp4(namePos + nameSize);
        if (dataPos + fileSize > archiveSize) {
            return count;  // 손상 방어
        }

        if (kNameEquals(name, nameLength, kTrailerName)) {
            return count;  // 정상 종료 - 트레일러는 콜백에 안 넘김
        }

        Entry entry;
        entry.name = name;
        entry.nameLength = nameLength;
        entry.data = fileSize > 0 ? (base + dataPos) : nullptr;
        entry.dataSize = fileSize;
        entry.mode = mode;
        callback(entry, userData);
        ++count;

        offset = kAlignUp4(dataPos + fileSize);
    }

    return count;
}

}  // namespace cpio
