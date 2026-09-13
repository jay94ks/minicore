#include "serial.h"

namespace {

constexpr unsigned short kCom1 = 0x3F8;

inline void kOutB(unsigned short port, unsigned char value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline unsigned char kInB(unsigned short port) {
    unsigned char value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

bool kIsTransmitEmpty() {
    return (kInB(kCom1 + 5) & 0x20) != 0;
}

}  // namespace

namespace kernel {

void Serial::kInit() {
    kOutB(kCom1 + 1, 0x00);  // 인터럽트 비활성화
    kOutB(kCom1 + 3, 0x80);  // DLAB 켜기
    kOutB(kCom1 + 0, 0x03);  // 분주값 하위바이트 (38400 baud)
    kOutB(kCom1 + 1, 0x00);  // 분주값 상위바이트
    kOutB(kCom1 + 3, 0x03);  // 8N1, DLAB 끄기
    kOutB(kCom1 + 2, 0xC7);  // FIFO 활성화/초기화, 14바이트 임계값
    kOutB(kCom1 + 4, 0x0B);  // IRQ 활성화, RTS/DSR set
}

void Serial::kPutChar(char c) {
    while (!kIsTransmitEmpty()) {
    }
    kOutB(kCom1, static_cast<unsigned char>(c));
}

void Serial::kWrite(const char* str) {
    while (*str) {
        if (*str == '\n') {
            kPutChar('\r');
        }
        kPutChar(*str++);
    }
}

}  // namespace kernel
