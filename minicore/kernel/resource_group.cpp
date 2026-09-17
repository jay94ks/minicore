#include "resource_group.h"

#include "libkenv/mem.h"
#include "process.h"
#include "scheduler.h"
#include "syscall.h"

namespace kernel {

// 정적 전역 - 실제 C++ 정적 초기화를 거친다(NSDMI 그대로 유효,
// resource_group.h의 클래스 주석 참고).
ResourceGroup gRootResourceGroup;

namespace {
constexpr char kRootResourceGroupName[] = "root";
}  // namespace

void kResourceGroupInit() {
    memcpy(gRootResourceGroup.name, kRootResourceGroupName, sizeof(kRootResourceGroupName) - 1);
    gRootResourceGroup.nameLength = sizeof(kRootResourceGroupName) - 1;
}

void ResourceGroup::addMember(const WeakPtr<Process>& proc) {
    SpinlockGuard guard(lock);
    memberProcesses.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    memberProcesses.insert(proc);
}

void ResourceGroup::removeMember(Process* proc) {
    if (!proc) {
        return;
    }
    SpinlockGuard guard(lock);
    auto* slot = memberProcesses.find([proc](const WeakPtr<Process>& weak) {
        // lock() 없이 원시 포인터로 비교 - 이 시점에 proc은 호출자가
        // 이미 SharedPtr/raw로 살아있음을 보장한 상태에서만 불린다
        // (Process::joinResourceGroup의 "옛 그룹에서 빼기" 경로).
        SharedPtr<Process> locked = weak.lock();
        return locked.get() == proc;
    });
    if (slot) {
        memberProcesses.erase(slot);
    }
}

void ResourceGroup::freeze() {
    SpinlockGuard guard(lock);
    if (frozen) {
        return;
    }
    frozen = true;
    // 이미 Ready 상태로 대기 중이던 Task는 여기서 당장 멈추지 못한다
    // (클래스 주석의 한계 참고) - 다음에 그 Task가 실제로 디스패치돼
    // Scheduler::onTick()을 거칠 때 kCheckAndMarkFrozen()이 잡는다.
    // 이 함수 자신은 순수하게 플래그만 세운다(추가 순회 불필요 -
    // frozen 하나만 보면 되므로 memberProcesses를 지금 당장 건드릴
    // 실익이 없다).
}

void ResourceGroup::thaw() {
    ChunkedList<SharedPtr<Process>, kMaxMemberChunkCapacity> toWake;
    {
        SpinlockGuard guard(lock);
        if (!frozen) {
            return;
        }
        frozen = false;
        toWake.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        memberProcesses.forEach([&](WeakPtr<Process>& weak, auto*) {
            SharedPtr<Process> proc = weak.lock();
            if (proc && proc->frozenByGroup) {
                toWake.insert(proc);
            }
        });
    }
    // 락 밖에서 실제로 깨운다(Scheduler::enqueue 자체가 별도 cli
    // 임계구역을 쓰므로 lock을 쥔 채 부를 이유가 없다 - 락 중첩 최소화).
    toWake.forEach([](SharedPtr<Process>& proc, auto*) {
        proc->frozenByGroup = false;
        if (proc->mainThread) {
            Scheduler::enqueue(Scheduler::currentCoreIndex(), proc->mainThread);
        }
    });
}

bool kCheckAndMarkFrozen(Task* task) {
    if (!task->isUserLevel) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(task);
    SharedPtr<Process> proc = thread->process.lock();
    if (!proc || !proc->group) {
        return false;
    }
    if (!proc->group->frozen) {
        return false;
    }
    proc->frozenByGroup = true;
    return true;
}

}  // namespace kernel
