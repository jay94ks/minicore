#ifndef MINICORE_KERNEL_IDT_H
#define MINICORE_KERNEL_IDT_H

namespace kernel {

// CPU 예외(벡터 0-31)를 위한 IDT를 구성한다. 아직 PIC/APIC을 세팅하지
// 않았으므로 하드웨어 인터럽트(32번 이후)는 다루지 않는다 - 그건
// 별도 마일스톤. 예외가 나면 kIsrHandler가 진단 로그를 찍고 멈춘다
// (SP-8B6B8D25 §2 "인터럽트 처리"의 첫 단계 - 아직 커널 서비스로
// 라우팅하지 않고 커널 자신이 처리한다).
class Idt {
public:
    static void init();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_IDT_H
