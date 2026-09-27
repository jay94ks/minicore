#ifndef MINICORE_KERNEL_EPOLL_H
#define MINICORE_KERNEL_EPOLL_H

#include "channel.h"  // ChannelError - EpollCreateArgs/EpollCtlArgs/EpollWaitArgs 반환 코드용
#include "libkenv/chunked_list.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

// epoll류 유저 영역 이벤트 다중화(SP-6350DEBB, PN-7562DA62) - v1은
// §4의 첫 실사용 대상인 소켓(kind==MountKind::Socket)과 커널 드라이버
// 파일(kind==MountKind::KernelDriver, 항상 준비됨 취급)만 다룬다.
// **레벨 트리거만 구현**(SP-6350DEBB §2-A의 edge 트리거/§2-D
// OneShot/§2-E Exclusive/§3-A 중첩 epoll은 이번 증분 범위 밖 - 각각
// PN-7562DA62 체크리스트 5/6/7/8번 항목으로 남아 있다. `EpollCtl`에
// 그 비트를 지정해도 v1은 무시하고 항상 레벨 트리거로만 동작한다).

enum class EpollEventMask : uint32_t {
    Readable = 1u << 0,       // EPOLLIN 대응
    Writable = 1u << 1,       // EPOLLOUT 대응
    Error = 1u << 2,          // EPOLLERR/EPOLLHUP 대응 - 항상 암묵적으로 함께 보고됨(호출자가 별도로 지정할 필요 없음)
    EdgeTriggered = 1u << 3,  // [보류, PN-7562DA62 항목5] v1 미구현 - 지정해도 무시(레벨 트리거로 동작)
    OneShot = 1u << 4,        // [보류, 항목6] v1 미구현 - 지정해도 무시
    Exclusive = 1u << 5,      // [보류, 항목7] v1 미구현 - 지정해도 무시
};

// SP-6350DEBB §3 그대로 - edge/oneShot 전용 필드(lastReportedReadable/
// Writable, armed)는 v1이 아직 안 쓰므로 생략했다(위 문서 주석 참고,
// 필요해지면 그 항목 착수 세션이 추가).
struct EpollWatch {
    int32_t targetFd = -1;
    uint32_t interestMask = 0;
    uint64_t userData = 0;
};

// EpollWait이 유저 메모리로 돌려주는 이벤트 하나 - POSIX epoll_event와
// 동일한 역할(events 비트마스크 + 그대로 되돌려주는 사용자 데이터).
struct EpollReadyEvent {
    uint32_t events = 0;  // EpollEventMask 비트합
    uint64_t userData = 0;
};

constexpr uint32_t kMaxEpollWatchesChunkCapacity = 16;

// [SP-6350DEBB §3] `EpollInstance`도 fd 테이블에 들어간다 - epoll_create()
// 가 반환하는 fd도 평범한 fd(POSIX와 동일, 중첩 epoll의 전제이나 v1은
// 아직 그 중첩 자체는 구현하지 않는다 - 위 문서 주석 참고).
class EpollInstance {
public:
    Spinlock lock;
    ChunkedList<EpollWatch, kMaxEpollWatchesChunkCapacity> watches;

    // raw slab 메모리 위에 놓이므로(RingBuffer/Channel과 동일한 이유)
    // 명시적으로 초기화한다 - Spinlock의 0값은 이미 unlocked와 같지만
    // 다른 필드와 동일한 관례로 명시.
    void init() {
        lock.unlock();
        watches.clear();
    }

    // kMakeShared 기본 삭제자가 마지막 강한 참조 해제 시 호출 -
    // ChunkedList가 슬랩에서 확보한 청크들을 반납해야 한다(RingBuffer::
    // data.reset()과 동일한 이유 - 그냥 두면 청크 메모리가 샌다).
    void destroy() { watches.clear(); }
};

enum class EpollCtlOp : uint32_t {
    Add = 1,
    Mod = 2,
    Del = 3,
};

// [갱신, 2026-09-27] RM-48E1E610 그룹6(Event, SP-2602CAA6)의 다음
// 미사용 call 번호(3-5)를 이어 쓴다 - 새 그룹을 만들지 않는다(설계
// 문서 §5, RM-23F4B687 §4 과도한 그룹 분리 방지 원칙과 동일).
constexpr SyscallEndpointId kSyscallEndpointEpollCreate = kMakeSyscallEndpointId(6, 3);
constexpr SyscallEndpointId kSyscallEndpointEpollCtl = kMakeSyscallEndpointId(6, 4);
constexpr SyscallEndpointId kSyscallEndpointEpollWait = kMakeSyscallEndpointId(6, 5);

struct EpollCreateArgs {
    // out
    ChannelError error = ChannelError::None;
    int64_t fd = -1;
};

struct EpollCtlArgs {
    int32_t epfd = -1;
    EpollCtlOp op = EpollCtlOp::Add;
    int32_t targetFd = -1;
    uint32_t mask = 0;      // EpollEventMask 비트합(Add/Mod에서만 의미 있음)
    uint64_t userData = 0;  // Add/Mod에서만 의미 있음
    // out
    ChannelError error = ChannelError::None;
};

struct EpollWaitArgs {
    int32_t epfd = -1;
    EpollReadyEvent* outEvents = nullptr;  // in: 유저 메모리, 최소 maxEvents개 원소
    uint32_t maxEvents = 0;
    int64_t timeoutMs = -1;  // -1=무한 대기, 0=즉시 반환(폴링), 그 외=밀리초
    // out
    ChannelError error = ChannelError::None;
    int64_t count = 0;
};

class Epoll {
public:
    // 부팅 시 한 번 호출 - 위 3개 endpoint를 전부 SyscallRegistry에 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_EPOLL_H
