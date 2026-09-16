#include "event_topic.h"

#include "libkenv/chunked_list.h"
#include "libkenv/spinlock.h"
#include "libkmm/slab.h"

namespace {

enum class EventSubscriberKind : kernel::uint32_t { UserThread, KernelCallback };

struct EventSubscriber {
    EventSubscriberKind kind = EventSubscriberKind::KernelCallback;
    kernel::Task* task = nullptr;             // kind == UserThread
    kernel::EventCallback callback = nullptr;  // kind == KernelCallback
    kernel::EventPendingQueue pending;         // kind == UserThread 전용
};

constexpr kernel::uint32_t kSubscriberChunkCapacity = 8;
using SubscriberList = kernel::ChunkedList<EventSubscriber, kSubscriberChunkCapacity>;

struct EventTopic {
    bool used = false;
    kernel::TopicPublishSource source = kernel::TopicPublishSource::KernelOnly;
    SubscriberList subscribers;
    kernel::Spinlock lock;
};

// v1 상한 - AsyncCallbackRegistry(async_task.cpp)의 kMaxHandlers와
// 동일한 패턴. 필요해지면 늘린다.
constexpr kernel::uint32_t kMaxEventTopics = 32;
EventTopic gTopics[kMaxEventTopics];
kernel::uint32_t gNextTopicId = 0;
kernel::Spinlock gRegistryLock;

}  // namespace

namespace kernel {

void EventPendingQueue::push(const EventPayload& payload) {
    if (_count < kCapacity) {
        const uint32_t tail = (_head + _count) % kCapacity;
        _entries[tail] = payload;
        ++_count;
    } else {
        // 가득 참 - 가장 오래된 것부터 버린다(§5) - _head 자리를 새
        // 값으로 덮어쓰고 head를 한 칸 전진시키면 그게 곧 "가장 오래된
        // 항목 제거 + 새 항목을 맨 뒤에 추가"와 동일한 효과다.
        _entries[_head] = payload;
        _head = (_head + 1) % kCapacity;
    }
}

bool EventPendingQueue::pop(EventPayload* outPayload) {
    if (_count == 0) {
        return false;
    }
    *outPayload = _entries[_head];
    _head = (_head + 1) % kCapacity;
    --_count;
    return true;
}

EventTopicId EventTopicRegistry::registerTopic(TopicPublishSource source) {
    SpinlockGuard guard(gRegistryLock);
    if (gNextTopicId >= kMaxEventTopics) {
        return kInvalidTopicId;
    }
    const EventTopicId id = gNextTopicId++;
    gTopics[id].used = true;
    gTopics[id].source = source;
    return id;
}

bool EventTopicRegistry::allowsUserPublish(EventTopicId topic) {
    (void)topic;
    // v1: TopicPublishSource엔 KernelOnly만 있어(§3, QU-B76FB9CF -
    // PublishEvent/유저->커널 방향 자체가 제거됨) 유저 발행을 허용
    // 하는 토픽이 애초에 존재할 수 없다 - 향후 재도입 시 여기서
    // gTopics[topic].source를 실제로 분기하면 된다.
    return false;
}

void EventTopicRegistry::subscribeKernel(EventTopicId topic, EventCallback callback) {
    if (topic >= gNextTopicId || !gTopics[topic].used) {
        return;
    }
    EventTopic& t = gTopics[topic];
    SpinlockGuard guard(t.lock);
    t.subscribers.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    EventSubscriber sub;
    sub.kind = EventSubscriberKind::KernelCallback;
    sub.callback = callback;
    t.subscribers.insert(sub);
}

bool EventTopicRegistry::subscribeUserThread(EventTopicId topic, Task* task) {
    if (topic >= gNextTopicId || !gTopics[topic].used) {
        return false;
    }
    EventTopic& t = gTopics[topic];
    SpinlockGuard guard(t.lock);
    t.subscribers.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    EventSubscriber sub;
    sub.kind = EventSubscriberKind::UserThread;
    sub.task = task;
    return t.subscribers.insert(sub) != nullptr;
}

bool EventTopicRegistry::unsubscribeUserThread(EventTopicId topic, Task* task) {
    if (topic >= gNextTopicId || !gTopics[topic].used) {
        return false;
    }
    EventTopic& t = gTopics[topic];
    SpinlockGuard guard(t.lock);
    SubscriberList::Slot* slot = t.subscribers.find([&](const EventSubscriber& sub) {
        return sub.kind == EventSubscriberKind::UserThread && sub.task == task;
    });
    if (!slot) {
        return false;
    }
    t.subscribers.erase(slot);
    return true;
}

bool EventTopicRegistry::pollUserThreadPending(EventTopicId topic, Task* task, EventPayload* outPayload) {
    if (topic >= gNextTopicId || !gTopics[topic].used) {
        return false;
    }
    EventTopic& t = gTopics[topic];
    SpinlockGuard guard(t.lock);
    SubscriberList::Slot* slot = t.subscribers.find([&](const EventSubscriber& sub) {
        return sub.kind == EventSubscriberKind::UserThread && sub.task == task;
    });
    if (!slot) {
        return false;
    }
    return slot->value.pending.pop(outPayload);
}

void EventPublisher::publish(EventTopicId topic, const EventPayload& payload) {
    if (topic >= gNextTopicId || !gTopics[topic].used) {
        return;
    }
    EventTopic& t = gTopics[topic];
    SpinlockGuard guard(t.lock);
    t.subscribers.forEach([&](EventSubscriber& sub, SubscriberList::Slot*) {
        if (sub.kind == EventSubscriberKind::KernelCallback) {
            sub.callback(topic, payload);
        } else {
            sub.pending.push(payload);
            // §7: WaitEvent로 블로킹 중이면 AsyncReactor::
            // submitCompletion()으로 깨워야 하지만, "블로킹 중"이라는
            // 상태 자체가 항목2(WaitEvent syscall) 구현 시 AsyncTask와
            // 함께 생긴다 - 그 배선은 항목2 몫으로 남긴다(지금은 pending
            // 큐 적재까지만, PN-AAE631EA 항목4 범위 그대로).
        }
    });
}

}  // namespace kernel
