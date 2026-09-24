#ifndef MINICORE_KERNEL_RTC_H
#define MINICORE_KERNEL_RTC_H

#include "libkenv/types.h"

// PN-83AE8AE9(SP-A658A124 §2 후속 증분 항목4) - 이 커널에 아직 없던
// wall-clock 시각 소스를 이 증분의 일부로 확정한다(계획 본문이 그렇게
// 요구). 이 프로젝트가 새로 고안한 것이 아니라 모든 PC 호환 하드웨어가
// 갖춘 MC146818A CMOS RTC(포트 0x70/0x71)를 그대로 읽는다 - OSDev
// Wiki "CMOS"/"RTC" 문서 및 실제 MC146818A 데이터시트의 표준 알고리즘
// (update-in-progress 대기 + 두 번 읽어 안정화 + BCD/12시간제 변환)
// 그대로(RM-23F4B687 §4 - 실제 하드웨어 스펙 그대로 구현, 임의 설계
// 아님). 아키텍처 종속 포트 I/O(x86_64::io_port.h)를 쓰므로 다른
// 아키텍처로 포팅 시 이 파일도 교체 대상.
namespace kernel {

// 1980년 이후 유효 범위만 가정(RTC 배터리가 없거나 시각이 전혀
// 설정되지 않은 환경에서도 안전하게 다룰 수 있게 호출자가 clamp할
// 수 있는 원시 값 그대로 - Rtc 자신은 clamp하지 않는다).
struct WallClockTime {
    uint16_t year = 1980;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
};

class Rtc {
public:
    // CMOS RTC를 읽어 현재 wall-clock 시각을 돌려준다 - 순수 하드웨어
    // 읽기(상태 변경 없음, 인터럽트/초기화 불필요), 언제든 호출 가능.
    static WallClockTime readWallClock();

    // [신규, 2026-09-25, PN-FE718C87] 그레고리력 날짜/시각을 UNIX
    // 에폭 초(1970-01-01 00:00:00 UTC 기준)로 변환 - Howard Hinnant의
    // 잘 알려진 `days_from_civil` 알고리즘(공개된 표준 알고리즘, 이
    // 프로젝트가 새로 고안한 게 아님 - libc++ `<chrono>` 등 여러
    // 표준 구현이 쓰는 것과 동일)을 그대로 옮겼다. ext4의 inode
    // 타임스탬프(atime/ctime/mtime, 순수 UNIX 에폭 32비트 초)를 채울
    // 때 필요(FAT의 `kFatEncodeDate`류와 달리 ext4는 자체 인코딩
    // 없이 에폭 초를 그대로 저장). 순수 함수(I/O 없음) - 실제 CMOS
    // 읽기가 필요하면 `readWallClock()`을 먼저 부를 것.
    static uint32_t toEpochSeconds(const WallClockTime& t);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_RTC_H
