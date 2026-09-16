#ifndef MINICORE_KERNEL_EVENT_TOPIC_H
#define MINICORE_KERNEL_EVENT_TOPIC_H

#include "libkenv/types.h"

namespace kernel {

struct Task;

// [PN-AAE631EA, SP-2602CAA6] 커널 이벤트 발행/구독 - 인터럽트 구독
// (SP-71DA77B3)/AsyncTaskCompletion(SP-F682B889 §9)/Signal/Channel
// IPC/pubreg 그 무엇도 흡수하지 않는 완전히 독립된 범용 멀티캐스트
// 토픽 발행/구독 프레임워크다(§2 서베이). 우선순위/배타적 선택
// 개념이 없다 - 모든 구독자가 매 이벤트를 받는다(§5/§11).
//
// v1 범위: §4/§6/§7/§8(토픽 레지스트리/커널 내부 구독/발행 로직/정리)
// 만 구현한다. §5의 유저 노출 syscall 3종(SubscribeEvent/
// UnsubscribeEvent/WaitEvent)은 devmgr 실사용처가 등장할 때까지
// 구현 착수를 보류한다(SP-2602CAA6 §13, QU-B76FB9CF 답변) - 그
// 전까지 `subscribeUserThread`/`pollUserThreadPending`은 syscall
// 계층 없이 TEMP 검증 코드가 직접 호출하는 내부 API로만 쓰인다.

using EventTopicId = uint32_t;

// registerTopic() 시점에 그 토픽이 허용하는 발행 방향을 선언한다.
// [2026-09-16, QU-B76FB9CF] 유저->커널(PublishEvent) 방향은 v1에서
// 완전히 제거됐다 - 따라서 이 enum은 현재 유일한 값만 갖는다. 필요
// 해지면(구체적 소비자가 생기면) 별도 질의로 재검토한다(CLAUDE.md
// 규칙 4).
enum class TopicPublishSource : uint32_t {
    KernelOnly,  // 커널->커널, 커널->유저만 - v1의 유일한 값
};

struct EventPayload {
    uint64_t code = 0;   // 토픽 안에서 이벤트 종류를 세분화(옵션)
    uint64_t data0 = 0;
    uint64_t data1 = 0;
};

// §6 - 커널 서브시스템 전용 구독 콜백. publish() 호출 컨텍스트(ISR
// 포함)에서 그 자리에서 직접 호출된다 - **무거운 작업을 여기서
// 직접 하면 안 된다**(SP-F682B889 §1의 "ISR은 무거운 로직을 직접
// 실행하면 안 된다" 원칙과 동일 - 실제 작업은 AsyncTask::submit()
// 등으로 리액터에 위임하는 게 권장 패턴). 이 콜백 안에서 같은
// 토픽에 재진입적으로 subscribeKernel()/publish()를 부르면 안
// 된다 - 토픽별 구독자 목록은 비재진입 Spinlock 하나로 보호되므로
// (§7) 그대로 데드락이다.
using EventCallback = void (*)(EventTopicId topic, const EventPayload& payload);

// §5 WaitEvent가 채울 구독자별 pending 큐 - 고정 용량 N=8(§5/§11,
// 실측 후 조정 가능). 가득 차면 가장 오래된 것부터 버린다("최근
// 이벤트가 더 중요하다"는 일반적 가정). SubscribeEvent/WaitEvent
// syscall이 아직 없어(항목2, devmgr 실사용처까지 보류), 지금은 이
// 큐 자체를 TEMP 검증 코드가 직접 확인한다.
class EventPendingQueue {
public:
    static constexpr uint32_t kCapacity = 8;

    void push(const EventPayload& payload);

    // 큐가 비어 있으면 false(outPayload 불변), 아니면 가장 오래된
    // 항목을 꺼내 outPayload에 채우고 true.
    bool pop(EventPayload* outPayload);

    uint32_t count() const { return _count; }

private:
    EventPayload _entries[kCapacity]{};
    uint32_t _head = 0;
    uint32_t _count = 0;
};

class EventTopicRegistry {
public:
    // 커널 서브시스템이 부팅/초기화 시 한 번 호출(SP-F682B889 §3.1과
    // 동일 관례 - 요청마다 등록/해제하지 않음). 실패(상한 초과) 시
    // kInvalidTopicId.
    static constexpr EventTopicId kInvalidTopicId = 0xFFFFFFFFu;
    static EventTopicId registerTopic(TopicPublishSource source);

    // "이 토픽이 유저 발행을 허용하는가" - PublishEvent가 v1에 없어
    // (§3, TopicPublishSource가 KernelOnly 하나뿐) 지금은 호출부가
    // 없지만, 향후 재도입 시를 대비해 API 자체는 남겨둔다.
    static bool allowsUserPublish(EventTopicId topic);

    // §6 - 커널 내부 구독(콜백 방식, syscall 없이 직접 등록).
    static void subscribeKernel(EventTopicId topic, EventCallback callback);

    // §5/§10-2 - UserThread 구독(내부 API). 항목2(SubscribeEvent
    // syscall) 구현 시 그 핸들러가 이 함수를 그대로 호출하면 된다 -
    // 지금은 TEMP 검증 코드가 직접 부른다. 실패(존재하지 않는 topic/
    // 슬랩 할당 실패)면 false.
    static bool subscribeUserThread(EventTopicId topic, Task* task);

    // 구독자 목록에서 제거한다 - 찾지 못하면 false. (§8의 "프로세스/
    // UserThread 종료 시 자동 정리"는 항목2와 함께 PN-40E976F2의
    // 사망 전파 목록에 배선하는 게 이 증분의 범위 밖이다 - 지금은
    // 명시적 호출로만 정리된다.)
    static bool unsubscribeUserThread(EventTopicId topic, Task* task);

    // WaitEvent 핸들러가 호출할 폴링 지점 - pending 큐에서 하나
    // 꺼낸다(§5). 실제 "이벤트가 올 때까지 블로킹"하는 것은 항목2
    // 구현 시 AsyncTask/Scheduler::parkCurrent류와 함께 배선한다 -
    // 지금은 큐가 비어 있으면 그냥 false를 반환하는 논블로킹 폴링
    // 까지만 제공한다.
    static bool pollUserThreadPending(EventTopicId topic, Task* task, EventPayload* outPayload);
};

class EventPublisher {
public:
    // 커널 코드(ISR 포함, 인터럽트 컨텍스트에서도 호출 가능)가 직접
    // 부르는 발행 경로 - 그 토픽의 구독자 전원에게 멀티캐스트한다
    // (§7). KernelCallback 구독자는 이 호출 스택 안에서 그 자리에서
    // 직접 호출되고, UserThread 구독자는 pending 큐에 적재만 한다
    // (블로킹 중 깨우기는 항목2 범위 - 위 pollUserThreadPending 문서
    // 참고).
    static void publish(EventTopicId topic, const EventPayload& payload);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_EVENT_TOPIC_H
