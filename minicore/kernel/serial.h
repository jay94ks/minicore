#ifndef MINICORE_KERNEL_SERIAL_H
#define MINICORE_KERNEL_SERIAL_H

namespace kernel {

// COM1 UART(0x3F8) 기반 초기 커널 로그 출력 (SP-8B6B8D25 §1 요구사항 -
// "시리얼 포트를 구현해 그것으로 커널 로그를 수신한다"). 인터럽트 없이
// 폴링만 한다 - 이 단계에서는 그걸로 충분하다.
class Serial {
public:
    static void kInit();
    static void kPutChar(char c);
    static void kWrite(const char* str);
    static void kWriteHex(unsigned long value);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SERIAL_H
