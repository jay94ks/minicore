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
// 취약점(channel.h 상단 주석 참고)을 막는다.
//
// [갱신, 2026-09-22, PN-260D7D73] 슬롯이 이제 raw `Channel*` 대신
// `SharedPtr<Channel>`을 담는다 - `Channel`이 `BridgePipe`와 동일하게
// `kMakeShared`로 관리되면서, 이 슬롯이 그 유일한 강한 소유자가 됐다
// (아래 `kCreateNamedChannel`/`kFreeChannelId` 참고). 세대 태그는
// 여전히 그대로 필요하다 - SharedPtr 자체는 "같은 슬롯을 나중에
// 재사용한 다른 Channel"과 "예전 그 Channel"을 구분해 주지 않으므로
// (둘 다 그 시점엔 각자 유효한 SharedPtr이다), 세대 불일치로 낡은
// ChannelId를 걸러내는 역할은 SharedPtr 도입과 무관하게 유지된다.
// **조회(`kResolveChannelId`)가 이제 `gChannelTableLock`을 짧게
// 잡는다** - 슬롯의 `SharedPtr<Channel>`을 복사(참조카운트 증가)해
// 돌려주려면 그 슬롯을 동시에 지우는 `kFreeChannelId`와 경쟁하면 안
// 되기 때문(이전엔 raw 포인터 값만 읽으면 됐어서 락이 필요 없었다) -
// 임계구역이 인덱스 범위/세대 비교+포인터 복사뿐이라 매우 짧다.
struct ChannelTableSlot {
    SharedPtr<Channel> ptr;
    uint32_t generation = 0;
};

constexpr uint32_t kMaxChannelTableSlots = 65536;  // [확정, 2026-09-17,
// QU-1AF2C16B 답변] "채널의 전역 상한은 64K".
ChannelTableSlot gChannelTable[kMaxChannelTableSlots];
Spinlock gChannelTableLock;  // 발급/해제/조회 전부 이 락으로 보호(위 갱신 절 참고).

ChannelId kAllocateChannelId(const SharedPtr<Channel>& channel) {
    SpinlockGuard guard(gChannelTableLock);
    for (uint32_t i = 0; i < kMaxChannelTableSlots; ++i) {
        if (!gChannelTable[i].ptr) {
            gChannelTable[i].generation++;
            gChannelTable[i].ptr = channel;
            return (static_cast<uint64_t>(gChannelTable[i].generation) << 32) | i;
        }
    }
    return 0;  // 슬롯 고갈 - 호출부가 ResourceExhausted로 매핑
}

// 안전 해석 - 이 함수를 거치지 않고는 어디서도 유저 제공 ChannelId를
// Channel로 캐스팅하지 않는다. 유저가 어떤 값을 넘기든 인덱스 범위
// 검사 + 세대 일치 확인만으로 끝난다. [갱신, 2026-09-22, PN-260D7D73]
// 반환값이 `SharedPtr<Channel>`로 바뀌어, 이 호출이 끝난 뒤에도 이
// 반환값을 들고 있는 동안은(스코프를 벗어날 때까지) 다른 코어의
// 동시 `DestroyChannel`이 그 메모리를 해제할 수 없다 - 이전엔 raw
// 포인터만 돌려줘, `AcceptFromChannelHandler`의 `AsyncTask::yield()`
// 대기 루프처럼 resolve 이후 다시 역참조하는 지점에서 이론상 실제
// use-after-free 경합이 가능했다.
SharedPtr<Channel> kResolveChannelId(ChannelId id) {
    if (id == 0) {
        return SharedPtr<Channel>();
    }
    const uint32_t index = static_cast<uint32_t>(id & 0xFFFFFFFFu);
    const uint32_t generation = static_cast<uint32_t>(id >> 32);
    if (index >= kMaxChannelTableSlots) {
        return SharedPtr<Channel>();
    }
    SpinlockGuard guard(gChannelTableLock);
    ChannelTableSlot& slot = gChannelTable[index];
    if (slot.generation != generation || !slot.ptr) {
        return SharedPtr<Channel>();
    }
    return slot.ptr;
}

// DestroyChannelHandler::onExec 안, 이름 해제 직후에 호출한다 - id
// 자체에서 인덱스를 역산하므로 O(1). [갱신, 2026-09-22, PN-260D7D73]
// 더 이상 직접 GenericSlabAllocator::free를 부르지 않는다 - 이 슬롯의
// SharedPtr을 비워(강한 참조 하나 반납) 테이블 쪽 소유권만 내려놓는다.
// 호출부(DestroyChannelHandler::onExec)가 이미 자기 몫의
// SharedPtr<Channel> 지역 변수를 들고 있으므로, 그 함수가 반환할 때
// (또는 그 사이 다른 동시 호출자가 자기 몫을 먼저 반납할 때) 마지막
// 강한 참조가 사라지는 순간 실제 반납(kDestroyAndFree<Channel>)이
// 일어난다.
void kFreeChannelId(ChannelId id) {
    if (id == 0) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(id & 0xFFFFFFFFu);
    if (index >= kMaxChannelTableSlots) {
        return;
    }
    SpinlockGuard guard(gChannelTableLock);
    gChannelTable[index].ptr.reset();  // generation은 그대로 - 다음 재사용 때 +1
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
SharedPtr<Channel> kCreateNamedChannel(const char* name, uint64_t nameLength, ChannelError* outError) {
    // [갱신, 2026-09-22, PN-260D7D73] `BridgePipe::createPair()`와 동일한
    // 패턴 - raw slab 메모리에 명시적으로 `init()`한 뒤 `kMakeShared`로
    // 감싼다. `kMakeShared` 실패(컨트롤 블록 슬랩 고갈, 극히 드묾)는
    // `preConstructed`를 건드리지 않으므로 여기서 직접 롤백한다.
    void* mem = GenericSlabAllocator::alloc(sizeof(Channel));
    if (!mem) {
        *outError = ChannelError::ResourceExhausted;
        return SharedPtr<Channel>();
    }
    auto* raw = reinterpret_cast<Channel*>(mem);
    raw->init();

    SharedPtr<Channel> channel = kMakeShared<Channel>(raw);
    if (!channel) {
        GenericSlabAllocator::free(mem, sizeof(Channel));
        *outError = ChannelError::ResourceExhausted;
        return SharedPtr<Channel>();
    }

    // [신규, PN-CE6A04AB/SP-CA3C3E57 §5] 이 채널의 안전한 ChannelId를
    // 여기서 한 번만 발급한다 - OpenChannelHandler/KernelReservedTable
    // (livefs.cpp) 양쪽 호출부가 전부 이 함수를 거치므로 여기서 발급
    // 하면 두 소비자 모두 자동으로 새 인코딩을 쓰게 된다. [갱신,
    // 2026-09-22, PN-260D7D73] 실패 시 로컬 SharedPtr이 스코프를
    // 벗어나며 스스로 정리된다(kMakeShared 성공 후엔 raw 슬랩을 직접
    // free하면 이중 해제가 되므로 절대 하지 않는다).
    channel->channelId = kAllocateChannelId(channel);
    if (channel->channelId == 0) {
        *outError = ChannelError::ResourceExhausted;
        return SharedPtr<Channel>();
    }

    if (nameLength > 0) {
        if (nameLength > kMaxNamedObjectNameLength) {
            kFreeChannelId(channel->channelId);  // 슬롯 누수 방지
            *outError = ChannelError::NameInUse;  // 길이 초과도 "사용 불가"로 뭉뚱그림 - 세분화 불필요
            return SharedPtr<Channel>();
        }
        // [갱신, 2026-09-22, PN-260D7D73] objectId로 더 이상 raw
        // 포인터를 저장하지 않는다 - `ConnectChannelHandler`의 이름
        // 기반 조회(onExec/onCancel)가 이 값을 검증 없이 그대로
        // `reinterpret_cast<Channel*>`하던 것과 완전히 같은 종류의
        // 취약점(channel.h 상단 주석의 ChannelId 정정과 동일한 이유)
        // 이라, 여기서도 `channelId`(불투명 핸들)만 저장한다 - 소비부
        // (ConnectChannelHandler)도 `kResolveChannelId()`를 거치도록
        // 함께 고쳤다.
        if (!NamedObjectTable::reserve(name, nameLength, NamedObjectKind::Channel, channel->channelId)) {
            kFreeChannelId(channel->channelId);  // 슬롯 누수 방지
            *outError = ChannelError::NameInUse;
            return SharedPtr<Channel>();
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

// [제거, 2026-09-20, SP-43331889 §3-1] 예전엔 여기 "이 AsyncTask를
// 제출한 UserThread가 속한 Process"를 얻는 `kProcessFromSubmitter(AsyncTask*)`
// 가 있었다(PN-9CC66142) - `submitter`를 무조건 `static_cast<UserThread*>`
// 해 Process 없는 KernelThread 제출자(devmgr/fs, §1 확정)가 생기면
// 잘못된 캐스팅이 되는 문제가 있었고, 이 파일의 모든 호출부가 이제
// `task->submitterTask.lock()` + 아래 `kOwnerOpenBridgesOf`(BridgePipe
// 소유권)/`channel.h`의 `owner` 필드(채널 소유권)로 옮겨가 완전히
// 대체됐다 - 제거.

// [신규, 2026-09-20, SP-43331889 §3-1] "이 Task가 자신이 연 BridgePipe를
// 걸어 두는 핸들 테이블"을 통일해서 얻는다 - `kOwnerProcessOf(Task*)`
// (process.h §4)와 정확히 같은 이유/패턴: `Process` 소속 UserThread는
// `Process::openBridges`를, Process 없는 KernelThread(devmgr/fs, §1)는
// 자기 자신의 `KernelThread::openBridges`를 직접 쓴다. 두 컨테이너의
// 청크 용량이 어긋나면 아래 타입이 안전하지 않으므로 컴파일 타임에
// 강제한다.
using OpenBridgeList = ChunkedList<SharedPtr<BridgePipe>, Process::kMaxOpenBridgesChunkCapacity>;
static_assert(Process::kMaxOpenBridgesChunkCapacity == KernelThread::kMaxOpenBridgesChunkCapacity,
              "Process::openBridges와 KernelThread::openBridges의 청크 용량이 일치해야 한다");

OpenBridgeList* kOwnerOpenBridgesOf(Task* task) {
    if (!task) {
        return nullptr;
    }
    if (task->isUserLevel) {
        SharedPtr<Process> process = static_cast<UserThread*>(task)->process.lock();
        if (!process) {
            return nullptr;
        }
        process->openBridges.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        return &process->openBridges;
    }
    if (task->isKernelMode) {
        auto* kernelThread = static_cast<KernelThread*>(task);
        kernelThread->openBridges.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        return &kernelThread->openBridges;
    }
    return nullptr;  // 순수 커널 전용 Task(idle/리액터 등) - 자원 소유 없음
}

// [신규, 2026-09-17, PN-9CC66142] `BridgeHandle`(유저가 syscall마다
// 넘기는 raw 값)을 호출자 자신의 `openBridges`에서 실제로 찾아
// 검증한다 - 예전처럼 아무 64비트 값이나 `reinterpret_cast`해 그대로
// 역참조하지 않는다(임의 포인터 역참조 보안 공백, DC-21647E46 로드맵
// 조사 중 발견). 찾으면 그 슬롯이 쥔 `SharedPtr<BridgePipe>`(=계속
// 살아있음을 보장)를, 못 찾으면(위조된 핸들, 남의 핸들, 이미 닫혀
// 목록에서 빠진 핸들) 빈 값을 반환한다.
SharedPtr<BridgePipe> kResolveOwnedBridge(AsyncTask* task, BridgeHandle handle) {
    // [갱신, 2026-09-20, SP-43331889 §3-1] Process 전용
    // kProcessFromSubmitter 대신 kOwnerOpenBridgesOf로 - 제출자가
    // Process 없는 KernelThread(devmgr/fs)여도 자기 몫의 openBridges를
    // 그대로 찾는다.
    SharedPtr<Task> submitter = task->submitterTask.lock();
    OpenBridgeList* bridges = kOwnerOpenBridgesOf(submitter.get());
    if (!bridges) {
        return SharedPtr<BridgePipe>();
    }
    auto* rawTarget = reinterpret_cast<BridgePipe*>(handle);
    auto* slot = bridges->find([rawTarget](const SharedPtr<BridgePipe>& sp) { return sp.get() == rawTarget; });
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
        // [신규, 2026-09-17, PN-EAB3A9AE 착수 중 실측 발견] 이 syscall만
        // 유일하게 name/nameLength가 kValidateUserBuffer 검증 없이 곧바로
        // kCreateNamedChannel() -> NamedObjectTable::reserve()/memcpy로
        // 역참조되고 있었다 - ChannelRead/Write가 PN-B552E75F로 이미
        // 겪은 것과 같은 종류의 보안 공백(임의 커널 메모리 읽기로 이어질
        // 수 있음)이 Open/Connect 두 곳은 그때 놓쳤다. 이 프로젝트 최초의
        // 진짜 유저랜드 Channel 호출부(pubreg, PN-EAB3A9AE)를 준비하다가
        // 코드 감사로 발견했다.
        if (args->nameLength > 0 && !kValidateUserBuffer(task, args->name, args->nameLength)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        SharedPtr<Channel> channel = kCreateNamedChannel(args->name, args->nameLength, &args->error);
        if (!channel) {
            co_return;
        }
        // [수정, PN-CE6A04AB] 더 이상 raw 포인터가 아니다 - channel->
        // channelId는 kCreateNamedChannel()이 이미 안전하게 발급해 둔
        // 값이다(channel.h §ChannelId 주석 참고).
        // [갱신, 2026-09-20, SP-43331889 §3-1] Process가 아니라 제출자
        // Task 자신의 신원을 직접 기록한다(channel.h의 `owner` 문서
        // 주석 참고) - devmgr/fs(KernelThread, Process 없음)가 연
        // 채널도 진짜 소유자로 식별되게 하기 위함.
        channel->owner = DontDeref<Task>(task->submitterTask.lock().get());
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

        SharedPtr<Channel> channel;
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
            // [신규, 2026-09-17, PN-EAB3A9AE 착수 중 실측 발견 - 위
            // OpenChannelHandler와 동일한 이유] name/nameLength도
            // NamedObjectTable::resolve()에 넘기기 전에 검증한다.
            if (!kValidateUserBuffer(task, args->name, args->nameLength)) {
                args->error = ChannelError::InvalidPointer;
                co_return;
            }
            NamedObjectKind kind{};
            uint64_t objectId = 0;
            if (!NamedObjectTable::resolve(args->name, args->nameLength, &kind, &objectId) ||
                kind != NamedObjectKind::Channel) {
                args->error = ChannelError::NotFound;  // 종류가 달라도 그냥 "못 찾음"(종류 은닉)
                co_return;
            }
            // [갱신, 2026-09-22, PN-260D7D73] objectId는 이제 raw
            // 포인터가 아니라 ChannelId다(kCreateNamedChannel 참고) -
            // target==id 경로와 완전히 동일하게 kResolveChannelId()를
            // 거친다(같은 이유: 위조/이미 소멸된 값 방어 + use-after-free
            // 방지, channel.h 상단 주석 참고).
            channel = kResolveChannelId(static_cast<ChannelId>(objectId));
            if (!channel) {
                args->error = ChannelError::NotFound;
                co_return;
            }
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

        while (req.done.load() == 0) {
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
        // [신규, 2026-09-18, PN-B5C2845A] 이 취소가 무엇 때문이든
        // (Kill 등) 호출자가 나중에 args->error로 원인을 알 수 있게
        // 항상 먼저 채운다(설계자 답변 "얘들을 실패시키면 되잖아",
        // QU-8E137FFD) - 아래 정리 로직의 성공/실패와 무관.
        args->error = ChannelError::Interrupted;
        SharedPtr<Channel> channel;
        if (args->target != 0) {
            channel = kResolveChannelId(args->target);  // [수정, PN-CE6A04AB]
        } else if (args->nameLength > 0) {
            NamedObjectKind kind{};
            uint64_t objectId = 0;
            if (NamedObjectTable::resolve(args->name, args->nameLength, &kind, &objectId) &&
                kind == NamedObjectKind::Channel) {
                // [갱신, 2026-09-22, PN-260D7D73] onExec과 동일 - objectId는
                // 이제 ChannelId다.
                channel = kResolveChannelId(static_cast<ChannelId>(objectId));
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
        // [갱신, 2026-09-22, PN-260D7D73] 이 SharedPtr을 onExec() 함수
        // 스코프 내내(아래 for(;;) 루프의 AsyncTask::yield() 대기까지
        // 포함) 그대로 들고 있는다 - 예전엔 raw Channel*라 그 사이
        // 다른 코어의 DestroyChannel이 이 메모리를 해제하면 다음
        // 루프에서 channel->lock을 다시 잡는 순간 use-after-free였다.
        SharedPtr<Channel> channel = kResolveChannelId(args->channelHandle);
        if (!channel) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        // [신규, PN-CE6A04AB/SP-CA3C3E57 §6] 소유자만 accept할 수 있다 -
        // owner==nullptr(커널이 만든 예약 채널, livefs.cpp)는 예외적으로
        // 무제한 허용(SP-9A6D579F §3.2의 "커널 자신은 예외"와 같은
        // 패턴, 실사용처 없음 - RM-C65F7760 참고). [갱신, 2026-09-20,
        // SP-43331889 §3-1] Process 동일성 대신 제출자 Task 동일성으로
        // 비교(channel.h의 `owner` 문서 주석 참고) - devmgr/fs(KernelThread)
        // 가 연 채널도 자기 자신 말고는 accept 못 하게 정확히 지켜진다.
        if (channel->owner) {
            SharedPtr<Task> caller = task->submitterTask.lock();
            if (!caller || DontDeref<Task>(caller.get()) != channel->owner) {
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
                req->done.store(1);
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }

            // [신규, 2026-09-17, PN-9CC66142] "이용자 객체가 양쪽에
            // 매달려야 한다"는 설계자 답변 그대로 - client 쪽은
            // `req->task`(ConnectChannel을 제출한 AsyncTask, submitterTask
            // 를 이미 들고 있음)로, acceptor 쪽은 이 accept 핸들러 자신의
            // `task`로 각각 제출자를 얻는다. [갱신, 2026-09-20,
            // SP-43331889 §3-1] Process 전용 kProcessFromSubmitter 대신
            // kOwnerOpenBridgesOf로 - devmgr/fs(KernelThread, Process
            // 없음)가 acceptor여도 자기 몫의 openBridges를 정상적으로
            // 얻는다(이 경로가 그 첫 실사용처가 됐다 - 예전 주석의
            // "아직 실사용처 없음"은 이제 사실이 아니다). 어느 한쪽이라도
            // 못 얻으면(제출자가 이미 죽었거나, 순수 커널 전용 Task처럼
            // 애초에 자원을 못 갖는 호출자) 거절한다 - serverSide/
            // clientSide는 지역 SharedPtr이라 그냥 스코프를 벗어나면서
            // 스스로 정리된다(별도 롤백 코드 불필요).
            SharedPtr<Task> clientTask = req->task->submitterTask.lock();
            SharedPtr<Task> acceptorTask = task->submitterTask.lock();
            OpenBridgeList* clientBridges = kOwnerOpenBridgesOf(clientTask.get());
            OpenBridgeList* acceptorBridges = kOwnerOpenBridgesOf(acceptorTask.get());
            if (!clientBridges || !acceptorBridges) {
                req->rejected = true;
                req->done.store(1);
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }

            auto* clientSlot = clientBridges->insert(clientSide);
            if (!clientSlot) {
                // 극히 드문 목록 슬랩 고갈 - serverSide/clientSide 지역
                // SharedPtr이 스코프 종료 시 스스로 정리된다(아직 어느
                // 쪽 openBridges에도 안 들어갔으므로 되돌릴 것도 없다).
                req->rejected = true;
                req->done.store(1);
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }
            if (!acceptorBridges->insert(serverSide)) {
                // client 쪽엔 이미 넣었으니 반드시 되돌린다 - 안 그러면
                // 이 실패한 accept로 client 쪽에만 "고아 BridgePipe"
                // (아무도 handle을 모르는 채로 강한 참조만 살아있는)
                // 한 짐이 남는다.
                clientBridges->erase(clientSlot);
                req->rejected = true;
                req->done.store(1);
                AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }

            req->resultBridge = clientSide.get();
            req->done.store(1);
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
        // [신규, 2026-09-18, PN-B5C2845A] ConnectChannelHandler::onCancel과 동일한 이유(위 참고).
        args->error = ChannelError::Interrupted;
        SharedPtr<Channel> channel = kResolveChannelId(args->channelHandle);  // [수정, PN-CE6A04AB]
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
        // [신규, 2026-09-18, PN-B5C2845A] ConnectChannelHandler::onCancel과 동일한 이유(위 참고).
        args->error = ChannelError::Interrupted;
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
        // [신규, 2026-09-18, PN-B5C2845A] ConnectChannelHandler::onCancel과 동일한 이유(위 참고).
        args->error = ChannelError::Interrupted;
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
        SharedPtr<Task> caller = task->submitterTask.lock();
        if (!caller) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        kCloseBridgeSync(caller, args->bridge, &args->error);
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
        // [갱신, 2026-09-22, PN-260D7D73] 이 지역 SharedPtr이 아래에서
        // kFreeChannelId()로 테이블 쪽 소유권을 내려놓은 뒤에도 이
        // 함수가 반환할 때까지 Channel을 계속 살려 둔다 - 그 사이 다른
        // 동시 호출자(같은 채널을 이미 resolve해 둔 accepter 등)가
        // 아직 자기 몫을 들고 있어도 안전하다(마지막 강한 참조가
        // 사라지는 시점에만 실제로 반납됨).
        SharedPtr<Channel> channel = kResolveChannelId(args->channelHandle);
        if (!channel) {
            args->error = ChannelError::NotFound;  // 기존엔 널체크조차 없었음
            co_return;
        }
        // [신규, PN-CE6A04AB/SP-CA3C3E57 §6] 소유자만 destroy할 수
        // 있다 - AcceptFromChannelHandler와 동일한 예외(owner==nullptr는
        // 커널 예약 채널). [갱신, 2026-09-20, SP-43331889 §3-1] 위
        // AcceptFromChannelHandler와 동일하게 Task 동일성으로 비교.
        if (channel->owner) {
            SharedPtr<Task> caller = task->submitterTask.lock();
            if (!caller || DontDeref<Task>(caller.get()) != channel->owner) {
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
            req->done.store(1);
            AsyncReactor::submitCompletion(req->task, channel->exclusivePreemptive);
            req = next;
        }

        if (channel->hasName) {
            NamedObjectTable::release(channel->name, channel->nameLength);
        }
        // [갱신, 2026-09-22, PN-260D7D73] 더 이상 직접 free하지 않는다 -
        // kFreeChannelId()가 테이블의 SharedPtr을 비우고, 이 함수가
        // 반환하며 위 지역 변수 `channel`도 스코프를 벗어나면 그게
        // (동시 호출자가 없는 한) 마지막 강한 참조라 kMakeShared의
        // 기본 삭제자(kDestroyAndFree<Channel>)가 자동으로 반납한다.
        kFreeChannelId(channel->channelId);  // [신규, PN-CE6A04AB] 슬롯 해제
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

// [신규, 2026-09-20, SP-43331889 §7(fs 전환)] channel.h 선언 참고 -
// OpenChannelHandler::onExec()과 달리 이름 있는 채널/유저 포인터
// 검증 경로가 아예 없다(커널 모드 직접 호출자 전용, 이름 없는
// 채널만 다룸).
void kOpenNamelessChannelSync(const SharedPtr<Task>& caller, ChannelId* outChannelId, BridgeHandle* outChannelHandle,
                              ChannelError* outError) {
    SharedPtr<Channel> channel = kCreateNamedChannel(nullptr, 0, outError);
    if (!channel) {
        return;
    }
    channel->owner = DontDeref<Task>(caller.get());
    *outChannelId = channel->channelId;
    *outChannelHandle = channel->channelId;
}

// [신규, 2026-09-20, SP-43331889 §7(fs 전환)] `CloseBridgeHandler::
// onExec()` 본문 그대로 - `kResolveOwnedBridge(AsyncTask*, ...)`가
// 필요로 하던 `task->submitterTask.lock()` 간접 참조를 걷어내고
// `caller`를 직접 쓴다(커널 모드 직접 호출자는 애초에 AsyncTask를
// 거치지 않으므로).
void kCloseBridgeSync(const SharedPtr<Task>& caller, uint64_t bridgeHandle, ChannelError* outError) {
    OpenBridgeList* bridges = kOwnerOpenBridgesOf(caller.get());
    if (!bridges) {
        *outError = ChannelError::InvalidHandle;
        return;
    }
    auto* rawTarget = reinterpret_cast<BridgePipe*>(bridgeHandle);
    auto* slot = bridges->find([rawTarget](const SharedPtr<BridgePipe>& sp) { return sp.get() == rawTarget; });
    if (!slot) {
        *outError = ChannelError::InvalidHandle;
        return;
    }
    SharedPtr<BridgePipe> bridge = slot->value;
    bridge->closedLocal = true;

    AsyncTaskWaitQueue woken = kWakeForClose(bridge.get());
    for (AsyncTask* t = woken.popFront(); t; t = woken.popFront()) {
        AsyncReactor::submitCompletion(t);
    }

    bridges->erase(slot);
    *outError = ChannelError::None;
}

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
