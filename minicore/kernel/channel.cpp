#include "channel.h"

#include "async_task.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "named_object.h"
#include "page_frame_allocator.h"
#include "paging.h"

namespace kernel {

bool BridgePipe::createPair(bool useHugePage, BridgePipe** outA, BridgePipe** outB) {
    void* memA = GenericSlabAllocator::alloc(sizeof(BridgePipe));
    void* memB = memA ? GenericSlabAllocator::alloc(sizeof(BridgePipe)) : nullptr;
    if (!memB) {
        if (memA) {
            GenericSlabAllocator::free(memA, sizeof(BridgePipe));
        }
        return false;
    }

    // huge page(PN-34B34DB4)면 물리적으로 연속인 2MiB 블록을
    // PageFrameAllocator에서 직접 확보해 kPhysToVirt()로 커널
    // 가상주소를 얻는다(channel.h 상단 주석 참고 - 유저 매핑이
    // 없어 Paging을 거칠 필요가 없다) - 4KiB 폴백은 기존 그대로
    // GenericSlabAllocator를 쓴다. outbound.physBase에는 나중에
    // destroyPair가 올바른 해제 함수를 고르는 데 쓸 값(물리주소 또는
    // slab 가상주소)을 그대로 담아 둔다(RingBuffer::physBase 필드
    // 주석 참고).
    uint8_t* dataA = nullptr;
    uint8_t* dataB = nullptr;
    uint64_t physBaseA = 0;
    uint64_t physBaseB = 0;
    uint64_t capacity = 0;

    if (useHugePage) {
        physBaseA = PageFrameAllocator::allocOrder(kHugeChannelRingBufferOrder);
        physBaseB = physBaseA ? PageFrameAllocator::allocOrder(kHugeChannelRingBufferOrder) : 0;
        if (!physBaseB) {
            if (physBaseA) {
                PageFrameAllocator::freeOrder(physBaseA, kHugeChannelRingBufferOrder);
            }
            GenericSlabAllocator::free(memB, sizeof(BridgePipe));
            GenericSlabAllocator::free(memA, sizeof(BridgePipe));
            return false;
        }
        dataA = reinterpret_cast<uint8_t*>(kPhysToVirt(physBaseA));
        dataB = reinterpret_cast<uint8_t*>(kPhysToVirt(physBaseB));
        capacity = kHugeChannelRingBufferSize;
    } else {
        void* bufA = GenericSlabAllocator::alloc(kChannelRingBufferSize);
        void* bufB = bufA ? GenericSlabAllocator::alloc(kChannelRingBufferSize) : nullptr;
        if (!bufB) {
            if (bufA) {
                GenericSlabAllocator::free(bufA, kChannelRingBufferSize);
            }
            GenericSlabAllocator::free(memB, sizeof(BridgePipe));
            GenericSlabAllocator::free(memA, sizeof(BridgePipe));
            return false;
        }
        dataA = reinterpret_cast<uint8_t*>(bufA);
        dataB = reinterpret_cast<uint8_t*>(bufB);
        physBaseA = reinterpret_cast<uint64_t>(bufA);
        physBaseB = reinterpret_cast<uint64_t>(bufB);
        capacity = kChannelRingBufferSize;
    }

    auto* a = reinterpret_cast<BridgePipe*>(memA);
    auto* b = reinterpret_cast<BridgePipe*>(memB);

    a->peer = b;
    a->closedLocal = false;
    a->blocking = false;
    a->outbound.reset(dataA, physBaseA, capacity);

    b->peer = a;
    b->closedLocal = false;
    b->blocking = false;
    b->outbound.reset(dataB, physBaseB, capacity);

    *outA = a;
    *outB = b;
    return true;
}

void BridgePipe::destroyPair(BridgePipe* a, BridgePipe* b) {
    // capacity로 huge/4K 경로를 구분한다(둘이 겹칠 수 없는 고정값 -
    // 새 discriminator 필드 불필요).
    if (a->outbound.capacity == kHugeChannelRingBufferSize) {
        PageFrameAllocator::freeOrder(a->outbound.physBase, kHugeChannelRingBufferOrder);
    } else {
        GenericSlabAllocator::free(a->outbound.data, kChannelRingBufferSize);
    }
    if (b->outbound.capacity == kHugeChannelRingBufferSize) {
        PageFrameAllocator::freeOrder(b->outbound.physBase, kHugeChannelRingBufferOrder);
    } else {
        GenericSlabAllocator::free(b->outbound.data, kChannelRingBufferSize);
    }
    GenericSlabAllocator::free(a, sizeof(BridgePipe));
    GenericSlabAllocator::free(b, sizeof(BridgePipe));
}

// 이 아래 익명 네임스페이스(핸들러 구현체들) 안에서도 호출해야 하므로
// 그 바깥, kernel 네임스페이스 스코프에 정의한다 - channel.h의 선언과
// 같은(외부) 링키지를 가져야 livefs.cpp에서도 호출 가능하다(익명
// 네임스페이스 안에 두면 내부 링키지 버전이 새로 생겨 헤더 선언과
// 모호해진다).
Channel* kCreateNamedChannel(const char* name, uint64_t nameLength, ChannelError* outError) {
    void* mem = GenericSlabAllocator::alloc(sizeof(Channel));
    if (!mem) {
        *outError = ChannelError::ResourceExhausted;
        return nullptr;
    }
    auto* channel = reinterpret_cast<Channel*>(mem);
    channel->init();

    if (nameLength > 0) {
        if (nameLength > kMaxNamedObjectNameLength) {
            GenericSlabAllocator::free(mem, sizeof(Channel));
            *outError = ChannelError::NameInUse;  // 길이 초과도 "사용 불가"로 뭉뚱그림 - 세분화 불필요
            return nullptr;
        }
        if (!NamedObjectTable::reserve(name, nameLength, NamedObjectKind::Channel,
                                        reinterpret_cast<uint64_t>(channel))) {
            GenericSlabAllocator::free(mem, sizeof(Channel));
            *outError = ChannelError::NameInUse;
            return nullptr;
        }
        channel->hasName = true;
        channel->nameLength = nameLength;
        memcpy(channel->name, name, nameLength);
    }

    *outError = ChannelError::None;
    return channel;
}

namespace {

// closeBridge()가 이 반쪽을 닫을 때 상대 쪽에서 깨워야 할 대기자를
// 전부 모아 반환한다(락 스코프 밖에서 AsyncReactor::submitCompletion을
// 부르기 위해 분리) - 이 반쪽이 닫히면: (1) 상대가 "이 반쪽의
// outbound"를 읽으려 기다리던 pendingReaders 전부, (2) 상대의
// outbound가 꽉 차서 상대 자신이 쓰기를 기다리던 pendingWriters 전부,
// 모두 이제 BrokenPipe로 깨어나야 한다 - 대기자가 여럿일 수 있으므로
// (PN-C9625015) 하나만 깨우면 나머지가 영구히 못 깨어난다.
AsyncTaskWaitQueue kWakeForClose(BridgePipe* closed) {
    AsyncTaskWaitQueue woken;
    {
        SpinlockGuard guard(closed->outbound.lock);
        for (AsyncTask* t = closed->outbound.pendingReaders.popFront(); t; t = closed->outbound.pendingReaders.popFront()) {
            woken.pushBack(t);
        }
    }
    BridgePipe* peer = closed->peer;
    {
        SpinlockGuard guard(peer->outbound.lock);
        for (AsyncTask* t = peer->outbound.pendingWriters.popFront(); t; t = peer->outbound.pendingWriters.popFront()) {
            woken.pushBack(t);
        }
    }
    return woken;
}

bool kIsBridgeBroken(BridgePipe* bridge) {
    return bridge->closedLocal || bridge->peer->closedLocal;
}

class OpenChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<OpenChannelArgs*>(argsRaw);
        Channel* channel = kCreateNamedChannel(args->name, args->nameLength, &args->error);
        if (!channel) {
            co_return;
        }
        args->channelId = reinterpret_cast<uint64_t>(channel);
        args->channelHandle = args->channelId;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class ConnectChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ConnectChannelArgs*>(argsRaw);

        Channel* channel = nullptr;
        if (args->target != 0) {
            channel = reinterpret_cast<Channel*>(args->target);
        } else if (args->nameLength > 0) {
            NamedObjectKind kind{};
            uint64_t objectId = 0;
            if (!NamedObjectTable::resolve(args->name, args->nameLength, &kind, &objectId) ||
                kind != NamedObjectKind::Channel) {
                args->error = ChannelError::NotFound;  // 종류가 달라도 그냥 "못 찾음"(종류 은닉)
                co_return;
            }
            channel = reinterpret_cast<Channel*>(objectId);
        } else {
            args->error = ChannelError::NotFound;
            co_return;
        }

        // useHugePage=true는 이제 BridgePipe::createPair()가 실제로
        // 지원한다(PN-34B34DB4) - 더 이상 여기서 조기 거부하지 않는다.
        PendingConnectRequest req;
        req.task = task;
        req.useHugePage = args->useHugePage;

        AsyncTask* accepter = nullptr;
        {
            SpinlockGuard guard(channel->lock);
            if (channel->destroyed) {
                args->error = ChannelError::NotFound;
                co_return;
            }
            channel->pushPendingConnect(&req);
            accepter = channel->pendingAccepters.popFront();
        }
        if (accepter) {
            // 이 completion은 connectChannel/acceptFromChannel 핸드셰이크
            // 자체(§2.2 "acceptFromChannel 등에서 파생된 것")라 channel이
            // 스코프에 있다 - exclusivePreemptive를 그대로 전달.
            AsyncReactor::submitCompletion(accepter, channel->exclusivePreemptive);
        }

        while (!req.done) {
            AsyncTask::yield();
        }

        if (req.rejected) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        args->bridge = reinterpret_cast<uint64_t>(req.resultBridge);
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class AcceptFromChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<AcceptFromChannelArgs*>(argsRaw);
        auto* channel = reinterpret_cast<Channel*>(args->channelHandle);

        for (;;) {
            PendingConnectRequest* req = nullptr;
            {
                SpinlockGuard guard(channel->lock);
                if (channel->destroyed) {
                    args->error = ChannelError::NotFound;
                    co_return;
                }
                req = channel->popPendingConnect();
                if (!req) {
                    channel->pendingAccepters.pushBack(task);
                }
            }

            if (!req) {
                AsyncTask::yield();
                continue;
            }

            BridgePipe* serverSide = nullptr;
            BridgePipe* clientSide = nullptr;
            if (!BridgePipe::createPair(req->useHugePage, &serverSide, &clientSide)) {
                req->rejected = true;
                req->done = true;
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }

            req->resultBridge = clientSide;
            req->done = true;
            AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);

            args->bridge = reinterpret_cast<uint64_t>(serverSide);
            args->error = ChannelError::None;
            co_return;
        }
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class ChannelReadHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ChannelReadArgs*>(argsRaw);
        auto* bridge = reinterpret_cast<BridgePipe*>(args->bridge);
        RingBuffer& ring = bridge->peer->outbound;  // 내가 읽는 대상 = 상대가 쓰는 곳

        for (;;) {
            AsyncTask* wakeWriter = nullptr;
            bool done = false;
            {
                SpinlockGuard guard(ring.lock);
                if (ring.used > 0) {
                    uint64_t n = ring.used < args->maxLength ? ring.used : args->maxLength;
                    auto* out = static_cast<uint8_t*>(args->buffer);
                    for (uint64_t i = 0; i < n; ++i) {
                        out[i] = ring.data[(ring.readPos + i) % ring.capacity];
                    }
                    ring.readPos = (ring.readPos + n) % ring.capacity;
                    ring.used -= n;
                    args->bytesRead = n;
                    args->error = ChannelError::None;
                    wakeWriter = ring.pendingWriters.popFront();
                    done = true;
                } else if (kIsBridgeBroken(bridge)) {
                    args->error = ChannelError::BrokenPipe;
                    done = true;
                } else {
                    ring.pendingReaders.pushBack(task);
                }
            }
            if (wakeWriter) {
                // BridgePipe에는 원본 Channel로의 역참조가 없어 여기서는
                // exclusivePreemptive를 판단할 수 없다(async_task.h의
                // submitCompletion 주석/PN-7AC01E6E 항목 6 참고) - 기본값
                // (false)으로 남긴다.
                AsyncReactor::submitCompletion(wakeWriter);
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

class ChannelWriteHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ChannelWriteArgs*>(argsRaw);
        auto* bridge = reinterpret_cast<BridgePipe*>(args->bridge);
        RingBuffer& ring = bridge->outbound;  // 내가 쓰는 대상 = 상대가 읽는 곳

        for (;;) {
            AsyncTask* wakeReader = nullptr;
            bool done = false;
            {
                SpinlockGuard guard(ring.lock);
                const uint64_t space = ring.capacity - ring.used;
                if (kIsBridgeBroken(bridge)) {
                    args->error = ChannelError::BrokenPipe;
                    done = true;
                } else if (space > 0) {
                    uint64_t n = space < args->length ? space : args->length;
                    const auto* in = static_cast<const uint8_t*>(args->data);
                    for (uint64_t i = 0; i < n; ++i) {
                        ring.data[(ring.writePos + i) % ring.capacity] = in[i];
                    }
                    ring.writePos = (ring.writePos + n) % ring.capacity;
                    ring.used += n;
                    args->bytesWritten = n;
                    args->error = ChannelError::None;
                    wakeReader = ring.pendingReaders.popFront();
                    done = true;
                } else {
                    ring.pendingWriters.pushBack(task);
                }
            }
            if (wakeReader) {
                // wakeWriter와 같은 이유(위 ChannelReadHandler 참고) -
                // BridgePipe 단계라 exclusivePreemptive 판단 불가, 기본값.
                AsyncReactor::submitCompletion(wakeReader);
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

class CloseBridgeHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<CloseBridgeArgs*>(argsRaw);
        auto* bridge = reinterpret_cast<BridgePipe*>(args->bridge);

        if (bridge->closedLocal) {
            args->error = ChannelError::None;  // 이미 닫힘 - 멱등 처리
            co_return;
        }
        bridge->closedLocal = true;

        AsyncTaskWaitQueue woken = kWakeForClose(bridge);
        for (AsyncTask* t = woken.popFront(); t; t = woken.popFront()) {
            AsyncReactor::submitCompletion(t);
        }

        if (bridge->peer->closedLocal) {
            // 양쪽 다 닫힘 - 이제 안전하게 반납(더 이상 아무도 이
            // 두 BridgePipe를 참조하지 않는다 - 둘 다 소유자가 이미
            // closeBridge를 불렀다는 뜻이므로).
            BridgePipe::destroyPair(bridge, bridge->peer);
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class DestroyChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<DestroyChannelArgs*>(argsRaw);
        auto* channel = reinterpret_cast<Channel*>(args->channelHandle);

        AsyncTaskWaitQueue accepters;
        PendingConnectRequest* rejectedHead = nullptr;
        {
            SpinlockGuard guard(channel->lock);
            if (channel->destroyed) {
                args->error = ChannelError::None;  // 멱등 처리
                co_return;
            }
            channel->destroyed = true;
            for (AsyncTask* t = channel->pendingAccepters.popFront(); t; t = channel->pendingAccepters.popFront()) {
                accepters.pushBack(t);
            }
            rejectedHead = channel->pendingHead;
            channel->pendingHead = nullptr;
            channel->pendingTail = nullptr;
        }

        // 대기 중이던 acceptFromChannel 전부(여럿일 수 있음,
        // PN-C9625015)를 깨운다 - 각자 다시 락을 잡고 channel->destroyed
        // 를 확인해 NotFound로 반환한다.
        for (AsyncTask* t = accepters.popFront(); t; t = accepters.popFront()) {
            AsyncReactor::submitCompletion(t, channel->exclusivePreemptive);
        }
        // 대기 중이던 connectChannel 호출들을 전부 실패로 깨운다
        // (설계 문서 destroyChannel 절 그대로).
        for (PendingConnectRequest* req = rejectedHead; req;) {
            PendingConnectRequest* next = req->next;
            req->rejected = true;
            req->done = true;
            AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
            req = next;
        }

        if (channel->hasName) {
            NamedObjectTable::release(channel->name, channel->nameLength);
        }
        GenericSlabAllocator::free(channel, sizeof(Channel));
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

OpenChannelHandler gOpenChannelHandler;
ConnectChannelHandler gConnectChannelHandler;
AcceptFromChannelHandler gAcceptFromChannelHandler;
ChannelReadHandler gChannelReadHandler;
ChannelWriteHandler gChannelWriteHandler;
CloseBridgeHandler gCloseBridgeHandler;
DestroyChannelHandler gDestroyChannelHandler;

}  // namespace

void Channel::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointOpenChannel, &gOpenChannelHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointConnectChannel, &gConnectChannelHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointAcceptFromChannel, &gAcceptFromChannelHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointChannelRead, &gChannelReadHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointChannelWrite, &gChannelWriteHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointCloseBridge, &gCloseBridgeHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDestroyChannel, &gDestroyChannelHandler);
}

}  // namespace kernel
