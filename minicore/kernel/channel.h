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
    HugePageUnsupported,   // useHugePage=true인데 커널 정책상 금지(v1 - 항상 금지, 아래 참고)
    BrokenPipe,            // 이 반쪽 또는 상대가 이미 닫힌 상태에서 read/write 시도
};

// 링버퍼 크기 정책(설계 문서 "링버퍼 크기 정책") - 호출부는 정확한
// 바이트 크기를 못 정하고 useHugePage 플래그만 제출한다. 커널이 들고
// 있는 설정값이 상한이다 - v1은 페이지 1개(4KiB) 고정값 하나뿐이다
// (아직 별도 커널 설정 파일류가 없어 상수로 시작 - 나중에 실측하며
// 조정 가능하도록 하드코딩 대신 이 상수 하나로 노출).
constexpr uint64_t kChannelRingBufferSize = 4096;

// **v1 huge page 정책**: 항상 "커널 설정에 의해 금지"로 취급해
// useHugePage=true 요청은 조용한 4K 폴백 없이 즉시
// ChannelError::HugePageUnsupported를 반환한다(설계 문서가 명시한
// 정상적인 실패 경로 - 새 DC 불필요). 실제로 물리적으로 연속인 2MiB
// 블록을 2M PDE 하나로 매핑하는 기능이 Paging에 아직 없어서다
// (`PageFrameAllocator::allocOrder(9)`로 블록 확보는 가능해도,
// `Paging::mapPage`는 4KiB PTE 매핑만 지원 - 설계 문서 "링버퍼 크기
// 정책" 절이 이미 예견한 상황). 이 기능이 필요해지면 Paging에 2M
// 매핑 경로를 먼저 추가하는 별도 계획으로 이어간다.

class Channel;
struct BridgePipe;

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
    // - 한 방향당 최대 하나만 지원한다(v1 제약 - 같은 BridgePipe
    // 핸들로 동시에 여러 read 또는 여러 write를 동시에 submit하지
    // 않는다는 전제, PN-C9625015로 확장 여지를 남겨 둠).
    AsyncTask* pendingReader = nullptr;
    AsyncTask* pendingWriter = nullptr;

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
        pendingReader = nullptr;
        pendingWriter = nullptr;
    }
};

// handshake 완료 후 양쪽이 하나씩 갖는 연결된 반쪽(설계 문서 그대로).
struct BridgePipe {
    BridgePipe* peer = nullptr;
    RingBuffer outbound;  // 이 반쪽이 write()로 채우는 방향 - peer가 read()로 읽는다
    bool closedLocal = false;  // closeBridge()로 이 반쪽이 닫혔는지 - peer 쪽 상태는 peer->closedLocal을 직접 읽는다(별도 미러 필드 불필요, "양쪽 다 닫혀야 반납"이 보장하는 수명 덕분에 항상 안전하게 역참조 가능)
    bool blocking = false;  // 설계 문서의 blocking 옵션 - 커널 메커니즘 자체는 항상 비동기이고, 이 값은 향후 유저랜드 스텁이 "제출 후 자동으로 wait까지 할지"를 결정하는 데만 쓰인다(v1은 커널 내부 호출자가 직접 판단)

    // Slab에서 BridgePipe 두 개 + 각자의 outbound 버퍼(kChannelRingBufferSize)
    // 를 확보해 서로를 peer로 잇는다. useHugePage=true면 위 v1 정책대로
    // 즉시 실패(호출부가 ChannelError::HugePageUnsupported로 변환).
    static bool createPair(bool useHugePage, BridgePipe** outA, BridgePipe** outB);

    // 양쪽 다 closedLocal이면 호출 - 두 BridgePipe와 그 outbound 버퍼
    // 전부를 GenericSlabAllocator::free로 반납한다.
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

    // connectChannel이 채워 넣고 acceptFromChannel이 꺼내가는 FIFO -
    // lock으로 이미 보호되므로 침습적 포인터에 원자 연산이 필요 없다.
    PendingConnectRequest* pendingHead = nullptr;
    PendingConnectRequest* pendingTail = nullptr;

    // acceptFromChannel이 꺼낼 요청이 없을 때 자기 자신을 등록해 두는
    // 슬롯 - connectChannel이 새 요청을 넣을 때 이 슬롯을 확인해
    // 깨운다. v1 제약: 동시에 이 채널에 대해 진행 중인
    // acceptFromChannel 호출은 하나만 지원한다(위 RingBuffer와 같은
    // 성격의 단순화).
    AsyncTask* pendingAccepter = nullptr;

    // Channel도 raw slab 메모리 위에 reinterpret_cast로 앉혀지므로
    // (AsyncTask/RingBuffer와 동일한 이유로 기본 멤버 초기화식이
    // 실행되지 않는다) 명시적으로 모든 필드를 리셋한다 -
    // lock.unlock()으로 Spinlock의 이전 점유자 상태도 강제 초기화.
    void init() {
        lock.unlock();
        destroyed = false;
        hasName = false;
        nameLength = 0;
        pendingHead = nullptr;
        pendingTail = nullptr;
        pendingAccepter = nullptr;
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
