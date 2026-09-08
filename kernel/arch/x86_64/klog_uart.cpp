// x86_64 klog::init()/putc() — 16550 호환 UART(COM1, I/O 포트 0x3F8)
// (docs/spec/debug-console.md §2.1, §3).
#include "klog.hpp"

#include <cstdint>

namespace {

constexpr uint16_t k_com1_base = 0x3F8;

constexpr uint16_t k_reg_data = 0;         // DLAB=0: 송수신 데이터
constexpr uint16_t k_reg_ier = 1;          // DLAB=0: 인터럽트 활성화 (사용 안 함, 폴링)
constexpr uint16_t k_reg_divisor_lo = 0;   // DLAB=1: 분주비 하위 바이트
constexpr uint16_t k_reg_divisor_hi = 1;   // DLAB=1: 분주비 상위 바이트
constexpr uint16_t k_reg_fifo_ctrl = 2;
constexpr uint16_t k_reg_line_ctrl = 3;
constexpr uint16_t k_reg_modem_ctrl = 4;
constexpr uint16_t k_reg_line_status = 5;

constexpr uint8_t k_lcr_8n1 = 0x03;        // 8비트, 패리티 없음, 정지비트 1
constexpr uint8_t k_lcr_dlab = 0x80;
constexpr uint8_t k_lsr_thr_empty = 0x20;

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

}  // namespace

namespace klog {

void init() {
    outb(k_com1_base + k_reg_ier, 0x00);  // 인터럽트 비활성화 (폴링 모드)

    outb(k_com1_base + k_reg_line_ctrl, k_lcr_dlab);
    outb(k_com1_base + k_reg_divisor_lo, 0x01);  // 115200 baud, divisor=1
    outb(k_com1_base + k_reg_divisor_hi, 0x00);
    outb(k_com1_base + k_reg_line_ctrl, k_lcr_8n1);  // DLAB=0으로 복귀, 8N1

    outb(k_com1_base + k_reg_fifo_ctrl, 0x00);   // FIFO 비활성 (폴링 모드)
    outb(k_com1_base + k_reg_modem_ctrl, 0x03);  // DTR/RTS 설정
}

void putc(char c) {
    while ((inb(k_com1_base + k_reg_line_status) & k_lsr_thr_empty) == 0) {
        // THR이 빌 때까지 폴링.
    }
    outb(k_com1_base + k_reg_data, static_cast<uint8_t>(c));
}

}  // namespace klog
