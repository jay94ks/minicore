#ifndef MINICORE_KERNEL_INTERRUPT_FRAME_H
#define MINICORE_KERNEL_INTERRUPT_FRAME_H

#include "libkenv/types.h"

namespace kernel {

// isr.S의 isr_common_stub이 스택에 쌓는 순서와 정확히 일치해야 한다
// (낮은 주소 -> 높은 주소 순, rsp가 이 구조체의 시작을 가리킨 채
// kIsrHandler가 호출됨). 레지스터 push 순서를 바꾸면 이 구조체도
// 반드시 같이 바꿔야 한다.
struct InterruptFrame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector;
    uint64_t errorCode;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rspOld;
    uint64_t ssOld;
};

// [신규, 2026-09-20, PN-81E49523 2단계, 설계자 답변] "쓰레드의 마지막으로
// 캡쳐된 TCB"라는 개념을 가리키는 이름 - 설계자 지시("InterruptFrame을
// 여러 갈래로 쪼개어놔서 혼란스럽다"는 QU-47A83CDF 지적과 일치하게)로
// 별도 타입을 새로 만들지 않고 기존 InterruptFrame을 그대로 재사용한다.
// 케이스1-3/#DB(완전한 InterruptFrame)와 케이스4(Scheduler::parkCurrent
// 기반 협조적 재개)를 개념적으로 같은 이름으로 부르기 위한 순수 별칭 -
// 새 필드/의미를 추가하지 않는다.
using TaskTcb = InterruptFrame;

}  // namespace kernel

#endif  // MINICORE_KERNEL_INTERRUPT_FRAME_H
