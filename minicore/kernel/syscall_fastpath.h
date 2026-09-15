#ifndef MINICORE_KERNEL_SYSCALL_FASTPATH_H
#define MINICORE_KERNEL_SYSCALL_FASTPATH_H

#include "libkenv/types.h"

// x86_64 `syscall`/`sysretq` 빠른 시스템 콜 진입 경로(PN-124C105B
// "남은 것" - QU-F75DAA77 설계자 답변 "(A) swapgs + GS_BASE 정석
// 경로, PerCpu 최소 형태로 새로 만듦"). 레거시 `int 0x80` 게이트
// (idt.cpp)와 달리 `syscall` 명령은 CPU가 스택을 전혀 전환해 주지
// 않는다 - 진입 스텁이 스스로 "이 코어의 커널 스택이 어디인지"를
// 알아내 RSP를 수동으로 바꿔치기해야 하는데, 그 순간 RSP는 아직
// 유저 스택이라 그 값을 담아 둘 곳이 필요하다 - 이 모듈이 코어마다
// KERNEL_GS_BASE MSR로 가리키는 작은 스크래치(GS 기반) 하나가 그
// 역할을 한다. SP-0666DB3C §12가 설계한 범용 `PerCpu<T>`는 아직
// 없으므로(미구현) 이 모듈 전용의 최소 스크래치만 새로 만든다 -
// 그 템플릿이 나중에 생기면 이 스크래치를 그 위로 옮기는 것도
// 고려할 수 있다(지금은 범위 밖).

namespace kernel {

class SyscallFastPath {
public:
    // BSP/AP 각자 자기 코어에서 정확히 한 번 호출해야 한다(STAR/LSTAR/
    // SFMASK/EFER.SCE/KERNEL_GS_BASE 전부 코어별 MSR이라 공유되지
    // 않음 - Gdt::loadTssForThisCore()/Idt::reloadOnThisCore()와 동일한
    // 관례). Acpi::init()/Lapic::init() 이후에만 호출 가능(자기 코어
    // 인덱스를 그걸로 찾음).
    static void initForThisCore();

    // `Gdt::setRsp0ForThisCore()`와 정확히 같은 타이밍에, 같은 값으로
    // 함께 호출해야 한다(kSyncRsp0ForDispatch, scheduler.cpp) - 이
    // 코어의 GS 기반 스크래치에 새 커널 RSP를 반영해 다음 `syscall`
    // 진입이 이 값으로 스택을 전환하게 한다. TSS.RSP0과 항상 같은
    // 값을 미러링한다(두 값이 어긋나면 int 0x80/`syscall` 두 경로가
    // 서로 다른 커널 스택을 쓰게 되는 심각한 버그가 된다).
    static void setKernelRspForThisCore(uint64_t kernelRsp);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SYSCALL_FASTPATH_H
