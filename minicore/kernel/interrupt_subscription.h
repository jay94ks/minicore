#ifndef MINICORE_KERNEL_INTERRUPT_SUBSCRIPTION_H
#define MINICORE_KERNEL_INTERRUPT_SUBSCRIPTION_H

#include "async_task.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "syscall.h"
#include "task.h"

namespace kernel {

// 인터럽트 구독(Interrupt Subscription) 서브시스템(SP-71DA77B3,
// PN-B3DD3D19) - 유저랜드 커널 서비스(devmgr 등)가 하드웨어 인터럽트를
// syscall submit/wait 패턴(SP-04EE2A18)으로 받을 수 있게 한다. 콜백을
// ring3로 직접 호출하지 않는다(SP-83A07867/PN-58501EAA가 이미 다룬
// CR3/스택 안전성 문제 재발 방지) - 대신 이벤트를 큐에 쌓고
// AsyncTask 완료로 깨운다.

// [SP-71DA77B3 §2] 벡터 위임 허용 목록 - 커널이 직접 처리해야 하는
// 고정 벡터(CPU 예외, 타이머, 스케줄러 틱, syscall, IPI류)는
// 하드코딩된 배제 목록으로 절대 허용되지 않는다(interrupt_subscription.cpp
// 참고) - 이게 없으면 어떤 유저 프로세스든 스케줄러 틱 벡터 등을
// 가로채 시스템을 정지시킬 수 있다.
class InterruptDelegation {
public:
    // 성공하면 이 벡터의 ISR을 이 서브시스템으로 등록한다(최초 1회만,
    // 이미 허용된 벡터를 다시 불러도 안전 - 멱등). 고정 벡터거나
    // 범위(0-255) 밖이면 false.
    static bool allow(uint32_t vector);
    static bool isAllowed(uint32_t vector);
};

// [SP-71DA77B3 §4] 경량 페이로드 - 모든 구독자에게 매번 복사되는
// 부분이라 작게 유지한다. 전체 GPR은 여기 안 담는다(GetInterruptDump로
// 분리).
struct InterruptEvent {
    uint64_t rip = 0;
    uint64_t errorCode = 0;  // 벡터에 하드웨어 에러코드가 없으면 0
    uint64_t rflags = 0;
    uint64_t dumpId = 0;  // 공유 덤프 링을 가리키는 태그 - GetInterruptDump로 조회
};

// [SP-71DA77B3 §4] 전체 레지스터 덤프 - idt.cpp의 kPanic이 찍는 것과
// 같은 필드 집합. 이벤트마다 매번 모든 구독자 큐에 복사하기엔 무거워
// 벡터당 공유 링 하나에만 저장하고, 이벤트는 이걸 가리키는 태그만
// 들고 다닌다.
struct InterruptFullDump {
    uint64_t dumpId = 0;
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0, rsi = 0, rdi = 0, rbp = 0, rsp = 0;
    uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    uint64_t rip = 0, rflags = 0, cr2 = 0, cr3 = 0;
};

constexpr uint32_t kInterruptEventQueueCapacity = 8;  // 구독자별 이벤트 링 - 실측 후 조정(RM-23F4B687 §4)
constexpr uint32_t kInterruptDumpRingCapacity = 8;     // 벡터당 공유 덤프 링 - 실측 후 조정
constexpr uint32_t kMaxSubscribersPerVector = 8;       // 벡터당 동시 구독자 상한 - 실측 후 조정

// [신규] AsyncTask 여러 개를 FIFO로 대기시키는 침습적 큐 - channel.h의
// AsyncTaskWaitQueue와 구조/역할이 완전히 동일하지만, 인터럽트
// 서브시스템이 Channel IPC 전용 헤더(BridgePipe/ChannelError 등)에
// 얹혀갈 이유가 없어(관계없는 두 서브시스템의 불필요한 결합 방지)
// 여기 독립적으로 다시 둔다 - AsyncTask::next 재사용 관례도 동일.
struct InterruptWaiterQueue {
    AsyncTask* head = nullptr;
    AsyncTask* tail = nullptr;

    void pushBack(AsyncTask* task) {
        task->next.store(nullptr);
        if (tail) {
            tail->next.store(task);
        } else {
            head = task;
        }
        tail = task;
    }

    AsyncTask* popFront() {
        AsyncTask* task = head;
        if (task) {
            head = task->next.load();
            if (!head) {
                tail = nullptr;
            }
        }
        return task;
    }

    // [구현, PN-BD276A24] channel.h의 AsyncTaskWaitQueue::remove()와
    // 정확히 같은 이유/구현 - 취소(onCancel) 전용. FIFO 순서를 지키는
    // pushBack/popFront와 달리 임의 위치의 항목 하나를 제거해야 한다
    // (그 AsyncTask 자신이 곧 반납될 예정이라 이 큐에 댕글링 포인터로
    // 남으면 안 됨 - WaitInterruptHandler::onCancel이 이걸 부른다).
    // 선형 탐색 - 이 큐들의 길이가 짧다는 전제(kMaxSubscribersPerVector
    // 당 대기자 소수). target이 큐에 없으면(이미 정상적으로 popFront된
    // 뒤였거나 애초에 이 슬롯 소속이 아니었던 경우) 아무 일도 하지
    // 않는다.
    void remove(AsyncTask* target) {
        AsyncTask* prev = nullptr;
        for (AsyncTask* cur = head; cur; prev = cur, cur = cur->next.load()) {
            if (cur == target) {
                AsyncTask* nextNode = cur->next.load();
                if (prev) {
                    prev->next.store(nextNode);
                } else {
                    head = nextNode;
                }
                if (cur == tail) {
                    tail = prev;
                }
                return;
            }
        }
    }
};

// [SP-71DA77B3 §4] 구독자 슬롯 - 모든 필드가 POD/raw 포인터라 값
// 대입(=)으로 통째 복사/리셋해도 안전하다(우선순위 정렬 시 슬롯을
// 그대로 swap, Unsubscribe 시 `*slot = InterruptSubscriber{};`로
// 리셋).
struct InterruptSubscriber {
    bool used = false;
    Task* owner = nullptr;   // v1은 명시적 Unsubscribe로만 정리(§6 - 종료 시 자동 정리는 PN-40E976F2 이후)
    uint32_t priority = 0;   // 낮을수록 높은 우선순위 - 슬롯 배열은 항상 (exclusive 먼저, 그다음 이 값 오름차순)으로 유지
    bool exclusive = false;  // true면 "커널 서비스"(ProcessRole::KernelService) 전용 배타적 수신자
    InterruptEvent events[kInterruptEventQueueCapacity];
    uint32_t head = 0;
    uint32_t count = 0;
    uint32_t droppedCount = 0;  // 큐 가득 찼을 때 유실된 이벤트 수(진단용)
    InterruptWaiterQueue waiters;  // 이 구독자를 위해 대기 중인 WaitInterrupt AsyncTask(최대 1개)
};

// 벡터 하나(0-255)의 전체 구독 상태 - gSubscriptions[vector]로 직접
// 인덱싱한다(interrupt_subscription.cpp).
struct InterruptSubscription {
    InterruptSubscriber subscribers[kMaxSubscribersPerVector];
    InterruptFullDump dumps[kInterruptDumpRingCapacity];
    uint32_t dumpHead = 0;
    uint32_t dumpCount = 0;
    uint64_t nextDumpId = 1;  // 이 벡터 안에서만 유일 - 실제 공개 dumpId는 (vector<<32|이 값)으로 인코딩(GetInterruptDump가 벡터를 즉시 역산해 256개 링 전체를 훑지 않게 하는 구현 세부)
    Spinlock lock;  // ISR과 syscall 양쪽에서 잡는다 - 구독자 슬롯 + 덤프 링 전부 이 하나로 보호
};

// [갱신, 2026-09-17, SP-E9B44929] Interrupt 그룹(5).
constexpr SyscallEndpointId kSyscallEndpointSubscribeInterrupt = kMakeSyscallEndpointId(5, 0);
constexpr SyscallEndpointId kSyscallEndpointWaitInterrupt = kMakeSyscallEndpointId(5, 1);
constexpr SyscallEndpointId kSyscallEndpointUnsubscribeInterrupt = kMakeSyscallEndpointId(5, 2);
constexpr SyscallEndpointId kSyscallEndpointGetInterruptDump = kMakeSyscallEndpointId(5, 3);

enum class InterruptSubscriptionError : uint32_t {
    None = 0,
    VectorNotAllowed,               // 위임 허용 목록에 없는 벡터
    AlreadySubscribed,               // 이 벡터를 이미 구독 중
    SubscriberSlotsFull,             // kMaxSubscribersPerVector 도달
    ExclusiveRequiresKernelService,  // exclusive=true인데 ProcessRole::KernelService가 아님
    NotSubscribed,                   // 이 벡터의 구독자가 아님(Unsubscribe/WaitInterrupt)
    DumpNotFound,                    // dumpId가 이미 밀려났거나 존재한 적 없음
};

struct SubscribeInterruptArgs {
    uint32_t vector = 0;
    uint32_t priority = 0;
    bool exclusive = false;
    // out
    InterruptSubscriptionError error = InterruptSubscriptionError::None;
};

struct UnsubscribeInterruptArgs {
    uint32_t vector = 0;
    // out
    InterruptSubscriptionError error = InterruptSubscriptionError::None;
};

struct WaitInterruptArgs {
    uint32_t vector = 0;
    // out
    InterruptEvent outEvent{};
    bool hasMore = false;  // 이번에 꺼낸 것 말고도 큐에 더 쌓여 있는지
    InterruptSubscriptionError error = InterruptSubscriptionError::None;
};

struct GetInterruptDumpArgs {
    uint64_t dumpId = 0;
    // out
    InterruptFullDump outDump{};
    InterruptSubscriptionError error = InterruptSubscriptionError::None;
};

class InterruptSubscriptionService {
public:
    // 부팅 시 한 번 호출 - 위 4개 endpoint 전부를 SyscallRegistry에 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_INTERRUPT_SUBSCRIPTION_H
