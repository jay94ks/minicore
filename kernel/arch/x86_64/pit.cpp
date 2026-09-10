// PIT(8254) 채널2 폴링 드라이버 구현 (pit.hpp 상단 주석 참고). 이
// 프로젝트가 지금까지 PIT를 카운터로 쓴 적이 없다(ADR-173은 레거시
// 8259 PIC 마스킹만 다뤘다) — LAPIC 타이머 보정(ADR-184)의 PIT 폴백
// 경로에서 처음 필요해졌다.
#include "pit.hpp"

namespace kern::arch::x86_64 {

namespace {

// idt.cpp/klog_uart.cpp와 같은 이유로 이 파일도 자기 몫의 로컬
// inb/outb를 둔다(공유 헤더가 없다 — 각 파일이 필요한 최소만
// 손으로 구현하는 이 프로젝트의 기존 관례).
void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

constexpr uint16_t k_port_channel2_data = 0x42;
constexpr uint16_t k_port_mode_command = 0x43;
constexpr uint16_t k_port_system_control = 0x61;  // NMI status/control(구 "keyboard controller port").

// 채널2(0b10<<6) + lobyte/hibyte 접근(0b11<<4) + 모드0(0b000<<1) +
// binary(bit0=0) — Intel 8254 데이터시트 Control Word Format.
constexpr uint8_t k_mode_channel2_mode0_lohi = 0xB0;

constexpr uint8_t k_sysctl_gate2 = 1u << 0;    // 채널2 GATE 입력.
constexpr uint8_t k_sysctl_spkr_data = 1u << 1;  // 스피커 출력 연결(끈다 — 소음 방지, 기능과 무관).
constexpr uint8_t k_sysctl_out2_status = 1u << 5;  // 채널2 OUT 핀 상태(모드0: 카운트다운 중=0, 종료=1).

}  // namespace

void pit_wait_ticks(uint16_t ticks) {
    outb(k_port_mode_command, k_mode_channel2_mode0_lohi);
    outb(k_port_channel2_data, static_cast<uint8_t>(ticks & 0xFFu));
    outb(k_port_channel2_data, static_cast<uint8_t>((ticks >> 8) & 0xFFu));

    // GATE2를 낮췄다가 다시 올려(0→1 에지) 카운트다운을 확실히 이
    // 시점부터 새로 시작시킨다 — 스피커 연결(bit1)은 끈다.
    uint8_t base = static_cast<uint8_t>((inb(k_port_system_control) & ~k_sysctl_spkr_data) &
                                         ~k_sysctl_gate2);
    outb(k_port_system_control, base);
    outb(k_port_system_control, static_cast<uint8_t>(base | k_sysctl_gate2));

    while ((inb(k_port_system_control) & k_sysctl_out2_status) == 0) {
        asm volatile("pause");
    }
}

}  // namespace kern::arch::x86_64
