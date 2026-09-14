#include "task.h"

#include "libkenv/types.h"
#include "page_frame_allocator.h"
#include "paging.h"

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

}  // namespace

namespace kernel {

void Task::init(TaskEntry entry, void* arg, uint64_t stackSize) {
    const uint32_t order = kOrderForStackSize(stackSize);
    kernelStackPhys = PageFrameAllocator::allocOrder(order);
    kernelStackSize = 4096UL << order;

    const uint64_t stackTop = kPhysToVirt(kernelStackPhys) + kernelStackSize;

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
