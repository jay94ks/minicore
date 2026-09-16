#include "interrupt_subscription.h"

#include "idt.h"
#include "process.h"

namespace kernel {

namespace {

bool gDelegationAllowed[256] = {};

void kInterruptSubscriptionIsr(InterruptFrame* frame);

// [SP-71DA77B3 §2] 하드코딩된 배제 목록 - 커널이 직접 처리해야 하는
// 고정 벡터는 절대 이 서브시스템에 위임될 수 없다. 각 상수의 실제
// 정의는 해당 파일(대부분 익명 네임스페이스 내부 상수라 여기서
// 직접 import할 수 없음)을 참고 - 값이 바뀌면 이 목록도 함께
// 갱신해야 한다.
bool kIsFixedVector(uint32_t vector) {
    if (vector <= 31) return true;    // CPU 예외(0-31)
    if (vector == 32) return true;    // kTimerVector(timer.h) - LAPIC 틱
    if (vector == 0x22) return true;  // kHpetVector(hpet.h) - HPET Timer0
    if (vector == 0x23) return true;  // kLegacyPitVector(timer.cpp) - 레거시 PIT
    if (vector == 0x24) return true;  // kSchedulerTickVector(scheduler.h)
    if (vector == 0x80) return true;  // kSyscallVector(idt.cpp) - int 0x80
    if (vector == 0xE0) return true;  // kTlbShootdownVector(tlb_shootdown.cpp)
    if (vector == 0xE1) return true;  // kLoadBalanceWakeVector(scheduler.cpp)
    if (vector == 0xE2) return true;  // kForcedMigrationVector(scheduler.h)
    if (vector == 0xE3) return true;  // kAsyncDrainVector(async_task.cpp)
    if (vector == 0xFF) return true;  // spurious(lapic.h 관례)
    return false;
}

InterruptSubscription gSubscriptions[256];

// [구현 세부] SP-71DA77B3 §4는 dumpId를 "이 벡터 안에서만 유일한 태그"
// 로만 요구한다 - GetInterruptDump가 그 태그 하나만으로 어느 벡터의
// 링인지 찾아야 하는데, 256개 벡터를 전부 훑는 대신 벡터 번호를
// dumpId 상위 32비트에 직접 인코딩해 O(1)로 역산한다(설계 문서가
// 정확한 비트 레이아웃까지 규정하진 않음 - 순수 구현 세부,
// RM-23F4B687 §4).
uint64_t kEncodeDumpId(uint32_t vector, uint64_t localId) {
    return (static_cast<uint64_t>(vector) << 32) | (localId & 0xFFFFFFFFULL);
}
uint32_t kDecodeDumpVector(uint64_t dumpId) {
    return static_cast<uint32_t>(dumpId >> 32);
}

// [SP-71DA77B3 §3/§4] 슬롯 배열을 (exclusive 먼저, 그다음 priority
// 오름차순, 미사용 슬롯은 맨 뒤)으로 유지한다 - kMaxSubscribersPerVector
// 가 작아(8) 매번 전체를 다시 훑는 단순 선택 정렬로 충분하다(호출
// 빈도도 Subscribe/Unsubscribe 시점뿐이라 성능에 영향 없음).
void kSortSubscribers(InterruptSubscriber* subs, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < count; ++j) {
            bool jBeforeBest = false;
            if (subs[j].used && !subs[best].used) {
                jBeforeBest = true;
            } else if (subs[j].used && subs[best].used) {
                if (subs[j].exclusive && !subs[best].exclusive) {
                    jBeforeBest = true;
                } else if (subs[j].exclusive == subs[best].exclusive && subs[j].priority < subs[best].priority) {
                    jBeforeBest = true;
                }
            }
            if (jBeforeBest) {
                best = j;
            }
        }
        if (best != i) {
            InterruptSubscriber tmp = subs[i];
            subs[i] = subs[best];
            subs[best] = tmp;
        }
    }
}

// [SP-71DA77B3 §5] ISR 쪽 처리 - 인터럽트 게이트(0x8E/0xEE)로 진입해
// IF가 하드웨어에 의해 자동으로 꺼져 있으므로, 같은 코어에서 실행
// 중인 syscall 핸들러의 SpinlockGuard와 데드락할 수 없다(다른
// 코어에서의 동시 진입만 Spinlock으로 막으면 된다).
void kInterruptSubscriptionIsr(InterruptFrame* frame) {
    const uint32_t vector = static_cast<uint32_t>(frame->vector);
    InterruptSubscription& sub = gSubscriptions[vector];
    SpinlockGuard guard(sub.lock);

    uint64_t cr2 = 0;
    uint64_t cr3 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    asm volatile("mov %%cr3, %0" : "=r"(cr3));

    const uint64_t dumpId = kEncodeDumpId(vector, sub.nextDumpId++);
    InterruptFullDump& dumpSlot = sub.dumps[(sub.dumpHead + sub.dumpCount) % kInterruptDumpRingCapacity];
    if (sub.dumpCount == kInterruptDumpRingCapacity) {
        sub.dumpHead = (sub.dumpHead + 1) % kInterruptDumpRingCapacity;
    } else {
        ++sub.dumpCount;
    }
    dumpSlot = InterruptFullDump{dumpId,
                                  frame->rax, frame->rbx, frame->rcx, frame->rdx, frame->rsi, frame->rdi, frame->rbp,
                                  frame->rspOld,
                                  frame->r8, frame->r9, frame->r10, frame->r11, frame->r12, frame->r13, frame->r14,
                                  frame->r15,
                                  frame->rip, frame->rflags, cr2, cr3};

    const InterruptEvent event{frame->rip, frame->errorCode, frame->rflags, dumpId};

    // [§4 "우선순위의 의미"] exclusive 구독자가 하나라도 있으면 그들
    // "에게만" 전달한다 - 슬롯 배열이 이미 exclusive가 앞쪽에 오도록
    // 정렬돼 있으므로 첫 슬롯만 봐도 충분하다.
    const bool hasExclusive = sub.subscribers[0].used && sub.subscribers[0].exclusive;

    for (auto& subscriber : sub.subscribers) {
        if (!subscriber.used) continue;
        if (hasExclusive && !subscriber.exclusive) continue;
        if (subscriber.count < kInterruptEventQueueCapacity) {
            subscriber.events[(subscriber.head + subscriber.count) % kInterruptEventQueueCapacity] = event;
            ++subscriber.count;
        } else {
            ++subscriber.droppedCount;
        }
        if (AsyncTask* task = subscriber.waiters.popFront()) {
            AsyncReactor::submitCompletion(task);  // 인터럽트 컨텍스트에서 호출 가능(async_task.h에 명시)
        }
    }
}

InterruptSubscriber* kFindSubscriberByOwner(InterruptSubscription& sub, Task* owner) {
    for (auto& s : sub.subscribers) {
        if (s.used && s.owner == owner) {
            return &s;
        }
    }
    return nullptr;
}

class SubscribeInterruptHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SubscribeInterruptArgs*>(argsRaw);
        InterruptSubscriptionError result = InterruptSubscriptionError::VectorNotAllowed;

        if (args->vector < 256 && InterruptDelegation::isAllowed(args->vector)) {
            SharedPtr<Task> submitter = task->submitterTask.lock();
            if (submitter) {
                Task* owner = submitter.get();
                bool exclusiveOk = true;
                if (args->exclusive) {
                    // [§3 개정3] exclusive는 ProcessRole::KernelService
                    // 자격을 가진 프로세스에게만 허용된다.
                    auto* thread = static_cast<UserThread*>(owner);
                    SharedPtr<Process> process = thread->process.lock();
                    exclusiveOk = process && process->role == ProcessRole::KernelService;
                }
                if (!exclusiveOk) {
                    result = InterruptSubscriptionError::ExclusiveRequiresKernelService;
                } else {
                    InterruptSubscription& sub = gSubscriptions[args->vector];
                    SpinlockGuard guard(sub.lock);
                    if (kFindSubscriberByOwner(sub, owner)) {
                        result = InterruptSubscriptionError::AlreadySubscribed;
                    } else {
                        InterruptSubscriber* slot = nullptr;
                        for (auto& s : sub.subscribers) {
                            if (!s.used) {
                                slot = &s;
                                break;
                            }
                        }
                        if (!slot) {
                            result = InterruptSubscriptionError::SubscriberSlotsFull;
                        } else {
                            *slot = InterruptSubscriber{};
                            slot->used = true;
                            slot->owner = owner;
                            slot->priority = args->priority;
                            slot->exclusive = args->exclusive;
                            kSortSubscribers(sub.subscribers, kMaxSubscribersPerVector);
                            result = InterruptSubscriptionError::None;
                        }
                    }
                }
            }
        }

        args->error = result;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class UnsubscribeInterruptHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<UnsubscribeInterruptArgs*>(argsRaw);
        InterruptSubscriptionError result = InterruptSubscriptionError::NotSubscribed;

        if (args->vector < 256) {
            SharedPtr<Task> submitter = task->submitterTask.lock();
            if (submitter) {
                InterruptSubscription& sub = gSubscriptions[args->vector];
                SpinlockGuard guard(sub.lock);
                if (InterruptSubscriber* slot = kFindSubscriberByOwner(sub, submitter.get())) {
                    *slot = InterruptSubscriber{};  // used=false로 리셋 + 큐/대기자 비움(§6)
                    kSortSubscribers(sub.subscribers, kMaxSubscribersPerVector);
                    result = InterruptSubscriptionError::None;
                }
            }
        }

        args->error = result;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class WaitInterruptHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<WaitInterruptArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        if (!submitter || args->vector >= 256) {
            args->error = InterruptSubscriptionError::NotSubscribed;
            co_return;
        }
        Task* owner = submitter.get();
        InterruptSubscription& sub = gSubscriptions[args->vector];

        for (;;) {
            bool done = false;
            {
                // [§3 "onExec 원자성 계약"] 확인과 대기열 등록 사이에
                // 락이 풀리는 구간이 있으면 안 된다(lost-wakeup 방지,
                // MutexCore::tryAcquireOrKeepLock과 동일한 계약).
                SpinlockGuard guard(sub.lock);
                InterruptSubscriber* slot = kFindSubscriberByOwner(sub, owner);
                if (!slot) {
                    args->error = InterruptSubscriptionError::NotSubscribed;
                    done = true;
                } else if (slot->count > 0) {
                    args->outEvent = slot->events[slot->head];
                    slot->head = (slot->head + 1) % kInterruptEventQueueCapacity;
                    --slot->count;
                    args->hasMore = slot->count > 0;
                    args->error = InterruptSubscriptionError::None;
                    done = true;
                } else {
                    slot->waiters.pushBack(task);
                }
            }
            if (done) {
                co_return;
            }
            AsyncTask::yield();
        }
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class GetInterruptDumpHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<GetInterruptDumpArgs*>(argsRaw);
        const uint32_t vector = kDecodeDumpVector(args->dumpId);

        InterruptSubscriptionError result = InterruptSubscriptionError::DumpNotFound;
        if (vector < 256) {
            InterruptSubscription& sub = gSubscriptions[vector];
            SpinlockGuard guard(sub.lock);
            for (uint32_t i = 0; i < sub.dumpCount; ++i) {
                const InterruptFullDump& dump = sub.dumps[(sub.dumpHead + i) % kInterruptDumpRingCapacity];
                if (dump.dumpId == args->dumpId) {
                    args->outDump = dump;
                    result = InterruptSubscriptionError::None;
                    break;
                }
            }
        }

        args->error = result;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SubscribeInterruptHandler gSubscribeInterruptHandler;
UnsubscribeInterruptHandler gUnsubscribeInterruptHandler;
WaitInterruptHandler gWaitInterruptHandler;
GetInterruptDumpHandler gGetInterruptDumpHandler;

}  // namespace

bool InterruptDelegation::allow(uint32_t vector) {
    if (vector >= 256 || kIsFixedVector(vector)) {
        return false;
    }
    if (!gDelegationAllowed[vector]) {
        gDelegationAllowed[vector] = true;
        Idt::registerHandler(vector, &kInterruptSubscriptionIsr);
    }
    return true;
}

bool InterruptDelegation::isAllowed(uint32_t vector) {
    return vector < 256 && gDelegationAllowed[vector];
}

void InterruptSubscriptionService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointSubscribeInterrupt, &gSubscribeInterruptHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointWaitInterrupt, &gWaitInterruptHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointUnsubscribeInterrupt, &gUnsubscribeInterruptHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointGetInterruptDump, &gGetInterruptDumpHandler);
}

}  // namespace kernel
