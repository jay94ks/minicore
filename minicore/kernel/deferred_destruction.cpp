#include "deferred_destruction.h"

#include "acpi.h"
#include "diag_ring.h"
#include "gdt.h"
#include "interrupt_frame.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "scheduler.h"

// [갱신, 2026-09-21, SP-A252E82F] 예전 gInterruptDepth 카운터 기반
// 설계를 폐기하고, "이 코어의 일반 디스패치 스택 또는 IST 스택
// 범위 안에 rsp가 있는가"를 그때그때 직접 확인하는 방식으로
// 바꿨다 - deferred_destruction.h의 kEnterInterruptStack/
// kLeaveInterruptStack/kIsAddressOnAnyInterruptStack 문서 주석 참고.
// 일반 인터럽트는 절대 중첩되지 않으므로(전체 저장소 sti 전수 조사로
// 확인, SP-A252E82F §1) 카운터로 "몇 겹째인지" 셀 필요 자체가 없다.
namespace kernel {
namespace {

// [신규, 2026-09-21, PN-D7B66FE4, DC-53B93BFF (B), 갱신 SP-A252E82F]
// 코어별 전용 일반-인터럽트 디스패치 스택 - deferred_destruction.h의
// kEnterInterruptStack/kLeaveInterruptStack 문서 주석 참고(Linux
// percpu irq stack과 동일한 원칙, gdt.cpp의 하드웨어 IST 스택과는
// 다른 메커니즘 - 이쪽은 소프트웨어가 매번 무조건 스왑한다). 관측된
// 초과폭(~8.5KiB, PN-584DB994 갱신29)에 비해 넉넉한 여유를 둔다 -
// gIstStacks(gdt.cpp)와 동일한 "alignas(16) + top = base+size" 관례.
constexpr uint32_t kInterruptDispatchStackSize = 32 * 1024;
alignas(16) uint8_t gInterruptDispatchStacks[kAcpiMaxCpus][kInterruptDispatchStackSize];

// 이번 일반 인터럽트 진입의 스왑 직전 원래 rsp를 잠깐 맡겨 두는 곳 -
// 일반 인터럽트끼리는 절대 중첩되지 않으므로 이 코어의 다음 자연
// 복귀가 항상 정확히 이 값을 가져간다(카운터가 없어도 짝이 어긋날
// 수 없음).
uint64_t gSavedTaskRsp[kAcpiMaxCpus] = {};

uint64_t kInterruptDispatchStackTop(uint32_t coreIndex) {
    return reinterpret_cast<uint64_t>(&gInterruptDispatchStacks[coreIndex][kInterruptDispatchStackSize]);
}

AtomicPtr<ControlBlockBase> gDeferredHead{nullptr};

bool kIsInInterruptContext() {
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return kIsAddressOnAnyInterruptStack(rsp, Scheduler::currentCoreIndex());
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

// [갱신, 2026-09-21, SP-A252E82F] isr.S가 일반 벡터 경로에서만
// 직접 call하는 리프 함수 - extern "C"라 이름이 안 맹글링되지만,
// 익명 네임스페이스(gInterruptDispatchStacks 등)에는 같은 번역
// 단위 안이라 그대로 접근 가능(익명 네임스페이스의 암묵적
// using-directive). 반환값의 의미는 deferred_destruction.h 문서
// 주석 참고.
extern "C" kernel::uint64_t kEnterInterruptStack(kernel::uint64_t currentRsp, kernel::uint32_t vector) {
    const kernel::uint32_t idx = kernel::Scheduler::currentCoreIndex();
    kernel::gSavedTaskRsp[idx] = currentRsp;
    // [신규, PN-61D908EB/PN-E4C6AF72] currentRsp는 곧 InterruptFrame*
    // (isr_common_stub이 스왑 전 rsp를 그대로 넘김) - 이 시점의 cs를
    // 같이 남겨 두면, 나중에 크래시가 나도 "이 ISR 진입 시점엔 이미
    // cs가 오염돼 있었는지"를 바로 알 수 있다.
    const auto* frame = reinterpret_cast<const kernel::InterruptFrame*>(currentRsp);
    kernel::kDiagRingLog(kernel::DiagRingEvent::EnterInterruptStack, idx, vector, currentRsp, frame->cs);
    return kernel::kInterruptDispatchStackTop(idx);
}

extern "C" kernel::uint64_t kLeaveInterruptStack() {
    const kernel::uint32_t idx = kernel::Scheduler::currentCoreIndex();
    const kernel::uint64_t saved = kernel::gSavedTaskRsp[idx];
    // [신규, PN-61D908EB/PN-E4C6AF72] saved도 마찬가지로 InterruptFrame*
    // - kIsrHandler(및 그 안의 동적 벡터 핸들러)가 실행되는 동안 cs가
    // 바뀌었는지를 Enter 시점 기록과 비교해 바로 확인할 수 있다.
    const auto* frame = reinterpret_cast<const kernel::InterruptFrame*>(saved);
    kernel::kDiagRingLog(kernel::DiagRingEvent::LeaveInterruptStack, idx, 0, saved, frame->cs);
    return saved;
}

// [신규, 2026-09-21, SP-A252E82F] deferred_destruction.h 문서 주석
// 참고 - 일반 디스패치 스택 범위는 이 파일이 직접 알고, IST 스택
// 범위는 다른 번역 단위(gdt.cpp)에 있는 gIstStacks를 대신 물어본다
// (Gdt::isAddressOnAnyIstStack).
extern "C" bool kIsAddressOnAnyInterruptStack(kernel::uint64_t addr, kernel::uint32_t coreIndex) {
    const kernel::uint64_t dispatchBase =
        reinterpret_cast<kernel::uint64_t>(&kernel::gInterruptDispatchStacks[coreIndex][0]);
    const kernel::uint64_t dispatchTop = dispatchBase + kernel::kInterruptDispatchStackSize;
    if (addr >= dispatchBase && addr < dispatchTop) {
        return true;
    }
    return kernel::Gdt::isAddressOnAnyIstStack(addr, coreIndex);
}
