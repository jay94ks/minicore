#ifndef MINICORE_KERNEL_CHANNEL_H
#define MINICORE_KERNEL_CHANNEL_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "named_object.h"
#include "syscall.h"

namespace kernel {

// 메시징 채널 IPC(SP-1FBC0EEB) - 이 파일이 구현하는 API는 전부
// SyscallRegistry endpoint 하나씩으로 등록된다(설계 문서 "API
// 시퀀스와 각 syscall의 처리" 그대로) - Channel::registerEndpoints()
// 가 부팅 시 한 번 이 매핑을 건다.
//
// **이름은 전부 제안**(설계 문서 "배경" 1번 그대로) - 확정된 이름이
// 아니다.

// ChannelId/BridgeHandle 둘 다 실제로는 그 오브젝트 구조체 자신의
// 포인터 값이다(Syscall 서브시스템의 토큰=포인터 관례와 동일 -
// PL-21344323 참고, 별도 전역 ID/핸들 테이블 불필요 - 새 DC 불필요
// 수준의 구현 세부). 프로세스 모델이 아직 없어 "ChannelId는 다른
// 프로세스에게 전달, BridgeHandle은 로컬 참조"라는 설계 문서의 구분이
// 지금은 의미가 없다 - v1은 둘을 같은 값으로 채운다(openChannel 참고).
using ChannelId = uint64_t;
using BridgeHandle = uint64_t;

// 각 syscall이 args를 통해 돌려주는 결과 코드 - AsyncTaskState
// (Completed/Failed)는 "프레임워크 차원에서 실행 자체가 됐는지"만
// 구분하므로(PL-21344323 Syscall::wait 참고), 실제 성공/실패 세부는
// 전부 이 코드로 전달한다.
enum class ChannelError : uint32_t {
    None = 0,
    NameInUse,             // 이름 충돌(종류 불문 - 보안 정책, named_object.h)
    NotFound,              // 채널을 ID/이름으로도 못 찾음, 또는 그 사이 소멸됨
    InvalidHandle,         // BridgeHandle/채널 handle이 유효하지 않음
    ResourceExhausted,     // Slab/페이지 고갈
    HugePageUnsupported,   // 더 이상 이 경로에서 반환되지 않음(PN-34B34DB4) - API 호환을 위해 값만 유지
    BrokenPipe,            // 이 반쪽 또는 상대가 이미 닫힌 상태에서 read/write 시도
};

// 링버퍼 크기 정책(설계 문서 "링버퍼 크기 정책") - 호출부는 정확한
// 바이트 크기를 못 정하고 useHugePage 플래그만 제출한다. 커널이 들고
// 있는 설정값이 상한이다 - v1은 페이지 1개(4KiB) 고정값 하나뿐이다
// (아직 별도 커널 설정 파일류가 없어 상수로 시작 - 나중에 실측하며
// 조정 가능하도록 하드코딩 대신 이 상수 하나로 노출).
constexpr uint64_t kChannelRingBufferSize = 4096;

// **huge page 지원(PN-34B34DB4, 2026-09-16 구현)**: useHugePage=true면
// PageFrameAllocator::allocOrder(kHugeChannelRingBufferOrder)로
// 물리적으로 연속인 2MiB 블록을 확보해 링버퍼로 쓴다. 이 블록은 커널
// 자신만(유저 주소공간에 매핑되지 않음) 접근하므로 Paging::mapPage/
// mapRange를 거칠 필요가 없다 - kDirectMapBase 덕분에 이미 설치된
// 모든 usable 물리 메모리가 커널 가상주소공간에 항상 매핑돼 있어
// (paging.h kPhysToVirt 참고), allocOrder()가 돌려준 물리주소를 그
// 함수 하나로 바로 커널 가상주소로 바꿔 쓰면 된다 - 새 페이지 테이블
// 엔트리를 만들 필요가 전혀 없다(4K 폴백 경로가 GenericSlabAllocator의
// 가상주소를 그대로 쓰는 것과 대칭). `ChannelError::HugePageUnsupported`
// 는 이제 이 경로에서 반환되지 않는다(할당 실패는 다른 경로와 동일하게
// ResourceExhausted) - enum 값 자체는 API 호환을 위해 남겨 둔다.
constexpr uint64_t kHugeChannelRingBufferSize = 2 * 1024 * 1024;  // allocOrder(9)와 일치
constexpr uint32_t kHugeChannelRingBufferOrder = 9;

class Channel;
struct BridgePipe;

// Channel 생성 로직 팩터링(SP-00CA7175 §2.0a) - alloc+init+
// (있으면)NamedObjectTable::reserve까지 한 번에 한다. openChannel
// syscall 핸들러(name은 유저 제공, nullptr/길이 0이면 이름 없이)와
// KernelReservedTable::reserveForKernelService(name=nullptr로 호출 -
// NamedObjectTable에 등록하지 않아야 하는 이유는 livefs.h 참고) 양쪽이
// 공유한다 - 순수 리팩터링, openChannel의 기존 동작은 무변경.
Channel* kCreateNamedChannel(const char* name, uint64_t nameLength, ChannelError* outError);

// AsyncTask 여러 개를 FIFO로 대기시키는 침습적 큐 - AsyncTask::next를
// 재사용한다(파킹돼 있는 동안엔 AsyncReactor 실행 큐에 없어 비어
// 있음 - kernel::Task가 WaitQueue에서 Task::next를 재사용하는 것과
// 같은 패턴). 호출부가 이미 잡고 있는 락(RingBuffer::lock/
// Channel::lock) 아래에서만 push/pop 해야 한다 - 자체 동기화는 없다
// (PN-C9625015 - 기존 "대기자 슬롯 하나" 필드들을 이 큐로 교체해
// 동시 다중 accepter/reader/writer를 지원한다).
struct AsyncTaskWaitQueue {
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
};

// 코어당이 아니라 채널 전역 - connectChannel이 채워 넣고
// acceptFromChannel이 꺼내 간다. Channel::lock으로 보호되는 단순
// 침습적 단일 연결 리스트(FIFO)라 별도 락/원자 연산이 필요 없다.
struct PendingConnectRequest {
    AsyncTask* task = nullptr;         // 완료 시 AsyncReactor::submitCompletion으로 깨울 대상
    bool useHugePage = false;
    bool done = false;                 // acceptFromChannel/destroyChannel이 세팅
    bool rejected = false;             // true면 accept 실패/채널 소멸 - resultBridge 무효
    BridgePipe* resultBridge = nullptr;  // 클라이언트 쪽 반쪽(성공 시)
    PendingConnectRequest* next = nullptr;
};

// 한 방향 raw binary 링버퍼(DS-D4E5C451 IPC 포맷 확정 그대로) -
// BridgePipe 하나가 "이 반쪽이 write()할 때 채우는" 자기 소유 버퍼
// 하나만 갖는다(상대는 이 버퍼를 읽는다) - 그래서 필드 이름이
// outbound: 전이중은 BridgePipe 두 개가 각자 outbound를 하나씩
// 가져서 자연히 성립한다(상대의 outbound = 내가 읽는 대상).
struct RingBuffer {
    Spinlock lock;
    uint8_t* data = nullptr;
    uint64_t physBase = 0;   // 해제용(PageFrameAllocator/Slab 어느 쪽이든 GenericSlabAllocator::free에 그대로 넘김)
    uint64_t capacity = 0;
    uint64_t readPos = 0;
    uint64_t writePos = 0;
    uint64_t used = 0;       // readPos/writePos로부터 매번 모듈로 계산하지 않고 직접 추적(가득참/빔 판정 단순화)

    // 이 버퍼가 비어서(read) 또는 가득 차서(write) 대기 중인 AsyncTask
    // 전부(FIFO) - 방향당 동시에 여러 read 또는 여러 write가 submit돼도
    // 전부 큐에 쌓였다가 순서대로 깨어난다(PN-C9625015, 예전엔 슬롯
    // 하나뿐이라 두 번째부터는 영구히 못 깨어나는 결함이 있었다).
    AsyncTaskWaitQueue pendingReaders;
    AsyncTaskWaitQueue pendingWriters;

    // data/physBase/capacity를 raw slab 메모리 위에 세팅하고 나머지
    // 필드를 명시적으로 리셋한다(AsyncTask::init()과 동일한 이유 -
    // reinterpret_cast로 앉혀진 raw 메모리라 기본 멤버 초기화식이
    // 실행되지 않는다). lock.unlock()으로 강제 초기화하는 이유도
    // 같다 - Spinlock._locked가 이전 점유자의 값을 그대로 들고 있을
    // 수 있다.
    void reset(uint8_t* dataIn, uint64_t physBaseIn, uint64_t capacityIn) {
        lock.unlock();
        data = dataIn;
        physBase = physBaseIn;
        capacity = capacityIn;
        readPos = 0;
        writePos = 0;
        used = 0;
        pendingReaders.head = nullptr;
        pendingReaders.tail = nullptr;
        pendingWriters.head = nullptr;
        pendingWriters.tail = nullptr;
    }
};

// handshake 완료 후 양쪽이 하나씩 갖는 연결된 반쪽(설계 문서 그대로).
struct BridgePipe {
    BridgePipe* peer = nullptr;
    RingBuffer outbound;  // 이 반쪽이 write()로 채우는 방향 - peer가 read()로 읽는다
    bool closedLocal = false;  // closeBridge()로 이 반쪽이 닫혔는지 - peer 쪽 상태는 peer->closedLocal을 직접 읽는다(별도 미러 필드 불필요, "양쪽 다 닫혀야 반납"이 보장하는 수명 덕분에 항상 안전하게 역참조 가능)
    bool blocking = false;  // 설계 문서의 blocking 옵션 - 커널 메커니즘 자체는 항상 비동기이고, 이 값은 향후 유저랜드 스텁이 "제출 후 자동으로 wait까지 할지"를 결정하는 데만 쓰인다(v1은 커널 내부 호출자가 직접 판단)

    // Slab에서 BridgePipe 두 개를 확보해 서로를 peer로 잇는다 - 각자의
    // outbound 버퍼는 useHugePage에 따라 GenericSlabAllocator(4KiB) 또는
    // PageFrameAllocator::allocOrder(9)(2MiB, PN-34B34DB4)에서 확보한다
    // (위 kHugeChannelRingBufferSize 주석 참고).
    static bool createPair(bool useHugePage, BridgePipe** outA, BridgePipe** outB);

    // 양쪽 다 closedLocal이면 호출 - 두 BridgePipe와 그 outbound 버퍼
    // 전부를 반납한다(버퍼는 만들 때 쓴 것과 같은 할당자로 - capacity로
    // 구분, channel.cpp 참고).
    static void destroyPair(BridgePipe* a, BridgePipe* b);
};

// 랑데부 지점(설계 문서 그대로) - openChannel이 만든다.
class Channel {
public:
    Spinlock lock;
    bool destroyed = false;
    bool hasName = false;
    uint64_t nameLength = 0;
    char name[kMaxNamedObjectNameLength] = {};

    // Tier B(SP-00CA7175 §2.2, "ExclusivePreemptiveChannel") - true면
    // 이 Channel의 accept/read/write에서 파생된 AsyncTask가 그 코어의
    // AsyncReactor 실행 큐에서 일반 우선순위보다 앞서 처리돼야 한다.
    // 기본값 false(기존 Channel 동작 무변경) - kCreateNamedChannel()로
    // 만든 뒤 KernelReservedTable::reserveForKernelService()만 명시적으로
    // true로 설정한다(§2.0a).
    bool exclusivePreemptive = false;

    // connectChannel이 채워 넣고 acceptFromChannel이 꺼내가는 FIFO -
    // lock으로 이미 보호되므로 침습적 포인터에 원자 연산이 필요 없다.
    PendingConnectRequest* pendingHead = nullptr;
    PendingConnectRequest* pendingTail = nullptr;

    // acceptFromChannel이 꺼낼 요청이 없을 때 자기 자신을 등록해 두는
    // 큐(FIFO) - connectChannel이 새 요청을 넣을 때 이 큐에서 하나를
    // 꺼내 깨운다. 동시에 이 채널에 대해 여러 acceptFromChannel이
    // 진행 중이어도 전부 순서대로 대기/처리된다(PN-C9625015).
    AsyncTaskWaitQueue pendingAccepters;

    // Channel도 raw slab 메모리 위에 reinterpret_cast로 앉혀지므로
    // (AsyncTask/RingBuffer와 동일한 이유로 기본 멤버 초기화식이
    // 실행되지 않는다) 명시적으로 모든 필드를 리셋한다 -
    // lock.unlock()으로 Spinlock의 이전 점유자 상태도 강제 초기화.
    void init() {
        lock.unlock();
        destroyed = false;
        hasName = false;
        nameLength = 0;
        exclusivePreemptive = false;
        pendingHead = nullptr;
        pendingTail = nullptr;
        pendingAccepters.head = nullptr;
        pendingAccepters.tail = nullptr;
    }

    void pushPendingConnect(PendingConnectRequest* req) {
        req->next = nullptr;
        if (pendingTail) {
            pendingTail->next = req;
        } else {
            pendingHead = req;
        }
        pendingTail = req;
    }

    PendingConnectRequest* popPendingConnect() {
        PendingConnectRequest* req = pendingHead;
        if (req) {
            pendingHead = req->next;
            if (!pendingHead) {
                pendingTail = nullptr;
            }
        }
        return req;
    }

    // 부팅 시 한 번 호출 - 아래 7개 endpoint 전부를 SyscallRegistry에
    // 등록한다.
    static void registerSyscallEndpoints();
};

// endpointId 상수(SyscallRegistry 고정 슬롯) - kSyscallEndpointSelfTerminate
// (syscall.h, 값 0) 바로 다음부터 배정한다.
constexpr SyscallEndpointId kSyscallEndpointOpenChannel = 1;
constexpr SyscallEndpointId kSyscallEndpointConnectChannel = 2;
constexpr SyscallEndpointId kSyscallEndpointAcceptFromChannel = 3;
constexpr SyscallEndpointId kSyscallEndpointChannelRead = 4;
constexpr SyscallEndpointId kSyscallEndpointChannelWrite = 5;
constexpr SyscallEndpointId kSyscallEndpointCloseBridge = 6;
constexpr SyscallEndpointId kSyscallEndpointDestroyChannel = 7;

struct OpenChannelArgs {
    const char* name = nullptr;   // nullptr 또는 nameLength==0 - 이름 없이 개설
    uint64_t nameLength = 0;
    // out
    ChannelError error = ChannelError::None;
    ChannelId channelId = 0;
    BridgeHandle channelHandle = 0;  // v1은 channelId와 같은 값(위 타입 주석 참고)
};

struct ConnectChannelArgs {
    ChannelId target = 0;         // 0이면 name으로 찾는다
    const char* name = nullptr;
    uint64_t nameLength = 0;
    bool useHugePage = false;
    // out
    ChannelError error = ChannelError::None;
    BridgeHandle bridge = 0;
};

struct AcceptFromChannelArgs {
    BridgeHandle channelHandle = 0;
    // out
    ChannelError error = ChannelError::None;
    BridgeHandle bridge = 0;
};

struct ChannelReadArgs {
    BridgeHandle bridge = 0;
    void* buffer = nullptr;
    uint64_t maxLength = 0;
    // out
    ChannelError error = ChannelError::None;
    uint64_t bytesRead = 0;
};

struct ChannelWriteArgs {
    BridgeHandle bridge = 0;
    const void* data = nullptr;
    uint64_t length = 0;
    // out
    ChannelError error = ChannelError::None;
    uint64_t bytesWritten = 0;
};

struct CloseBridgeArgs {
    BridgeHandle bridge = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct DestroyChannelArgs {
    BridgeHandle channelHandle = 0;
    // out
    ChannelError error = ChannelError::None;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_CHANNEL_H
