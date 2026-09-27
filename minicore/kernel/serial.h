#ifndef MINICORE_KERNEL_SERIAL_H
#define MINICORE_KERNEL_SERIAL_H

#include "libkenv/types.h"

namespace kernel {

// COM1 UART(0x3F8) 기반 초기 커널 로그 출력 (SP-8B6B8D25 §1 요구사항 -
// "시리얼 포트를 구현해 그것으로 커널 로그를 수신한다").
//
// [변경, 2026-09-27, DC-2CB9DDA0 방향(E), 설계자 지시] `write()`는
// 원래 문자별 UART 폴링(busy-wait) 전체를 스핀락으로 감쌌는데, 이
// 락을 쥔 코어가 실제로 느린 하드웨어 I/O를 하는 동안 다른 코어가
// 그 락을 기다리다 자기 스케줄러 틱조차 못 받아 NMI 워치독에 걸릴
// 수 있음이 gdb로 실측 확인됐다(dbgdriver 60회 배치 중 1건) - 이제
// `write()`는 링버퍼에 채워 넣기만 하고(락 보유 시간이 memcpy
// 수준으로 짧음) 실제 UART 전송은 인터럽트 핸들러가 비동기로 한다
// (`enableInterruptDriven()` 호출 이전, 즉 IOAPIC이 아직 없는 부팅
// 극초반에는 예전과 동일한 동기 폴링으로 안전하게 폴백).
class Serial {
public:
    static void init();

    // [신규] IoApic::init() 이후 한 번 호출 - COM1 IRQ4를 TX-ready
    // 인터럽트로 라우팅해 write()가 실제로 비동기 경로를 타게 한다.
    // 라우팅 실패 시 아무 효과 없이 기존 동기 폴백이 계속 쓰인다.
    static void enableInterruptDriven();

    // 항상 동기(폴링) - 인터럽트 유무와 무관하게 이 한 바이트를 지금
    // 즉시 하드웨어로 내보낸다. 일반 코드는 write()를 쓴다.
    static void putChar(char c);

    // 일반 로그 출력 경로 - `enableInterruptDriven()` 이후에는 링버퍼에
    // 채워 넣고 즉시 반환(비동기), 그 전에는 동기 폴링.
    static void write(const char* str);

    // [신규] panic/NMI/워치독 진단 덤프처럼 "이 호출 직후 이 코어가
    // 인터럽트를 영구히 끄고 멈출 수 있는" 경로 전용 - write()와
    // 달리 절대 비동기 링버퍼를 거치지 않고 항상 동기 폴링으로 그
    // 자리에서 직접 내보낸다(안 그러면 그 뒤 cli+hlt로 인터럽트가
    // 다시 안 켜져 큐에 남은 진단 로그가 영영 유실될 수 있음).
    static void writeSync(const char* str);

    static void writeHex(uint64_t value);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SERIAL_H
