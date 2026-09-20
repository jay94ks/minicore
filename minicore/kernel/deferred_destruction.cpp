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

// [신규, 2026-09-21, PN-D7B66FE4, DC-53B93BFF (B)] 코어별 전용
// 인터럽트 디스패치 스택 - deferred_destruction.h의 kEnterInterruptDepth/
// kLeaveInterruptDepth 문서 주석 참고(Linux percpu irq stack과 동일한
// 원칙, SP-677210E6의 하드웨어 IST1-4와는 다른 메커니즘). 관측된
// 초과폭(~8.5KiB, PN-584DB994 갱신29)에 비해 넉넉한 여유를 둔다 -
// gIstStacks(gdt.cpp)와 동일한 "alignas(16) + top = base+size" 관례.
constexpr uint32_t kInterruptDispatchStackSize = 32 * 1024;
alignas(16) uint8_t gInterruptDispatchStacks[kAcpiMaxCpus][kInterruptDispatchStackSize];

// 가장 바깥쪽(중첩 아닌) 인터럽트 진입이 스왑 직전의 원래 rsp를
// 잠깐 맡겨 두는 곳 - 그 코어에서 대응하는 이탈이 원래 스택으로
// 되돌아갈 때만 읽는다.
uint64_t gSavedTaskRspForOutermostInterrupt[kAcpiMaxCpus] = {};

uint64_t kInterruptDispatchStackTop(uint32_t coreIndex) {
    return reinterpret_cast<uint64_t>(&gInterruptDispatchStacks[coreIndex][kInterruptDispatchStackSize]);
}

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

// [신규, 2026-09-20, SP-5130284C §3.2 / 갱신, 2026-09-21, PN-D7B66FE4]
// isr.S/context_switch.S가 직접 call하는 리프 함수 - extern "C"라
// 이름이 안 맹글링되지만, 익명 네임스페이스(gInterruptDepth 등)에는
// 같은 번역 단위 안이라 그대로 접근 가능(익명 네임스페이스의 암묵적
// using-directive). 반환값의 의미는 deferred_destruction.h 문서
// 주석 참고 - isr.S만 실제로 그 값을 써서 rsp를 바꾼다.
extern "C" kernel::uint64_t kEnterInterruptDepth(kernel::uint64_t currentRsp) {
    const kernel::uint32_t idx = kernel::Scheduler::currentCoreIndex();
    asm volatile("incl %0" : "+m"(kernel::gInterruptDepth[idx]) : : "memory");
    if (kernel::gInterruptDepth[idx] != 1) {
        return 0;  // 중첩 - 이미 전용 스택 위에 있으므로 되감지 않는다
    }
    kernel::gSavedTaskRspForOutermostInterrupt[idx] = currentRsp;
    return kernel::kInterruptDispatchStackTop(idx);
}

extern "C" kernel::uint64_t kLeaveInterruptDepth() {
    const kernel::uint32_t idx = kernel::Scheduler::currentCoreIndex();
    asm volatile("decl %0" : "+m"(kernel::gInterruptDepth[idx]) : : "memory");
    if (kernel::gInterruptDepth[idx] != 0) {
        return 0;  // 아직 바깥쪽 인터럽트가 진행 중 - 스택을 되돌리지 않는다
    }
    return kernel::gSavedTaskRspForOutermostInterrupt[idx];
}

// [신규, 2026-09-21, PN-584DB994] kIsrHandler(idt.cpp)가 이 인터럽트가
// 중첩인지(값 > 1) 아닌지(값 == 1) 판단하는 데 쓴다 - deferred_destruction.h
// 문서 주석 참고. 단순 읽기라 kEnter/LeaveInterruptDepth처럼 인라인
// asm으로 강제할 필요 없음(load 자체가 쪼개질 위험이 없는 단순 조회).
extern "C" unsigned int kCurrentInterruptDepth() {
    const kernel::uint32_t idx = kernel::Scheduler::currentCoreIndex();
    return kernel::gInterruptDepth[idx];
}
