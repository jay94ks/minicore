#include "rtc.h"

#include "x86_64/io_port.h"

namespace {

constexpr kernel::uint16_t kCmosIndexPort = 0x70;
constexpr kernel::uint16_t kCmosDataPort = 0x71;
constexpr kernel::uint8_t kRegSecond = 0x00;
constexpr kernel::uint8_t kRegMinute = 0x02;
constexpr kernel::uint8_t kRegHour = 0x04;
constexpr kernel::uint8_t kRegDay = 0x07;
constexpr kernel::uint8_t kRegMonth = 0x08;
constexpr kernel::uint8_t kRegYear = 0x09;
constexpr kernel::uint8_t kRegStatusA = 0x0A;
constexpr kernel::uint8_t kRegStatusB = 0x0B;
// [주의, RM-23F4B687 §4] ACPI FADT의 century 필드를 아직 이 커널이
// 파싱하지 않아 정확한 레지스터 번호를 표준적으로 알아낼 방법이
// 없다 - QEMU가 에뮬레이트하는 PIIX4/i440fx, q35 두 머신타입
// 모두(이 프로젝트의 표준 회귀 대상, DS-D4E5C451) CMOS 0x32에
// century를 채워 두는 사실상의 업계 관례를 그대로 따른다(실제
// hxxp://uefi.org ACPI 스펙 예시 FADT도 0x32를 기본값으로 든다).
// 이 레지스터가 실제로 없는 하드웨어는 century=0으로 읽혀 아래에서
// 자동으로 "2000년대"로 가정한다.
constexpr kernel::uint8_t kRegCentury = 0x32;

kernel::uint8_t kCmosRead(kernel::uint8_t reg) {
    kernel::arch::kOutB(kCmosIndexPort, reg);
    return kernel::arch::kInB(kCmosDataPort);
}

bool kUpdateInProgress() {
    return (kCmosRead(kRegStatusA) & 0x80) != 0;
}

kernel::uint8_t kBcdToBin(kernel::uint8_t v) {
    return static_cast<kernel::uint8_t>((v & 0x0F) + ((v >> 4) * 10));
}

struct RawRtcSnapshot {
    kernel::uint8_t second = 0;
    kernel::uint8_t minute = 0;
    kernel::uint8_t hour = 0;
    kernel::uint8_t day = 0;
    kernel::uint8_t month = 0;
    kernel::uint8_t year = 0;
    kernel::uint8_t century = 0;

    bool operator==(const RawRtcSnapshot& o) const {
        return second == o.second && minute == o.minute && hour == o.hour && day == o.day && month == o.month &&
               year == o.year && century == o.century;
    }
};

RawRtcSnapshot kReadRawSnapshot() {
    RawRtcSnapshot s;
    s.second = kCmosRead(kRegSecond);
    s.minute = kCmosRead(kRegMinute);
    s.hour = kCmosRead(kRegHour);
    s.day = kCmosRead(kRegDay);
    s.month = kCmosRead(kRegMonth);
    s.year = kCmosRead(kRegYear);
    s.century = kCmosRead(kRegCentury);
    return s;
}

}  // namespace

namespace kernel {

WallClockTime Rtc::readWallClock() {
    // 표준 CMOS RTC 읽기 관례(OSDev Wiki "CMOS"/"RTC" 문서, 데이터시트
    // 그대로) - update-in-progress 비트가 꺼질 때까지 기다린 뒤 읽고,
    // 필드 갱신 도중에 걸쳐 읽었을 가능성을 배제하기 위해 두 번 연속
    // 같은 값이 나올 때까지 반복한다. 이론상 무한 루프가 될 수 있어
    // 실무적으로 상한(8회)을 둔다 - 넘으면 마지막으로 읽은 값을 그냥
    // 쓴다(잘못된 시각이 파일 타임스탬프에 들어가는 것뿐, 커널이
    // 멈추는 것보단 안전).
    while (kUpdateInProgress()) {
    }
    RawRtcSnapshot prev = kReadRawSnapshot();
    RawRtcSnapshot cur = prev;
    for (int attempt = 0; attempt < 8; ++attempt) {
        while (kUpdateInProgress()) {
        }
        cur = kReadRawSnapshot();
        if (cur == prev) {
            break;
        }
        prev = cur;
    }

    const uint8_t regB = kCmosRead(kRegStatusB);
    const bool isBinary = (regB & 0x04) != 0;
    const bool is24Hour = (regB & 0x02) != 0;

    uint8_t second = cur.second;
    uint8_t minute = cur.minute;
    uint8_t hour = cur.hour;
    uint8_t day = cur.day;
    uint8_t month = cur.month;
    uint8_t year2 = cur.year;
    uint8_t century = cur.century;
    const bool pmBit = (hour & 0x80) != 0;
    hour &= 0x7F;

    if (!isBinary) {
        second = kBcdToBin(second);
        minute = kBcdToBin(minute);
        hour = kBcdToBin(hour);
        day = kBcdToBin(day);
        month = kBcdToBin(month);
        year2 = kBcdToBin(year2);
        century = kBcdToBin(century);
    }
    if (!is24Hour) {
        if (pmBit && hour != 12) {
            hour = static_cast<uint8_t>(hour + 12);
        } else if (!pmBit && hour == 12) {
            hour = 0;
        }
    }

    WallClockTime out;
    out.year = (century != 0) ? static_cast<uint16_t>(century) * 100 + year2 : static_cast<uint16_t>(2000 + year2);
    out.month = month;
    out.day = day;
    out.hour = hour;
    out.minute = minute;
    out.second = second;
    return out;
}

}  // namespace kernel
