#include "channel.h"

#include "async_task.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "named_object.h"

namespace kernel {

bool BridgePipe::createPair(bool useHugePage, BridgePipe** outA, BridgePipe** outB) {
    if (useHugePage) {
        return false;  // v1 정책(channel.h 상단 주석 참고) - 호출부가 HugePageUnsupported로 변환
    }

    void* memA = GenericSlabAllocator::alloc(sizeof(BridgePipe));
    void* memB = memA ? GenericSlabAllocator::alloc(sizeof(BridgePipe)) : nullptr;
    void* bufA = memB ? GenericSlabAllocator::alloc(kChannelRingBufferSize) : nullptr;
    void* bufB = bufA ? GenericSlabAllocator::alloc(kChannelRingBufferSize) : nullptr;

    if (!bufB) {
        if (bufA) {
            GenericSlabAllocator::free(bufA, kChannelRingBufferSize);
        }
        if (memB) {
            GenericSlabAllocator::free(memB, sizeof(BridgePipe));
        }
        if (memA) {
            GenericSlabAllocator::free(memA, sizeof(BridgePipe));
        }
        return false;
    }

    auto* a = reinterpret_cast<BridgePipe*>(memA);
    auto* b = reinterpret_cast<BridgePipe*>(memB);

    a->peer = b;
    a->closedLocal = false;
    a->blocking = false;
    a->outbound.reset(reinterpret_cast<uint8_t*>(bufA), reinterpret_cast<uint64_t>(bufA), kChannelRingBufferSize);

    b->peer = a;
    b->closedLocal = false;
    b->blocking = false;
    b->outbound.reset(reinterpret_cast<uint8_t*>(bufB), reinterpret_cast<uint64_t>(bufB), kChannelRingBufferSize);

    *outA = a;
    *outB = b;
    return true;
}

void BridgePipe::destroyPair(BridgePipe* a, BridgePipe* b) {
    GenericSlabAllocator::free(a->outbound.data, kChannelRingBufferSize);
    GenericSlabAllocator::free(b->outbound.data, kChannelRingBufferSize);
    GenericSlabAllocator::free(a, sizeof(BridgePipe));
    GenericSlabAllocator::free(b, sizeof(BridgePipe));
}

namespace {

// closeBridge()가 이 반쪽을 닫을 때 상대 쪽에서 깨워야 할 대기자를
// 찾아 반환한다(락 스코프 밖에서 AsyncReactor::submitCompletion을
// 부르기 위해 분리) - 이 반쪽이 닫히면: (1) 상대가 "이 반쪽의
// outbound"를 읽으려 기다리던 pendingReader, (2) 상대의 outbound가
// 꽉 차서 상대 자신이 쓰기를 기다리던 pendingWriter, 둘 다 이제
// BrokenPipe로 깨어나야 한다.
void kWakeForClose(BridgePipe* closed, AsyncTask** outReader, AsyncTask** outWriter) {
    {
        SpinlockGuard guard(closed->outbound.lock);
        *outReader = closed->outbound.pendingReader;
        closed->outbound.pendingReader = nullptr;
    }
    BridgePipe* peer = closed->peer;
    {
        SpinlockGuard guard(peer->outbound.lock);
        *outWriter = peer->outbound.pendingWriter;
        peer->outbound.pendingWriter = nullptr;
    }
}

bool kIsBridgeBroken(BridgePipe* bridge) {
    return bridge->closedLocal || bridge->peer->closedLocal;
}

class OpenChannelHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<OpenChannelArgs*>(argsRaw);
        void* mem = GenericSlabAllocator::alloc(sizeof(Channel));
        if (!mem) {
            args->error = ChannelError::ResourceExhausted;
            return;
        }
        auto* channel = reinterpret_cast<Channel*>(mem);
        channel->init();

        if (args->nameLength > 0) {
            if (args->nameLength > kMaxNamedObjectNameLength) {
                GenericSlabAllocator::free(mem, sizeof(Channel));
                args->error = ChannelError::NameInUse;  // 길이 초과도 "사용 불가"로 뭉뚱그림 - 세분화 불필요
                return;
            }
            if (!NamedObjectTable::reserve(args->name, args->nameLength, NamedObjectKind::Channel,
                                            reinterpret_cast<uint64_t>(channel))) {
                GenericSlabAllocator::free(mem, sizeof(Channel));
                args->error = ChannelError::NameInUse;
                return;
            }
            channel->hasName = true;
            channel->nameLength = args->nameLength;
            memcpy(channel->name, args->name, args->nameLength);
        }

        args->error = ChannelError::None;
        args->channelId = reinterpret_cast<uint64_t>(channel);
        args->channelHandle = args->channelId;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class ConnectChannelHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask* task, void* argsRaw) override {
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
                return;
            }
            channel = reinterpret_cast<Channel*>(objectId);
        } else {
            args->error = ChannelError::NotFound;
            return;
        }

        if (args->useHugePage) {
            args->error = ChannelError::HugePageUnsupported;
            return;
        }

        PendingConnectRequest req;
        req.task = task;
        req.useHugePage = args->useHugePage;

        AsyncTask* accepter = nullptr;
        {
            SpinlockGuard guard(channel->lock);
            if (channel->destroyed) {
                args->error = ChannelError::NotFound;
                return;
            }
            channel->pushPendingConnect(&req);
            accepter = channel->pendingAccepter;
            channel->pendingAccepter = nullptr;
        }
        if (accepter) {
            AsyncReactor::submitCompletion(accepter);
        }

        while (!req.done) {
            AsyncTask::yield();
        }

        if (req.rejected) {
            args->error = ChannelError::NotFound;
            return;
        }
        args->bridge = reinterpret_cast<uint64_t>(req.resultBridge);
        args->error = ChannelError::None;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class AcceptFromChannelHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<AcceptFromChannelArgs*>(argsRaw);
        auto* channel = reinterpret_cast<Channel*>(args->channelHandle);

        for (;;) {
            PendingConnectRequest* req = nullptr;
            {
                SpinlockGuard guard(channel->lock);
                if (channel->destroyed) {
                    args->error = ChannelError::NotFound;
                    return;
                }
                req = channel->popPendingConnect();
                if (!req) {
                    channel->pendingAccepter = task;
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
                AsyncReactor::submitCompletion(req->task);
                args->error = ChannelError::ResourceExhausted;
                return;
            }

            req->resultBridge = clientSide;
            req->done = true;
            AsyncReactor::submitCompletion(req->task);

            args->bridge = reinterpret_cast<uint64_t>(serverSide);
            args->error = ChannelError::None;
            return;
        }
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class ChannelReadHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask* task, void* argsRaw) override {
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
                    wakeWriter = ring.pendingWriter;
                    ring.pendingWriter = nullptr;
                    done = true;
                } else if (kIsBridgeBroken(bridge)) {
                    args->error = ChannelError::BrokenPipe;
                    done = true;
                } else {
                    ring.pendingReader = task;
                }
            }
            if (wakeWriter) {
                AsyncReactor::submitCompletion(wakeWriter);
            }
            if (done) {
                return;
            }
            AsyncTask::yield();
        }
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class ChannelWriteHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask* task, void* argsRaw) override {
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
                    wakeReader = ring.pendingReader;
                    ring.pendingReader = nullptr;
                    done = true;
                } else {
                    ring.pendingWriter = task;
                }
            }
            if (wakeReader) {
                AsyncReactor::submitCompletion(wakeReader);
            }
            if (done) {
                return;
            }
            AsyncTask::yield();
        }
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class CloseBridgeHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<CloseBridgeArgs*>(argsRaw);
        auto* bridge = reinterpret_cast<BridgePipe*>(args->bridge);

        if (bridge->closedLocal) {
            args->error = ChannelError::None;  // 이미 닫힘 - 멱등 처리
            return;
        }
        bridge->closedLocal = true;

        AsyncTask* reader = nullptr;
        AsyncTask* writer = nullptr;
        kWakeForClose(bridge, &reader, &writer);
        if (reader) {
            AsyncReactor::submitCompletion(reader);
        }
        if (writer) {
            AsyncReactor::submitCompletion(writer);
        }

        if (bridge->peer->closedLocal) {
            // 양쪽 다 닫힘 - 이제 안전하게 반납(더 이상 아무도 이
            // 두 BridgePipe를 참조하지 않는다 - 둘 다 소유자가 이미
            // closeBridge를 불렀다는 뜻이므로).
            BridgePipe::destroyPair(bridge, bridge->peer);
        }
        args->error = ChannelError::None;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class DestroyChannelHandler : public AsyncTaskHandler {
public:
    void onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<DestroyChannelArgs*>(argsRaw);
        auto* channel = reinterpret_cast<Channel*>(args->channelHandle);

        AsyncTask* accepter = nullptr;
        PendingConnectRequest* rejectedHead = nullptr;
        {
            SpinlockGuard guard(channel->lock);
            if (channel->destroyed) {
                args->error = ChannelError::None;  // 멱등 처리
                return;
            }
            channel->destroyed = true;
            accepter = channel->pendingAccepter;
            channel->pendingAccepter = nullptr;
            rejectedHead = channel->pendingHead;
            channel->pendingHead = nullptr;
            channel->pendingTail = nullptr;
        }

        if (accepter) {
            AsyncReactor::submitCompletion(accepter);
        }
        // 대기 중이던 connectChannel 호출들을 전부 실패로 깨운다
        // (설계 문서 destroyChannel 절 그대로).
        for (PendingConnectRequest* req = rejectedHead; req;) {
            PendingConnectRequest* next = req->next;
            req->rejected = true;
            req->done = true;
            AsyncReactor::submitCompletion(req->task);
            req = next;
        }

        if (channel->hasName) {
            NamedObjectTable::release(channel->name, channel->nameLength);
        }
        GenericSlabAllocator::free(channel, sizeof(Channel));
        args->error = ChannelError::None;
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
