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

}  // namespace kernel

#endif  // MINICORE_KERNEL_INTERRUPT_FRAME_H
