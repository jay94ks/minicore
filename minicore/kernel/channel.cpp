#include "channel.h"

#include "async_task.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "named_object.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "process.h"

namespace kernel {

// [신규, 2026-09-17, PN-21C2D4E9] RingBuffer::data(UniquePtr) 전용
// 삭제자 - destroyPair가 예전에 직접 들고 있던 분기(huge page vs 4K)
// 를 그대로 옮겨 왔을 뿐, 해제 시점/경로는 전혀 안 바뀐다.
void RingBufferDeleter::operator()(uint8_t* ptr) const {
    if (useHugePage) {
        PageFrameAllocator::freeOrder(physBase, kHugeChannelRingBufferOrder);
    } else {
        GenericSlabAllocator::free(ptr, kChannelRingBufferSize);
    }
}

bool BridgePipe::createPair(bool useHugePage, SharedPtr<BridgePipe>* outA, SharedPtr<BridgePipe>* outB) {
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

    // [수정, 2026-09-17, PN-9CC66142 실측 중 발견] `peer`가 raw
    // BridgePipe*에서 `WeakPtr<BridgePipe>`로 바뀌면서, 이 슬랩
    // 메모리를 memset 없이 그대로 reinterpret_cast하면 `peer`의
    // `_block`/`_ptr` 필드가 이전 프리리스트 점유자의 쓰레기 값을
    // 그대로 담고 있다 - 아래에서 `spA->peer = WeakPtr<BridgePipe>(spB);`
    // (대입 연산자)를 실행하는 순간 그 쓰레기 `_block`을 진짜 컨트롤
    // 블록 포인터로 오인해 역참조한다(async_task.cpp의 AsyncTask::
    // submit() memset 수정과 완전히 동일한 버그, 같은 실측 세션에서
    // 발견 - "NSDMI가 이미 빈 값"이라는 가정은 실제 생성자를 거치지
    // 않는 raw 슬랩 메모리엔 적용되지 않는다). `UserThread::allocate()`
    // /`Process::allocate()`와 동일한 해법(memset(0) 먼저).
    memset(memA, 0, sizeof(BridgePipe));
    memset(memB, 0, sizeof(BridgePipe));
    auto* a = reinterpret_cast<BridgePipe*>(memA);
    auto* b = reinterpret_cast<BridgePipe*>(memB);

    a->closedLocal = false;
    a->blocking = false;
    a->outbound.reset(dataA, physBaseA, capacity);

    b->closedLocal = false;
    b->blocking = false;
    b->outbound.reset(dataB, physBaseB, capacity);

    SharedPtr<BridgePipe> spA = kMakeShared<BridgePipe>(a);
    SharedPtr<BridgePipe> spB = spA ? kMakeShared<BridgePipe>(b) : SharedPtr<BridgePipe>();
    if (!spA || !spB) {
        // 컨트롤 블록 슬랩 할당 실패(극히 드묾) - kMakeShared는 실패
        // 시 넘겨준 preConstructed(a/b)를 건드리지 않으므로, 여기서
        // 수동으로 되돌린다(위 alloc 실패 분기들과 동일한 롤백 관례).
        // spA가 성공했다면 그 SharedPtr이 스코프를 벗어나며 자기
        // outbound/자기 슬랩을 스스로 반납한다(destroy() 참고) - a를
        // 이중으로 반납하지 않도록 spA 성공 여부로 분기한다.
        if (!spA) {
            a->outbound.data.reset();
            GenericSlabAllocator::free(a, sizeof(BridgePipe));
        }
        b->outbound.data.reset();
        GenericSlabAllocator::free(b, sizeof(BridgePipe));
        return false;
    }

    spA->peer = WeakPtr<BridgePipe>(spB);
    spB->peer = WeakPtr<BridgePipe>(spA);

    *outA = spA;
    *outB = spB;
    return true;
}

namespace {

// [신규, PN-CE6A04AB/SP-CA3C3E57] Channel의 세대 태그 슬롯 테이블 -
// 유저 syscall이 넘기는 ChannelId를 검증 없이 reinterpret_cast하던
// 취약점(channel.h 상단 주석 참고)을 막는다. `Channel`은 SharedPtr이
// 아니라 `GenericSlabAllocator`로 직접 관리되는 순수 포인터 객체라
// SP-9CB55C5B의 `WeakPtr::lock()` 생존 판정을 그대로 못 쓴다 - 대신
// 슬롯의 `ptr == nullptr` 여부로 생존을 수동 판정한다(SP-CA3C3E57 §2).
struct ChannelTableSlot {
    Channel* ptr = nullptr;
    uint32_t generation = 0;
};

constexpr uint32_t kMaxChannelTableSlots = 65536;  // [확정, 2026-09-17,
// QU-1AF2C16B 답변] "채널의 전역 상한은 64K".
ChannelTableSlot gChannelTable[kMaxChannelTableSlots];
Spinlock gChannelTableLock;  // 발급/해제만 보호(드묾) - 조회는 락 없이
                             // 인덱스+세대만 비교한다(SP-CA3C3E57 §3 -
                             // 최악의 경우도 안전하게 NotFound로 실패).

ChannelId kAllocateChannelId(Channel* channel) {
    SpinlockGuard guard(gChannelTableLock);
    for (uint32_t i = 0; i < kMaxChannelTableSlots; ++i) {
        if (gChannelTable[i].ptr == nullptr) {
            gChannelTable[i].generation++;
            gChannelTable[i].ptr = channel;
            return (static_cast<uint64_t>(gChannelTable[i].generation) << 32) | i;
        }
    }
    return 0;  // 슬롯 고갈 - 호출부가 ResourceExhausted로 매핑
}

// 안전 해석 - 이 함수를 거치지 않고는 어디서도 유저 제공 ChannelId를
// Channel*로 캐스팅하지 않는다. 유저가 어떤 값을 넘기든 인덱스 범위
// 검사 + 세대 일치 확인만으로 끝난다 - reinterpret_cast<Channel*>를
// 실제 살아있는 객체가 아닌 값에 대해 절대 성립시키지 않는다.
Channel* kResolveChannelId(ChannelId id) {
    if (id == 0) {
        return nullptr;
    }
    const uint32_t index = static_cast<uint32_t>(id & 0xFFFFFFFFu);
    const uint32_t generation = static_cast<uint32_t>(id >> 32);
    if (index >= kMaxChannelTableSlots) {
        return nullptr;
    }
    ChannelTableSlot& slot = gChannelTable[index];
    if (slot.generation != generation || slot.ptr == nullptr) {
        return nullptr;
    }
    return slot.ptr;
}

// DestroyChannelHandler::onExec의 GenericSlabAllocator::free 직전에
// 호출한다 - id 자체에서 인덱스를 역산하므로 O(1).
void kFreeChannelId(ChannelId id) {
    if (id == 0) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(id & 0xFFFFFFFFu);
    if (index >= kMaxChannelTableSlots) {
        return;
    }
    SpinlockGuard guard(gChannelTableLock);
    gChannelTable[index].ptr = nullptr;  // generation은 그대로 - 다음 재사용 때 +1
}

}  // namespace

void BridgePipe::destroy() {
    // [수정, 2026-09-17, PN-9CC66142] huge/4K 분기는 RingBufferDeleter
    // 안에 있다 - `data.reset()`이 그 삭제자를 그대로 부른다. 이
    // 프로젝트는 placement new/실제 소멸자를 안 쓰므로(그래서 이
    // 슬랩을 반납해도 ~RingBuffer()가 자동으로 안 불린다) 이 명시적
    // 호출이 없으면 outbound 버퍼 자체가 그대로 샌다. BridgePipe
    // 구조체 자신의 슬랩 메모리 반납은 kMakeShared의 기본 삭제자
    // (`kDestroyAndFree<BridgePipe>`)가 이 함수 호출 직후 이어서
    // 처리한다(Process::destroy()와 동일한 역할 분리).
    outbound.data.reset();
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

    // [신규, PN-CE6A04AB/SP-CA3C3E57 §5] 이 채널의 안전한 ChannelId를
    // 여기서 한 번만 발급한다 - OpenChannelHandler/KernelReservedTable
    // (livefs.cpp) 양쪽 호출부가 전부 이 함수를 거치므로 여기서 발급
    // 하면 두 소비자 모두 자동으로 새 인코딩을 쓰게 된다.
    channel->channelId = kAllocateChannelId(channel);
    if (channel->channelId == 0) {
        GenericSlabAllocator::free(mem, sizeof(Channel));
        *outError = ChannelError::ResourceExhausted;
        return nullptr;
    }

    if (nameLength > 0) {
        if (nameLength > kMaxNamedObjectNameLength) {
            kFreeChannelId(channel->channelId);  // 슬롯 누수 방지
            GenericSlabAllocator::free(mem, sizeof(Channel));
            *outError = ChannelError::NameInUse;  // 길이 초과도 "사용 불가"로 뭉뚱그림 - 세분화 불필요
            return nullptr;
        }
        if (!NamedObjectTable::reserve(name, nameLength, NamedObjectKind::Channel,
                                        reinterpret_cast<uint64_t>(channel))) {
            kFreeChannelId(channel->channelId);  // 슬롯 누수 방지
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
    // [수정, 2026-09-17, PN-9CC66142] peer가 이제 WeakPtr - 상대가
    // 이미 자기 프로세스의 openBridges에서 빠져 반납됐으면(프로세스
    // 종료 등) lock()이 빈 값을 반환한다, 그러면 깨울 대상 자체가
    // 없으므로 조용히 건너뛴다.
    SharedPtr<BridgePipe> peer = closed->peer.lock();
    if (peer) {
        SpinlockGuard guard(peer->outbound.lock);
        for (AsyncTask* t = peer->outbound.pendingWriters.popFront(); t; t = peer->outbound.pendingWriters.popFront()) {
            woken.pushBack(t);
        }
    }
    return woken;
}

// [수정, 2026-09-17, PN-9CC66142] peer.lock()이 실패하면(상대가 이미
// 자기 프로세스 종료 등으로 반납됨) 그 자체로 broken - closedLocal을
// 명시적으로 안 불렀어도 더 이상 상대에게 도달할 방법이 없다는 뜻은
// 같다.
bool kIsBridgeBroken(BridgePipe* bridge) {
    if (bridge->closedLocal) {
        return true;
    }
    SharedPtr<BridgePipe> peer = bridge->peer.lock();
    return !peer || peer->closedLocal;
}

// [신규, 2026-09-17, PN-9CC66142] "이 AsyncTask를 제출한 UserThread가
// 속한 Process"를 얻는 공용 체이닝 - AcceptFromChannelHandler가 accept
// 완료 시 양쪽(client/acceptor) Process에 BridgePipe 강한 참조를
// 나눠 주는 데 쓴다. `submitterTask`는 `Syscall::submit()`만 채우고
// (syscall.cpp) 그 계약 자체가 "반드시 UserThread 실행 흐름에서만
// 호출"이므로(syscall.h), `static_cast<UserThread*>`는 그 기존 계약을
// 그대로 재사용하는 것뿐이다(Syscall::submit 자신도 동일한 캐스트를
// 이미 쓴다). 제출자가 이미 죽었거나(WeakPtr 만료) Process가 이미
// 종료됐으면 빈 SharedPtr.
SharedPtr<Process> kProcessFromSubmitter(AsyncTask* task) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter) {
        return SharedPtr<Process>();
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return thread->process.lock();
}

// [신규, 2026-09-17, PN-9CC66142] `BridgeHandle`(유저가 syscall마다
// 넘기는 raw 값)을 호출자 자신의 `openBridges`에서 실제로 찾아
// 검증한다 - 예전처럼 아무 64비트 값이나 `reinterpret_cast`해 그대로
// 역참조하지 않는다(임의 포인터 역참조 보안 공백, DC-21647E46 로드맵
// 조사 중 발견). 찾으면 그 슬롯이 쥔 `SharedPtr<BridgePipe>`(=계속
// 살아있음을 보장)를, 못 찾으면(위조된 핸들, 남의 핸들, 이미 닫혀
// 목록에서 빠진 핸들) 빈 값을 반환한다.
SharedPtr<BridgePipe> kResolveOwnedBridge(AsyncTask* task, BridgeHandle handle) {
    SharedPtr<Process> process = kProcessFromSubmitter(task);
    if (!process) {
        return SharedPtr<BridgePipe>();
    }
    auto* rawTarget = reinterpret_cast<BridgePipe*>(handle);
    auto* slot = process->openBridges.find(
        [rawTarget](const SharedPtr<BridgePipe>& sp) { return sp.get() == rawTarget; });
    if (!slot) {
        return SharedPtr<BridgePipe>();
    }
    return slot->value;
}

// [신규, 2026-09-17, PN-B552E75F] `ChannelReadArgs::buffer`/
// `ChannelWriteArgs::data`(유저 포인터)를 역참조하기 전에 호출자
// 자신의 유저 주소공간에 실제로 속하는지 검증한다(SP-6BEAE0C1 §3이
// 도입한 `Paging::isUserRangeValid()`의 첫 소급 적용 - 이전까지
// Channel Read/Write 핸들러는 이 검증이 전혀 없었다, 임의 커널
// 메모리 읽기/쓰기로 이어질 수 있는 보안 공백).
//
// **주의(일반 기본 인자를 안 쓰는 이유)**: `isUserRangeValid()`의
// 기본 `pml4Phys=0`은 "현재 CR3"를 뜻하는데, 그 문서 주석은 "syscall
// 핸들러가 항상 제출자 자신의 컨텍스트에서 실행된다"고 가정한다 -
// 그런데 `onExec()`은 `AsyncReactor`가 나중에(리액터 자신의 스택 위,
// `Scheduler::runLoop()`의 idle 경로에서) 실행하므로 그 순간의 CR3가
// 반드시 이 syscall을 제출한 UserThread의 것이라는 보장이 없다
// (`task.h`의 `Task::userPml4Phys` 문서 주석 - "runLoop()의 idle
// 컨텍스트에서는 CR3 재동기화를 하지 않는다" - PN-DB5153B6/
// PN-9CC66142가 이미 다룬 것과 정확히 같은 문제 클래스). 그래서
// 기본값을 믿지 않고 `submitterTask.lock()` 체이닝으로 얻은 실제
// UserThread의 `userPml4Phys`를 명시적으로 넘긴다 - 제출자를 못
// 찾으면(이미 죽었거나 Process 없는 호출자) 안전한 쪽으로 실패.
bool kValidateUserBuffer(AsyncTask* task, const void* ptr, uint64_t length) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return Paging::isUserRangeValid(reinterpret_cast<uint64_t>(ptr), length, thread->userPml4Phys);
}

class OpenChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<OpenChannelArgs*>(argsRaw);
        Channel* channel = kCreateNamedChannel(args->name, args->nameLength, &args->error);
        if (!channel) {
            co_return;
        }
        // [수정, PN-CE6A04AB] 더 이상 raw 포인터가 아니다 - channel->
        // channelId는 kCreateNamedChannel()이 이미 안전하게 발급해 둔
        // 값이다(channel.h §ChannelId 주석 참고).
        channel->ownerProcess = DontDeref<Process>(kProcessFromSubmitter(task).get());
        args->channelId = channel->channelId;
        args->channelHandle = args->channelId;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // [검토 완료, PN-C4611402] 이 onExec은 co_await/yield 지점이 전혀
    // 없어 항상 한 번에 끝까지 실행된다 - 그래서 취소는 오직 "이
    // AsyncTask가 리액터에서 한 번도 실행되기 전"에만 일어날 수 있고,
    // 그 시점엔 Channel이 아직 만들어지지도 않았다(onExec 안에서 처음
    // 만들어짐) - 정리할 자원이 없다. no-op 유지가 맞음.
    void onCancel(AsyncTask*, void*) override {}
};

class ConnectChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ConnectChannelArgs*>(argsRaw);

        Channel* channel = nullptr;
        if (args->target != 0) {
            // [수정, PN-CE6A04AB] 유저가 준 target을 더 이상 직접
            // reinterpret_cast하지 않는다 - 위조/이미 소멸된 값이면
            // 안전하게 NotFound.
            channel = kResolveChannelId(args->target);
            if (!channel) {
                args->error = ChannelError::NotFound;
                co_return;
            }
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
    // [구현, PN-C4611402] SP-1FBC0EEB가 명시한 "connectChannel 취소:
    // 그 Channel의 대기열에서 자신의 PendingConnectRequest를 제거"를
    // 실제로 수행한다 - 이전엔 no-op이라 `&req`(취소되면 코루틴 스택과
    // 함께 곧 반납될 지역 변수)가 channel->pendingConnects에 댕글링
    // 포인터로 남아, 다음 acceptFromChannel의 popPendingConnect()가
    // 그걸 꺼내 역참조하면 UAF였다. onExec과 동일한 방법으로 Channel을
    // 재조회한다(target 우선, 없으면 name) - args는 onExec에 넘겼던
    // 바로 그 포인터라 여기서도 안전하게 다시 읽을 수 있다(onCancel도
    // onExec/onFailure와 동일하게 args에 접근 가능 - async_task.h
    // AsyncTaskHandler::onCancel 문서 참고).
    void onCancel(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ConnectChannelArgs*>(argsRaw);
        Channel* channel = nullptr;
        if (args->target != 0) {
            channel = kResolveChannelId(args->target);  // [수정, PN-CE6A04AB]
        } else if (args->nameLength > 0) {
            NamedObjectKind kind{};
            uint64_t objectId = 0;
            if (NamedObjectTable::resolve(args->name, args->nameLength, &kind, &objectId) &&
                kind == NamedObjectKind::Channel) {
                channel = reinterpret_cast<Channel*>(objectId);
            }
        }
        if (!channel) {
            // 이름이 이미 해제됐다(=destroyChannel이 먼저 실행돼 이름
            // 해제까지 끝났다는 뜻 - 그 경로가 pendingConnects 전체를
            // 이미 비우고 깨웠으므로 더 할 일 없음) 또는 target==0인
            // 채로 취소된 경우.
            return;
        }
        SpinlockGuard guard(channel->lock);
        if (channel->destroyed) {
            return;  // destroyChannel이 이미 pendingConnects 전체를 비우고 깨웠음
        }
        channel->removePendingConnect(task);
    }
};

class AcceptFromChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<AcceptFromChannelArgs*>(argsRaw);
        // [수정, PN-CE6A04AB] 더 이상 검증 없이 역참조하지 않는다.
        auto* channel = kResolveChannelId(args->channelHandle);
        if (!channel) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        // [신규, PN-CE6A04AB/SP-CA3C3E57 §6] 소유자만 accept할 수 있다 -
        // ownerProcess==nullptr(커널이 만든 예약 채널, livefs.cpp)는
        // 예외적으로 무제한 허용(SP-9A6D579F §3.2의 "커널 자신은 예외"
        // 와 같은 패턴, 실사용처 없음 - RM-C65F7760 참고).
        if (channel->ownerProcess) {
            SharedPtr<Process> caller = kProcessFromSubmitter(task);
            if (!caller || DontDeref<Process>(caller.get()) != channel->ownerProcess) {
                args->error = ChannelError::PermissionDenied;
                co_return;
            }
        }

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

            SharedPtr<BridgePipe> serverSide;
            SharedPtr<BridgePipe> clientSide;
            if (!BridgePipe::createPair(req->useHugePage, &serverSide, &clientSide)) {
                req->rejected = true;
                req->done = true;
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }

            // [신규, 2026-09-17, PN-9CC66142] "이용자 객체가 양쪽에
            // 매달려야 한다"는 설계자 답변 그대로 - client 쪽은
            // `req->task`(ConnectChannel을 제출한 AsyncTask, submitterTask
            // 를 이미 들고 있음)로, acceptor 쪽은 이 accept 핸들러 자신의
            // `task`로 각각 제출자 Process를 얻는다. 어느 한쪽이라도
            // 못 얻으면(제출자가 이미 죽었거나 커널 서비스처럼 Process가
            // 없는 호출자 - §6, 아직 실사용처 없음) 거절한다 -
            // serverSide/clientSide는 지역 SharedPtr이라 그냥 스코프를
            // 벗어나면서 스스로 정리된다(별도 롤백 코드 불필요).
            SharedPtr<Process> clientProcess = kProcessFromSubmitter(req->task);
            SharedPtr<Process> acceptorProcess = kProcessFromSubmitter(task);
            if (!clientProcess || !acceptorProcess) {
                req->rejected = true;
                req->done = true;
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }

            clientProcess->openBridges.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
            acceptorProcess->openBridges.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
            auto* clientSlot = clientProcess->openBridges.insert(clientSide);
            if (!clientSlot) {
                // 극히 드문 목록 슬랩 고갈 - serverSide/clientSide 지역
                // SharedPtr이 스코프 종료 시 스스로 정리된다(아직 어느
                // 프로세스의 openBridges에도 안 들어갔으므로 되돌릴 것도
                // 없다).
                req->rejected = true;
                req->done = true;
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }
            if (!acceptorProcess->openBridges.insert(serverSide)) {
                // client 쪽엔 이미 넣었으니 반드시 되돌린다 - 안 그러면
                // 이 실패한 accept로 client 프로세스에만 "고아
                // BridgePipe"(아무도 handle을 모르는 채로 강한 참조만
                // 살아있는) 한 짐이 남는다.
                clientProcess->openBridges.erase(clientSlot);
                req->rejected = true;
                req->done = true;
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }

            req->resultBridge = clientSide.get();
            req->done = true;
            AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);

            args->bridge = reinterpret_cast<uint64_t>(serverSide.get());
            args->error = ChannelError::None;
            co_return;
        }
    }
    void onFailure(AsyncTask*) override {}
    // [정정, PN-C4611402] SP-1FBC0EEB의 "acceptFromChannel 취소: 별도
    // 정리 없음(대기열은 그대로)"는 실제 자료구조와 맞지 않았다 - 이
    // 큐(channel->pendingAccepters)에 매다는 건 다른 무언가가 아니라
    // **취소되면 곧 반납될 이 AsyncTask 자기 자신**이다. no-op이면
    // 반납된 AsyncTask가 댕글링 포인터로 남아, 나중에 connectChannel이
    // popFront()로 그걸 꺼내 AsyncReactor::submitCompletion()에
    // 넘기면 UAF다(SP-1FBC0EEB §"취소/실패 처리"에 정정 각주 추가함).
    void onCancel(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<AcceptFromChannelArgs*>(argsRaw);
        auto* channel = kResolveChannelId(args->channelHandle);  // [수정, PN-CE6A04AB]
        if (!channel) {
            return;
        }
        SpinlockGuard guard(channel->lock);
        if (channel->destroyed) {
            return;  // destroyChannel이 이미 pendingAccepters 전체를 비우고 깨웠음
        }
        channel->pendingAccepters.remove(task);
    }
};

class ChannelReadHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ChannelReadArgs*>(argsRaw);
        // [수정, 2026-09-17, PN-9CC66142] args->bridge를 그대로
        // reinterpret_cast하지 않는다 - 호출자 자신의 openBridges에서
        // 실제로 찾아야만 유효하다(임의 포인터 역참조 보안 공백 방지).
        SharedPtr<BridgePipe> bridge = kResolveOwnedBridge(task, args->bridge);
        if (!bridge) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        SharedPtr<BridgePipe> peer = bridge->peer.lock();
        if (!peer) {
            args->error = ChannelError::BrokenPipe;  // 상대가 이미 반납됨(프로세스 종료 등) - closedLocal 여부와 무관하게 broken
            co_return;
        }
        // [신규, 2026-09-17, PN-B552E75F] 실제로 버퍼에 쓰기 전에 그
        // 포인터가 호출자 자신의 유저 주소공간에 속하는지 검증.
        if (!kValidateUserBuffer(task, args->buffer, args->maxLength)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        RingBuffer& ring = peer->outbound;  // 내가 읽는 대상 = 상대가 쓰는 곳

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
                } else if (kIsBridgeBroken(bridge.get())) {
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
    // [구현, PN-C4611402] SP-1FBC0EEB는 read/write 취소 시 "이미 만들어진
    // BridgePipe는 그대로 유지"만 명시했지만, 실제 코드는 대기 중일 때
    // 이 AsyncTask 자신을 `ring.pendingReaders`에 매달아 둔다 -
    // pendingAccepters와 완전히 같은 모양의 댕글링 포인터 위험(취소되면
    // 곧 반납될 이 task가 그 큐에 남아, 나중에 write()가 popFront()로
    // 꺼내 깨우면 UAF). BridgePipe 자체의 수명(Process::openBridges가
    // 강한 소유)과는 별개의 문제라 이 정리도 별도로 필요하다.
    void onCancel(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ChannelReadArgs*>(argsRaw);
        SharedPtr<BridgePipe> bridge = kResolveOwnedBridge(task, args->bridge);
        if (!bridge) {
            return;
        }
        SharedPtr<BridgePipe> peer = bridge->peer.lock();
        if (!peer) {
            return;
        }
        SpinlockGuard guard(peer->outbound.lock);
        peer->outbound.pendingReaders.remove(task);
    }
};

class ChannelWriteHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ChannelWriteArgs*>(argsRaw);
        // [수정, 2026-09-17, PN-9CC66142] ChannelReadHandler와 동일한
        // 이유(위 참고) - 호출자의 openBridges에서 검증된 핸들만 쓴다.
        SharedPtr<BridgePipe> bridge = kResolveOwnedBridge(task, args->bridge);
        if (!bridge) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        // [신규, 2026-09-17, PN-B552E75F] 실제로 버퍼를 읽기 전에 그
        // 포인터가 호출자 자신의 유저 주소공간에 속하는지 검증
        // (ChannelReadHandler와 동일한 이유, 위 kValidateUserBuffer 참고).
        if (!kValidateUserBuffer(task, args->data, args->length)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        RingBuffer& ring = bridge->outbound;  // 내가 쓰는 대상 = 상대가 읽는 곳

        for (;;) {
            AsyncTask* wakeReader = nullptr;
            bool done = false;
            {
                SpinlockGuard guard(ring.lock);
                const uint64_t space = ring.capacity - ring.used;
                if (kIsBridgeBroken(bridge.get())) {
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
    // [구현, PN-C4611402] ChannelReadHandler::onCancel과 대칭 - 대기
    // 중이던 이 task를 `bridge->outbound.pendingWriters`에서 제거한다
    // (같은 댕글링 포인터 위험, 위 ChannelReadHandler 주석 참고).
    void onCancel(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ChannelWriteArgs*>(argsRaw);
        SharedPtr<BridgePipe> bridge = kResolveOwnedBridge(task, args->bridge);
        if (!bridge) {
            return;
        }
        SpinlockGuard guard(bridge->outbound.lock);
        bridge->outbound.pendingWriters.remove(task);
    }
};

class CloseBridgeHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<CloseBridgeArgs*>(argsRaw);
        // [수정, 2026-09-17, PN-9CC66142] 다른 핸들러와 동일한 이유로
        // 호출자의 openBridges에서 검증한다. 이미 닫혀 목록에서 빠진
        // 핸들로 다시 closeBridge를 부르면 여기서 InvalidHandle이
        // 나온다 - 예전의 "closedLocal==true면 멱등 처리" 분기는
        // 이제 도달 불가능해졌다(같은 핸들이 살아있는 채로 closedLocal
        // 만 true인 상태가 없다 - 아래에서 closedLocal을 세우는 것과
        // openBridges에서 빼는 것을 같은 호출 안에서 함께 하므로).
        SharedPtr<BridgePipe> bridge = kResolveOwnedBridge(task, args->bridge);
        if (!bridge) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        bridge->closedLocal = true;

        AsyncTaskWaitQueue woken = kWakeForClose(bridge.get());
        for (AsyncTask* t = woken.popFront(); t; t = woken.popFront()) {
            AsyncReactor::submitCompletion(t);
        }

        // [수정, 2026-09-17, PN-9CC66142] 반납은 이제 참조 카운팅이
        // 담당한다 - "양쪽 다 closedLocal"을 기다리지 않는다. 호출자
        // 자신의 openBridges에서 이 슬롯을 지워 자기 몫의 강한 참조를
        // 내려놓으면, 상대(peer) 쪽이 아직 자기 몫을 들고 있는 한
        // BridgePipe 객체는 안전하게 살아있다(peer.lock() 계속 유효) -
        // 상대도 이미 닫아 자기 몫을 내려놨다면 두 객체 다 자연히
        // destroy()까지 끝난다. 별도 destroyPair() 호출이 필요 없다.
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (process) {
            auto* rawTarget = bridge.get();
            auto* slot =
                process->openBridges.find([rawTarget](const SharedPtr<BridgePipe>& sp) { return sp.get() == rawTarget; });
            if (slot) {
                process->openBridges.erase(slot);
            }
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // [검토 완료, PN-C4611402] OpenChannelHandler와 동일한 이유 - 이
    // onExec도 co_await/yield 없이 한 번에 끝까지 실행되므로 취소는
    // "실행되기 전"에만 가능하고, 그 시점엔 closedLocal도 안 세워졌고
    // openBridges에서도 안 빠졌다 - 정리할 자원이 없다. no-op 유지.
    void onCancel(AsyncTask*, void*) override {}
};

class DestroyChannelHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DestroyChannelArgs*>(argsRaw);
        // [수정, PN-CE6A04AB] 더 이상 검증 없이 역참조하지 않는다.
        auto* channel = kResolveChannelId(args->channelHandle);
        if (!channel) {
            args->error = ChannelError::NotFound;  // 기존엔 널체크조차 없었음
            co_return;
        }
        // [신규, PN-CE6A04AB/SP-CA3C3E57 §6] 소유자만 destroy할 수
        // 있다 - AcceptFromChannelHandler와 동일한 예외(ownerProcess==
        // nullptr는 커널 예약 채널).
        if (channel->ownerProcess) {
            SharedPtr<Process> caller = kProcessFromSubmitter(task);
            if (!caller || DontDeref<Process>(caller.get()) != channel->ownerProcess) {
                args->error = ChannelError::PermissionDenied;
                co_return;
            }
        }

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
        kFreeChannelId(channel->channelId);  // [신규, PN-CE6A04AB] 슬롯 해제
        GenericSlabAllocator::free(channel, sizeof(Channel));
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // [검토 완료, PN-C4611402] 위 CloseBridgeHandler/OpenChannelHandler와
    // 동일한 이유 - co_await/yield 없이 한 번에 끝까지 실행되므로
    // 취소는 실행 전에만 가능하고 그 시점엔 아무 자원도 안 건드렸다.
    // no-op 유지.
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
