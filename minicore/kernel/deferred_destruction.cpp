#include "deferred_destruction.h"

#include "acpi.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "scheduler.h"

// [신규, 2026-09-20, SP-5130284C] 코어별 "지금 인터럽트 컨텍스트인가"
// 깊이 카운터 - 다른 코어가 읽지 않으므로(각 코어가 자기 자신의
// 칸만 건드림) Atomic 불필요. 다만 같은 코어 위 중첩 인터럽트(NMI 등)에
// 안전하려면 실제 증감 명령어 자체가 단일 명령이어야 한다(x86은
// 명령어 중간에는 인터럽트를 받지 않으므로 - kEnterInterruptDepth/
// kLeaveInterruptDepth 정의의 인라인 asm이 그 이유로 컴파일러의
// load+add+store 분리를 피하고 명시적으로 "incl [mem]"/"decl [mem]"
// 하나만 내보내게 강제한다).
namespace kernel {
namespace {

uint32_t gInterruptDepth[kAcpiMaxCpus] = {};

AtomicPtr<ControlBlockBase> gDeferredHead{nullptr};

bool kIsInInterruptContext() {
    return gInterruptDepth[Scheduler::currentCoreIndex()] > 0;
}

// 할당 없음, 락 없음 - 순수 CAS 루프(Treiber 스택 push). 인터럽트
// 컨텍스트를 포함해 어디서든 안전하게 호출 가능.
void kPushDeferredDestructionImpl(ControlBlockBase* block) {
    ControlBlockBase* head = gDeferredHead.load();
    do {
        block->setDeferredNext(head);
    } while (!gDeferredHead.compareExchange(head, block));
}

}  // namespace

void kInitDeferredDestruction() {
    gShouldDeferHeavyDestruction = &kIsInInterruptContext;
    gPushDeferredDestructionHook = &kPushDeferredDestructionImpl;
}

void kDrainDeferredDestructions() {
    // 스택 전체를 한 번에 떼어낸다(head를 nullptr로 원자적 교체) -
    // 드레인 도중 다른 코어가 새로 push해도 그 항목은 다음 드레인이
    // 처리한다(유실 없음, Michael-Scott류 큐와 달리 순서 보장이
    // 필요 없는 자리라 이 정도로 충분 - RM-23F4B687 §4).
    ControlBlockBase* node = gDeferredHead.exchange(nullptr);
    while (node) {
        ControlBlockBase* next = node->deferredNext();
        node->finishDeferredDestruction();  // SP-5130284C §3.2-a - _destroyOwned + releaseWeak 재현
        node = next;
    }
}

}  // namespace kernel

// [신규, 2026-09-20, SP-5130284C §3.2] isr.S/context_switch.S가 직접
// call하는 리프 함수 - extern "C"라 이름이 안 맹글링되지만, 익명
// 네임스페이스(gInterruptDepth)에는 같은 번역 단위 안이라 그대로
// 접근 가능(익명 네임스페이스의 암묵적 using-directive).
extern "C" void kEnterInterruptDepth() {
    const kernel::uint32_t idx = kernel::Scheduler::currentCoreIndex();
    asm volatile("incl %0" : "+m"(kernel::gInterruptDepth[idx]) : : "memory");
}

extern "C" void kLeaveInterruptDepth() {
    const kernel::uint32_t idx = kernel::Scheduler::currentCoreIndex();
    asm volatile("decl %0" : "+m"(kernel::gInterruptDepth[idx]) : : "memory");
}
