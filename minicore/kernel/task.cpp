#include "task.h"

#include "acpi.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "scheduler.h"

namespace {

extern "C" void kTaskStartTrampoline();

// stackSize를 담기에 충분한 최소 버디 order(4KiB 단위) - 스택 크기는
// 항상 4KiB의 배수로 반올림된다(호출부가 딱 떨어지는 값을 넘기지
// 않으면 실제 확보량이 커질 수 있음, 방어적 반올림이라 문제 없음).
kernel::uint32_t kOrderForStackSize(kernel::uint64_t stackSize) {
    kernel::uint64_t pages = (stackSize + 4095UL) / 4096UL;
    kernel::uint32_t order = 0;
    while ((1UL << order) < pages) {
        ++order;
    }
    return order;
}

#if MINICORE_TASK_STACK_GUARD_PAGE

// direct map(paging.h의 kDirectMapBase)은 1GiB 거대페이지로 통짜
// 매핑돼 있어(Paging::init) 그 안에서는 4KiB 단위로 구멍을 낼 수
// 없다 - 그래서 가드 페이지가 켜지면 direct map을 아예 쓰지 않고,
// 스택마다 이 전용 가상주소 구간에 4KiB 페이지 단위로 개별
// 매핑한다(Paging::mapPage) - 다른 용도(direct map/lazy zone/LAPIC
// 등 MMIO)가 쓰는 구간과 겹치지 않는 별도 슬롯이다(관계도 참고).
constexpr kernel::uint64_t kTaskStackVirtBase = 0xFFFF902000000000UL;

kernel::Spinlock gStackVirtLock;
kernel::uint64_t gNextStackVirtBase = kTaskStackVirtBase;

// pageCount개짜리 스택 + 바로 아래 가드 페이지 1개를 합친 만큼 가상
// 주소를 순차로 떼어준다(재사용/반납은 아직 없음 - Task 소멸 자체가
// 구현 안 됨, PL-2D3184BC 8단계 이후 과제). 반환값은 가드 페이지의
// 시작 주소이고, 그 바로 위(+4096)부터가 실제 스택이다.
kernel::uint64_t kReserveStackVirtRange(kernel::uint32_t pageCount) {
    kernel::SpinlockGuard guard(gStackVirtLock);
    const kernel::uint64_t guardBase = gNextStackVirtBase;
    gNextStackVirtBase += static_cast<kernel::uint64_t>(pageCount + 1) * 4096UL;
    return guardBase;
}

#endif  // MINICORE_TASK_STACK_GUARD_PAGE

}  // namespace

namespace kernel {

void Task::init(TaskEntry entry, void* arg, uint64_t stackSize) {
    // [신규, PN-A74871F2] 이 Task를 생성 중인 코어의 NUMA 노드를 사후
    // 기록 - 할당 정책 자체는 이미 PageFrameAllocator::allocOrder()가
    // "현재 코어 노드 우선"으로 하고 있으므로 여기서는 그 사실을 나중에
    // 다시 조회할 수 있게 값만 남긴다.
    numaNode = Acpi::cpuNumaNode(Scheduler::currentCoreIndex());

    const uint32_t order = kOrderForStackSize(stackSize);
    kernelStackPhys = PageFrameAllocator::allocOrder(order);
    kernelStackSize = 4096UL << order;

#if MINICORE_TASK_STACK_GUARD_PAGE
    // 가드 페이지(guardBase, 의도적으로 안 매핑) 바로 위부터 스택을
    // 페이지 단위로 매핑한다 - 스택이 이 아래로 넘치면 #PF가 걸린다.
    // **주의(실측 확인, DC-3D3212A4)**: 이 #PF는 CR2가 가드 페이지를
    // 정확히 가리키긴 하지만, 이미 다 찬 스택에 인터럽트 프레임을
    // 푸시하려다 재폴트 -> #DF -> 트리플 폴트(조용한 리셋)로
    // 이어진다 - IST 없이는 idt.cpp의 kPanic 진단 로그까지 도달하지
    // 못한다. 지금은 "진단 없는 확실한 크래시"까지만 보장(설계자
    // 결정 대기 중, QU-4E00C118).
    const uint32_t pageCount = static_cast<uint32_t>(kernelStackSize / 4096UL);
    const uint64_t guardBase = kReserveStackVirtRange(pageCount);
    const uint64_t stackVirtBase = guardBase + 4096UL;
    for (uint32_t i = 0; i < pageCount; ++i) {
        Paging::mapPage(stackVirtBase + static_cast<uint64_t>(i) * 4096UL,
                         kernelStackPhys + static_cast<uint64_t>(i) * 4096UL, PAGE_WRITABLE);
    }
    const uint64_t stackTop = stackVirtBase + kernelStackSize;
#else
    const uint64_t stackTop = kPhysToVirt(kernelStackPhys) + kernelStackSize;
#endif
    kernelStackTop = stackTop;

    // kContextSwitch가 기대하는 pop 순서(r15,r14,r13,r12,rbx,rbp,
    // popfq,ret)와 정확히 대응하도록, 스택을 높은 주소부터 채워
    // 낮은 주소가 top(=savedRsp)이 되게 한다 - context_switch.S 참고.
    // rbx=entry, r12=arg로 채워 kTaskStartTrampoline이 그대로 꺼내
    // 쓰게 한다. RFLAGS는 IF=1(인터럽트 허용, 비트9)만 켜서 시작한다.
    auto* sp = reinterpret_cast<uint64_t*>(stackTop);
    *(--sp) = reinterpret_cast<uint64_t>(&kTaskStartTrampoline);  // "return address"
    *(--sp) = 0x202;                                              // RFLAGS: IF=1 + 예약된 비트1
    *(--sp) = 0;                                                  // rbp
    *(--sp) = reinterpret_cast<uint64_t>(entry);                  // rbx -> 트램폴린이 call
    *(--sp) = reinterpret_cast<uint64_t>(arg);                    // r12 -> 트램폴린이 rdi로 옮김
    *(--sp) = 0;                                                  // r13
    *(--sp) = 0;                                                  // r14
    *(--sp) = 0;                                                  // r15

    savedRsp = reinterpret_cast<uint64_t>(sp);
    state = TaskState::Ready;
}

}  // namespace kernel
