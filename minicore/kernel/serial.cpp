#include "serial.h"

#include "acpi.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "ioapic.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "x86_64/io_port.h"

namespace {

constexpr kernel::uint16_t kCom1 = 0x3F8;
constexpr kernel::uint16_t kComIer = kCom1 + 1;  // Interrupt Enable Register

// [신규, 2026-09-27, DC-2CB9DDA0 방향(E)] COM1 TX-ready IRQ 전용
// 고정 벡터 - RM-28225668 §3("디바이스 IRQ") 신규 배정, 할당 규칙
// (다음 미사용 번호)에 따라 0xE6.
constexpr kernel::uint32_t kSerialTxVector = 0xE6;
constexpr kernel::uint32_t kIsaIrqCom1 = 4;

bool kIsTransmitEmpty() {
    return (kernel::arch::kInB(kCom1 + 5) & 0x20) != 0;
}

// 항상 동기(폴링) - 인터럽트 유무를 몰라도 되는 최하위 레벨 헬퍼.
void kPutCharRaw(char c) {
    while (!kIsTransmitEmpty()) {
    }
    kernel::arch::kOutB(kCom1, static_cast<kernel::uint8_t>(c));
}

// [변경, 2026-09-27, DC-2CB9DDA0 방향(E), 설계자 지시] 이제 이 락은
// 링버퍼 상태(head/tail/used)만 보호한다 - 실제 UART I/O(느린 문자별
// 폴링)는 이 락 밖, `kSerialTxIsr`(또는 `writeSync()`의 동기 경로)
// 에서만 일어난다. 원래는 `write()` 전체(폴링 포함)를 감싸서, 이
// 락을 쥔 코어가 느린 하드웨어 I/O를 하는 동안 다른 코어가 자기
// 스케줄러 틱조차 못 받아 NMI 워치독에 걸릴 수 있었다(gdb로 실측
// 확인, dbgdriver 60회 배치 중 1건 - `Serial::write()`가 감싸던 바로
// 이 락이었다).
kernel::Spinlock gWriteLock;

constexpr kernel::uint32_t kTxBufferSize = 4096;
kernel::uint8_t gTxBuffer[kTxBufferSize];
kernel::uint32_t gTxHead = 0;  // 다음에 뺄 위치
kernel::uint32_t gTxTail = 0;  // 다음에 넣을 위치
kernel::uint32_t gTxUsed = 0;
kernel::uint64_t gTxDropped = 0;  // 버퍼가 꽉 찼을 때 버려진 바이트 수(진단용, 아직 노출 API 없음)

bool gInterruptDriven = false;

void kEnableTxInterrupt() {
    kernel::arch::kOutB(kComIer, 0x02);  // bit1 = THRE(송신 홀딩 레지스터 비었음)
}
void kDisableTxInterrupt() {
    kernel::arch::kOutB(kComIer, 0x00);
}

// [ISR, kSerialTxVector] THRE가 빌 때마다 링버퍼에서 정확히 한
// 바이트를 뽑아 즉시 내보낸다 - 절대 이 안에서 busy-wait하지 않는다
// (그러면 원래 문제가 인터럽트 컨텍스트 안에서 그대로 재발한다).
// 더 보낼 게 없으면 인터럽트를 꺼서 idle로 되돌린다(THRE는 비어
// 있는 한 계속 pending이라, 안 끄면 즉시 재진입해 busy-loop가 된다).
void kSerialTxIsr(kernel::InterruptFrame*) {
    if (!kIsTransmitEmpty()) {
        return;  // 드물게 아직 안 비었으면 다음 THRE 인터럽트에서 재시도
    }
    kernel::uint8_t c;
    {
        kernel::SpinlockGuard guard(gWriteLock);
        if (gTxUsed == 0) {
            kDisableTxInterrupt();
            return;
        }
        c = gTxBuffer[gTxHead];
        gTxHead = (gTxHead + 1) % kTxBufferSize;
        --gTxUsed;
    }
    kernel::arch::kOutB(kCom1, c);
}

// 링버퍼에 한 바이트를 채운다 - 가득 차면 버린다(진단 로그 손실은
// v1에서 허용, RM-23F4B687 §4 - 가득 찼을 때 무한정 기다리면 이
// 함수 자신이 다시 gWriteLock을 오래 쥐는 옛 문제로 되돌아간다).
// 반환값 true = "직전까지 버퍼가 비어 있었다"(인터럽트를 새로
// 켜야 하는 유일한 시점).
bool kEnqueue(char c) {
    kernel::SpinlockGuard guard(gWriteLock);
    if (gTxUsed >= kTxBufferSize) {
        ++gTxDropped;
        return false;
    }
    gTxBuffer[gTxTail] = static_cast<kernel::uint8_t>(c);
    gTxTail = (gTxTail + 1) % kTxBufferSize;
    ++gTxUsed;
    return gTxUsed == 1;
}

}  // namespace

namespace kernel {

void Serial::init() {
    kernel::arch::kOutB(kCom1 + 1, 0x00);  // 인터럽트 비활성화(enableInterruptDriven() 전까지 유지)
    kernel::arch::kOutB(kCom1 + 3, 0x80);  // DLAB 켜기
    kernel::arch::kOutB(kCom1 + 0, 0x03);  // 분주값 하위바이트 (38400 baud)
    kernel::arch::kOutB(kCom1 + 1, 0x00);  // 분주값 상위바이트
    kernel::arch::kOutB(kCom1 + 3, 0x03);  // 8N1, DLAB 끄기
    kernel::arch::kOutB(kCom1 + 2, 0xC7);  // FIFO 활성화/초기화, 14바이트 임계값
    kernel::arch::kOutB(kCom1 + 4, 0x0B);  // IRQ 활성화(OUT2/RTS/DSR set)
}

void Serial::enableInterruptDriven() {
    if (gInterruptDriven) {
        return;
    }
    Idt::registerHandler(kSerialTxVector, kSerialTxIsr);
    const bool routed = IoApic::setRedirectionForIsaIrq(kIsaIrqCom1, kSerialTxVector, Lapic::id());
    if (!routed) {
        Idt::unregisterHandler(kSerialTxVector);
        return;  // 라우팅 실패해도 write()는 계속 동기 폴백으로 안전하게 동작
    }
    gInterruptDriven = true;
}

void Serial::putChar(char c) {
    kPutCharRaw(c);
}

void Serial::write(const char* str) {
    if (!gInterruptDriven) {
        writeSync(str);
        return;
    }
    bool shouldKick = false;
    while (*str) {
        char c = *str++;
        if (c == '\n' && kEnqueue('\r')) {
            shouldKick = true;
        }
        if (kEnqueue(c)) {
            shouldKick = true;
        }
    }
    if (shouldKick) {
        kEnableTxInterrupt();
    }
}

void Serial::writeSync(const char* str) {
    SpinlockGuard guard(gWriteLock);
    while (*str) {
        if (*str == '\n') {
            kPutCharRaw('\r');
        }
        kPutCharRaw(*str++);
    }
}

void Serial::writeHex(uint64_t value) {
    // buf[0..1]="0x", buf[2..17]=16개 16진 자리, buf[18]='\0'(건드리지
    // 않음) - 여기 인덱스를 잘못 밀면 널 종단이 지워져 스택 밖까지
    // 읽어버린다(실측으로 걸림, 2026-09-14).
    char buf[19] = "0x0000000000000000";
    constexpr char kHexDigits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < 16; ++i) {
        buf[17 - i] = kHexDigits[(value >> (i * 4)) & 0xF];
    }
    write(buf);
}

}  // namespace kernel
