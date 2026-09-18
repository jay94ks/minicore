#ifndef MINICORE_KERNEL_CHANNEL_H
#define MINICORE_KERNEL_CHANNEL_H

#include "libkenv/shared_ptr.h"
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

// [정정, 2026-09-17, PN-CE6A04AB/SP-CA3C3E57] 예전엔 ChannelId/
// BridgeHandle 둘 다 그 오브젝트 구조체 자신의 포인터 값이었다 -
// 유저 syscall 인자로 그 값을 받아 검증 없이 reinterpret_cast로
// 역참조하는 보안 취약점으로 이어져(임의 포인터 역참조), 실제로는
// 그렇게 두면 안 됐다. **`BridgeHandle`은 여전히 `BridgePipe*` 값
// 그대로다** - `BridgePipe`는 `Process::openBridges`(호출자 소유
// 목록)에서만 검증되므로(`kResolveOwnedBridge`, PN-9CC66142) 값
// 자체가 무엇이든 상관없다. **`ChannelId`는 이제 커널이 발급하는
// 불투명 핸들이다** - 세대 태그 슬롯 테이블(`gChannelTable`,
// channel.cpp)의 인덱스+세대를 인코딩한 값으로, `kResolveChannelId()`
// 를 거쳐야만 실제 `Channel*`로 해석된다(SP-9CB55C5B의 `ProcessId`와
// 동일한 패턴 - Channel은 SharedPtr이 아니라 슬랩 할당이라 WeakPtr
// 대신 슬롯의 `ptr==nullptr` 여부로 생존을 판정한다).
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
    InvalidPointer,        // [신규, PN-B552E75F] buffer/data가 호출자 자신의 유저 주소공간에 속하지 않음(Paging::isUserRangeValid 실패)
    InvalidArgument,       // [신규, PN-71E50394] 인자 자체가 유효 범위 밖(예: 정의 안 된 SignalNumber, Kill/Stop을 Ignore로 설정 시도)
    NotSupported,          // [신규, PN-71E50394] 유효한 요청이지만 아직 구현되지 않은 기능(예: SignalDisposition::Handler - §4.4/PN-124C105B 전까지)
    PermissionDenied,      // [신규, PN-87D6B615] 호출자가 이 작업을 수행할 자격이 없음(예: DebugAttach - 대상의 직계 부모가 아님, SP-9A6D579F §3.2)
    AlreadyExists,         // [신규, PN-87D6B615] 이미 같은 자원/상태가 존재해 요청이 무의미함(예: DebugAttach - 이 대상에 이미 다른 디버거의 세션이 있음, SP-9A6D579F §7 영구 불변조건)
    Interrupted,           // [신규, PN-B5C2845A] 대기 도중 호출자 자신이 Kill/Terminate 대상이 돼 강제로 실패 완료됨(QU-8E137FFD 답변 - "얘들을 실패시키면 되잖아")
    NotEmpty,              // [신규, PN-4190BBD3, SP-6A563A8F §5-A] ResourceGroupDestroy - 자식/멤버가 남아있는 그룹은 삭제 거부(Linux cgroup과 동일한 "비어있음 강제")
    NotOwner,              // [신규, PN-E82744B1, SP-0666DB3C §17.4] MutexUnlock - 호출자가 그 Mutex의 마지막 lock 성공자(UserMutex::owner)가 아님(락 상태는 그대로 유지)
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
class Process;
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

    // [신규, PN-C4611402] 취소(onCancel) 전용 - FIFO 순서를 지키는
    // pushBack/popFront와 달리 임의 위치의 항목 하나를 제거해야 한다
    // (그 AsyncTask 자신이 곧 반납될 예정이라 이 큐에 댕글링 포인터로
    // 남으면 안 됨). 선형 탐색 - 이 큐들의 길이가 짧다는 다른
    // AsyncTaskWaitQueue 소비자와 동일한 전제. target이 큐에 없으면
    // (이미 정상적으로 popFront된 뒤였거나 애초에 안 들어간 경우)
    // 아무 일도 하지 않는다.
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

// [신규, 2026-09-17, PN-21C2D4E9(DC-21647E46 전역 적용 로드맵 Phase 1)]
// RingBuffer::data(아래)의 삭제자 - 확보 경로가 둘이라(useHugePage에
// 따라 GenericSlabAllocator 4KiB 또는 PageFrameAllocator::allocOrder
// 2MiB) 해제도 그 경로를 그대로 되짚어야 한다(예전엔 BridgePipe::
// destroyPair가 이 분기를 직접 들고 있었다 - 이제 이 삭제자 하나로
// 옮겨 옴). `physBase`는 huge page 경로에서만 의미 있다(가상주소
// data와 물리주소 physBase가 다르므로 PageFrameAllocator::freeOrder
// 에는 반드시 물리주소를 넘겨야 함 - RingBuffer::physBase 필드와
// 동일한 값을 이 삭제자도 별도로 들고 있는 이유). operator() 본체는
// PageFrameAllocator/GenericSlabAllocator를 몰라도 되게 channel.cpp에
// 정의한다(이 헤더에 그 둘을 새로 include하지 않기 위함).
struct RingBufferDeleter {
    bool useHugePage = false;
    uint64_t physBase = 0;
    void operator()(uint8_t* ptr) const;
};

// 한 방향 raw binary 링버퍼(DS-D4E5C451 IPC 포맷 확정 그대로) -
// BridgePipe 하나가 "이 반쪽이 write()할 때 채우는" 자기 소유 버퍼
// 하나만 갖는다(상대는 이 버퍼를 읽는다) - 그래서 필드 이름이
// outbound: 전이중은 BridgePipe 두 개가 각자 outbound를 하나씩
// 가져서 자연히 성립한다(상대의 outbound = 내가 읽는 대상).
struct RingBuffer {
    Spinlock lock;
    // [신규, 2026-09-17, PN-21C2D4E9] 배타적 소유(다른 소유자가 없음)라
    // UniquePtr로 전환 - DC-21647E46/SP-201238BB가 이미 이 필드를
    // UniquePtr 후보로 분류해 뒀다. 실제 확보/해제 시점·경로는 전혀
    // 안 바뀐다(RAII 래퍼일 뿐 정책 변경 없음) - reset()이 여전히
    // 그 시점을 결정한다.
    UniquePtr<uint8_t, RingBufferDeleter> data;
    uint64_t physBase = 0;   // 해제용(PageFrameAllocator/Slab 어느 쪽이든 GenericSlabAllocator::free에 그대로 넘김) - RingBufferDeleter도 이 값을 별도로 들고 있음(huge page 경로 전용)
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
    //
    // [수정, 2026-09-17, PN-21C2D4E9] `data`가 `UniquePtr`로 바뀌면서
    // **`data.initRaw(...)`를 쓴다 - 절대 `data = ...`(operator=)를
    // 쓰지 않는다.** 이 RingBuffer 자신이 위 주석 그대로 raw 슬랩
    // 메모리 위에 놓여 아직 실제 생성자를 거친 적이 없으므로, `data`가
    // 들고 있는 `_ptr`/`_deleter`는 진짜 값이 아니라 이전 슬랩
    // 점유자가 남긴 쓰레기다 - 평범한 `operator=`는 "기존 소유
    // 대상을 안전하게 먼저 해제"하려고 그 쓰레기 `_ptr`/`_deleter`를
    // 그대로 읽어 호출해 버린다(실측 전 코드 추적으로 발견 - 이
    // 세션에서 반복돼 온 "raw 슬랩 메모리 위 reinterpret_cast는
    // 실제 생성자를 안 거친다" 패턴과 정확히 같은 함정). `initRaw()`
    // 는 옛 값을 절대 읽지 않고 그대로 덮어써 이 함정을 피한다.
    // useHugePage는 capacityIn만으로 판별 가능(둘이 겹칠 수 없는
    // 고정값 - BridgePipe::destroyPair의 기존 판별 관례와 동일).
    void reset(uint8_t* dataIn, uint64_t physBaseIn, uint64_t capacityIn) {
        lock.unlock();
        const bool useHugePage = (capacityIn == kHugeChannelRingBufferSize);
        data.initRaw(dataIn, RingBufferDeleter{useHugePage, physBaseIn});
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
//
// [수정, 2026-09-17, PN-9CC66142, DC-21647E46 로드맵 Phase 4] 이제
// 각 반쪽이 독립적으로 `kMakeShared<BridgePipe>()`로 만들어지고
// (Process::openBridges가 실제 강한 소유자), `peer`는 그 상대를
// 관찰만 하는 `WeakPtr<BridgePipe>`다 - 표준 shared_ptr 관례와 동일
// (별도 컨트롤 블록 별칭이 필요 없다, BridgePipe 자신이 이미 최상위
// 독립 할당 객체이므로). **`closedLocal`은 메모리 수명과는 이제
// 무관하다** - 반납은 순수하게 `Process::openBridges`에서 이
// `SharedPtr<BridgePipe>`가 빠지는 시점(참조 카운트 0)에 자동으로
// 일어난다. `closedLocal`은 여전히 "이 반쪽이 앞으로 새 데이터를
// write()하지 않겠다"는 신호 전용으로 남는다(read()가 `kIsBridgeBroken`
// 으로 확인하는 대상 - 상대가 이미 write를 멈췄다는 뜻이지, 그
// BridgePipe 객체 자체가 아직 살아있는지와는 별개 질문이다. 객체
// 생존 여부는 항상 `peer.lock()`으로 따로 확인한다).
struct BridgePipe {
    WeakPtr<BridgePipe> peer;
    RingBuffer outbound;  // 이 반쪽이 write()로 채우는 방향 - peer가 read()로 읽는다
    bool closedLocal = false;  // closeBridge()로 이 반쪽이 "더 이상 안 쓴다"고 선언했는지(순수 신호 - 메모리 수명과 무관, 위 클래스 주석 참고)
    bool blocking = false;  // 설계 문서의 blocking 옵션 - 커널 메커니즘 자체는 항상 비동기이고, 이 값은 향후 유저랜드 스텁이 "제출 후 자동으로 wait까지 할지"를 결정하는 데만 쓰인다(v1은 커널 내부 호출자가 직접 판단)

    // Slab에서 BridgePipe 두 개를 확보해 각각 kMakeShared로 감싸고
    // 서로를 peer(WeakPtr)로 잇는다 - 각자의 outbound 버퍼는
    // useHugePage에 따라 GenericSlabAllocator(4KiB) 또는
    // PageFrameAllocator::allocOrder(9)(2MiB, PN-34B34DB4)에서 확보한다
    // (위 kHugeChannelRingBufferSize 주석 참고). 실패 시 부분적으로
    // 확보된 자원까지 전부 롤백한다(channel.cpp 참고).
    static bool createPair(bool useHugePage, SharedPtr<BridgePipe>* outA, SharedPtr<BridgePipe>* outB);

    // kMakeShared의 기본 삭제자(`kDestroyAndFree<T>`)가 마지막 강한
    // 참조 해제 시 호출 - outbound 버퍼만 반납한다(BridgePipe 구조체
    // 자신의 슬랩 메모리 반납은 그 삭제자가 이어서 처리, Process::
    // destroy()와 동일한 역할 분리 관례).
    void destroy();
};

// 랑데부 지점(설계 문서 그대로) - openChannel이 만든다.
class Channel {
public:
    Spinlock lock;
    bool destroyed = false;
    bool hasName = false;
    uint64_t nameLength = 0;
    char name[kMaxNamedObjectNameLength] = {};

    // [신규, PN-CE6A04AB/SP-CA3C3E57 §2] kCreateNamedChannel()이 발급한
    // 이 채널 자신의 ChannelId(세대 태그 슬롯 테이블 인코딩) - 해제
    // 시(kFreeChannelId) 이 값에서 인덱스를 역산해 O(1)로 슬롯을 비운다.
    ChannelId channelId = 0;

    // [신규, PN-CE6A04AB/SP-CA3C3E57 §6.1, 타입 승격 PN-18FDBFF3/§6-A]
    // 이 채널을 만든 프로세스 - `DontDeref<Process>`라 애초에
    // 역참조할 방법이 없다(operator*/->/T* 변환 없음, shared_ptr.h
    // 참고). AcceptFromChannel/DestroyChannel의 호출자가 이 값과
    // 동일성만 비교하는 용도.
    DontDeref<Process> ownerProcess;

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
        channelId = 0;
        ownerProcess = DontDeref<Process>();
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

    // [신규, PN-C4611402] ConnectChannelHandler::onCancel 전용 - 취소된
    // connectChannel의 `PendingConnectRequest`(그 코루틴 자신의 스택
    // 위 지역 변수라 곧 반납될 예정)를 이 큐에서 찾아 제거한다. 그
    // 요청 자체가 아니라 그것을 제출한 `task`로 찾는다(onCancel은
    // `&req` 주소를 모른다 - AsyncTaskHandler 계약이 `AsyncTask*`/
    // `void* args`만 넘겨준다). 못 찾으면(이미 popPendingConnect됐거나
    // destroyChannel이 먼저 비웠음) 아무 일도 하지 않는다.
    void removePendingConnect(AsyncTask* task) {
        PendingConnectRequest* prev = nullptr;
        for (PendingConnectRequest* cur = pendingHead; cur; prev = cur, cur = cur->next) {
            if (cur->task == task) {
                if (prev) {
                    prev->next = cur->next;
                } else {
                    pendingHead = cur->next;
                }
                if (cur == pendingTail) {
                    pendingTail = prev;
                }
                return;
            }
        }
    }

    // 부팅 시 한 번 호출 - 아래 7개 endpoint 전부를 SyscallRegistry에
    // 등록한다.
    static void registerSyscallEndpoints();
};

// [갱신, 2026-09-17, SP-E9B44929] Group+Call 2단계 인코딩 - Channel은
// 그룹 1, call은 0부터 독자적으로 배정(RM-48E1E610 그룹 배정 절 참고).
constexpr SyscallEndpointId kSyscallEndpointOpenChannel = kMakeSyscallEndpointId(1, 0);
constexpr SyscallEndpointId kSyscallEndpointConnectChannel = kMakeSyscallEndpointId(1, 1);
constexpr SyscallEndpointId kSyscallEndpointAcceptFromChannel = kMakeSyscallEndpointId(1, 2);
constexpr SyscallEndpointId kSyscallEndpointChannelRead = kMakeSyscallEndpointId(1, 3);
constexpr SyscallEndpointId kSyscallEndpointChannelWrite = kMakeSyscallEndpointId(1, 4);
constexpr SyscallEndpointId kSyscallEndpointCloseBridge = kMakeSyscallEndpointId(1, 5);
constexpr SyscallEndpointId kSyscallEndpointDestroyChannel = kMakeSyscallEndpointId(1, 6);

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
