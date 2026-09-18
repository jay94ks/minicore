#ifndef MINICORE_KERNEL_VFS_SYSCALL_H
#define MINICORE_KERNEL_VFS_SYSCALL_H

#include "channel.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

// SP-7CC5693A §2.2/§2.5 - VFS 커널 서브시스템의 유저랜드 노출 syscall
// 5종(Mount/Unmount/ResolvePath/SignalUserlandReady/WaitForUserlandReady,
// RM-48E1E610 그룹3 Vfs 0-4번). §3(fs 서비스 내부 - 파일시스템 드라이버
// 우선순위)/§9(표준 파일 API, SP-2AAD7C8D)는 이 파일 범위 밖 - PN-452FF696
// 체크리스트 항목 5/6(실제 드라이버 필요)이 다룬다.

// [SP-7CC5693A §2.2] fs 서비스 → 커널: 이 fs 서비스 인스턴스가 이 경로
// 아래를 전담하겠다고 등록한다.
struct MountArgs {
    const char* path = nullptr;  // in
    uint32_t pathLen = 0;
    uint64_t channelId = 0;      // in: 이 fs 서비스가 미리 만들어 둔 Channel(PL-C8648D4D)
    // out
    // [기존 ChannelError 재사용 - RequestIoPermissionArgs 선례와 동일
    // 관례] 문서(§2.2)가 말하는 "AlreadyMounted"/"InvalidPath"는 새
    // enum 값을 만들지 않고 AlreadyExists/InvalidPointer로 매핑한다.
    // MountTable::mount()는 "이미 그 정확한 경로가 마운트됨"과 "테이블
    // 이 가득 참" 두 실패를 구분하지 않고 둘 다 false를 반환하므로,
    // 이 syscall도 둘 다 AlreadyExists로 뭉뚱그린다(정직하게 기록 -
    // 구분이 필요해지면 MountTable 자체의 반환값 확장이 선행돼야 함).
    ChannelError error = ChannelError::None;
};

// [SP-7CC5693A §2.2] fs 서비스 → 커널: 정확히 일치하는 마운트를 해제한다.
struct UnmountArgs {
    const char* path = nullptr;
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;  // NotFound(문서의 "NotMounted") 재사용
};

// [SP-7CC5693A §2.2] 임의의 프로세스 → 커널: 절대 경로를 담당 fs
// 서비스의 Channel로 라우팅한다 - 실제 열기/읽기/쓰기는 호출부가 그
// Channel로 직접 IPC해야 한다(§4-A "커널은 라우팅 대상으로만 안다").
//
// [정직하게 기록, SP-2AAD7C8D §9.1] 이 구조체는 MountKind::Channel
// 담당자만 표현할 수 있다 - `MountKind::KernelDriver`(예: /sys/live의
// livefs)를 만나면 `ChannelError::NotSupported`로 응답한다(그 경우를
// 어떻게 노출할지는 §9 Open/Read syscall 착수 시 FileDescriptor 설계와
// 함께 확정하기로 이미 열려 있는 질문 - 여기서 임의로 결정하지 않음).
struct ResolvePathArgs {
    const char* path = nullptr;  // in: 절대 경로
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;  // NotFound / InvalidPointer / NotSupported(위 참고)
    uint64_t ownerChannelId = 0;
    uint32_t relPathOffset = 0;
};

// [SP-7CC5693A §2.5] init(QU-FF3F0CAA로 확정된 "첫 프로세스") → 커널:
// 유저 영역이 정상 동작 중임을 커널 전역에 단 1회만 알린다. 호출자
// 신원은 검증하지 않는다(문서 §4 항목5 - v1 범위 밖, 필요해지면 별도 DC).
struct SignalUserlandReadyArgs {
    // in: 없음
    // out
    ChannelError error = ChannelError::None;  // AlreadyExists(문서의 "AlreadyCalled") 재사용
};

// [SP-7CC5693A §2.5] 다른 서비스(fs 등) → 커널: 신호가 아직 없으면
// 블로킹(AsyncTask::yield() 반복 - 이 프로젝트의 다른 "동기처럼 보이는
// 비동기 대기"와 동일한 관례, 예: ConnectChannelHandler), 이미 왔으면
// 즉시 반환. 대기는 무한정 - init이 죽으면 어차피 시스템이 죽은 것.
struct WaitForUserlandReadyArgs {
    // in: 없음
    // out
    ChannelError error = ChannelError::None;  // 항상 None(문서 그대로 - 실패 케이스 없음)
};

// [SP-2AAD7C8D §9.3, PN-EA4EE935] 표준 파일 API 4종(Open/Close/Read/
// Write) - RM-48E1E610 그룹3 call 5-8에 이미 예약된 번호 그대로.
// **스코프**: `MountTable::resolve()`가 `MountKind::Channel`을 내는
// 마운트(유저랜드 fs 서비스 대상)는 아직 지원하지 않는다 - §9.1의
// "그 Channel로 Open IPC 메시지 전송"의 실제 와이어 포맷이 어디에도
// 정의돼 있지 않아(설계 문서 자체의 공백), 이를 임의로 정하지 않고
// `ChannelError::NotSupported`로 정직하게 응답한다(CLAUDE.md 규칙 4,
// PN-EA4EE935 "범위 밖" 절 참고) - `MountKind::KernelDriver`(livefs 등,
// Channel/IPC 없이 커널이 직접 호출) 마운트만 실제로 동작한다.
// Lseek/Stat/Readdir/Mkdir/Unlink(call 9-13)는 이번 증분 범위 밖.
struct OpenArgs {
    const char* path = nullptr;  // in: 절대 경로
    uint32_t pathLen = 0;
    uint32_t flags = 0;  // in: mount_table.h의 KernelFsOpenArgs::flags로 그대로 전달(§9.3 OpenFlags)
    // out
    int32_t fd = -1;      // 실패 시 -1
    ChannelError error = ChannelError::None;
};

struct CloseArgs {
    int32_t fd = -1;
    // out
    ChannelError error = ChannelError::None;
};

struct ReadArgs {
    int32_t fd = -1;
    void* buf = nullptr;
    uint32_t len = 0;
    // out
    uint32_t bytesRead = 0;  // 0이면 EOF
    ChannelError error = ChannelError::None;
};

struct WriteArgs {
    int32_t fd = -1;
    const void* buf = nullptr;
    uint32_t len = 0;
    // out
    uint32_t bytesWritten = 0;
    ChannelError error = ChannelError::None;
};

// [SP-2AAD7C8D §9.3, PN-E9960D10] **[정직하게 기록] `End`는 이번
// 증분 범위 밖** - 파일 크기를 알아야 하는데 `Process::FileDescriptor`
// 가 원래 경로를 안 들고 있어(fsHandle만 저장) `KernelFsStatArgs`
// (경로 기반 API)로 조회할 방법이 없다. `Set`/`Current`는 fd 테이블
// offset의 순수 산술이라 `KernelFsDriver` 호출 자체가 필요 없다.
enum class SeekWhence : uint32_t { Set, Current, End };

struct LseekArgs {
    int32_t fd = -1;
    int64_t offset = 0;
    SeekWhence whence = SeekWhence::Set;
    // out
    uint64_t newOffset = 0;
    ChannelError error = ChannelError::None;  // InvalidHandle / InvalidArgument(음수 결과) / NotSupported(End)
};

// [SP-2AAD7C8D §9.3/§9.4, PN-238FD331] Mkdir/Unlink와 같은 급의
// "경로만으로 동작, fd 불필요" 오퍼레이션(§9.4) - `ResolvePathArgs`와
// 거의 같은 모양이다. `MountKind::Channel` 마운트는 Open과 동일한
// 이유(§9.1 IPC 와이어 포맷 미정)로 아직 NotSupported.
struct StatArgs {
    const char* path = nullptr;  // in: 절대 경로
    uint32_t pathLen = 0;
    // out
    uint64_t size = 0;
    bool isDirectory = false;
    ChannelError error = ChannelError::None;
};

// [SP-2AAD7C8D §9.3/§9.4, PN-CF030FC3] StatArgs와 같은 모양(fd 불필요) -
// **[정직하게 기록]** 현재 유일한 KernelFsDriver 구현체(livefs)는
// 읽기 전용이라 실제로는 항상 PermissionDenied를 반환한다
// (mount_table.h "v1 축소 범위" 절) - Write와 동일한 상황.
struct MkdirArgs {
    const char* path = nullptr;
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
};

struct UnlinkArgs {
    const char* path = nullptr;
    uint32_t pathLen = 0;
    // out
    ChannelError error = ChannelError::None;
};

// [신규, 2026-09-19, PN-770A28FB, SP-7CC5693A §3.2] 디렉터리를 먼저
// `Open()`으로 열어 얻은 `fd`에 대해 반복 호출하는 스트리밍 나열 -
// 매 호출마다 다음 엔트리 하나(커서는 `Process::FileDescriptor::offset`
// 이 소유 - Read가 바이트 오프셋을 쓰는 것과 동일한 관례, mount_table.h
// 의 `KernelFsReaddirArgs` 문서 주석 참고). `hasMore=false`면 이미
// 끝났다는 뜻(`name`은 무의미) - Read의 `bytesRead==0` EOF 관례와
// 동일한 결.
struct ReaddirArgs {
    int32_t fd = -1;
    // out
    char name[64] = {};
    uint32_t nameLength = 0;
    bool isDirectory = false;
    bool hasMore = false;
    ChannelError error = ChannelError::None;
};

// [갱신, SP-E9B44929] Vfs 그룹(3).
constexpr SyscallEndpointId kSyscallEndpointMount = kMakeSyscallEndpointId(3, 0);
constexpr SyscallEndpointId kSyscallEndpointUnmount = kMakeSyscallEndpointId(3, 1);
constexpr SyscallEndpointId kSyscallEndpointResolvePath = kMakeSyscallEndpointId(3, 2);
constexpr SyscallEndpointId kSyscallEndpointSignalUserlandReady = kMakeSyscallEndpointId(3, 3);
constexpr SyscallEndpointId kSyscallEndpointWaitForUserlandReady = kMakeSyscallEndpointId(3, 4);
constexpr SyscallEndpointId kSyscallEndpointOpen = kMakeSyscallEndpointId(3, 5);
constexpr SyscallEndpointId kSyscallEndpointClose = kMakeSyscallEndpointId(3, 6);
constexpr SyscallEndpointId kSyscallEndpointRead = kMakeSyscallEndpointId(3, 7);
constexpr SyscallEndpointId kSyscallEndpointWrite = kMakeSyscallEndpointId(3, 8);
constexpr SyscallEndpointId kSyscallEndpointLseek = kMakeSyscallEndpointId(3, 9);
constexpr SyscallEndpointId kSyscallEndpointStat = kMakeSyscallEndpointId(3, 10);
constexpr SyscallEndpointId kSyscallEndpointReaddir = kMakeSyscallEndpointId(3, 11);
constexpr SyscallEndpointId kSyscallEndpointMkdir = kMakeSyscallEndpointId(3, 12);
constexpr SyscallEndpointId kSyscallEndpointUnlink = kMakeSyscallEndpointId(3, 13);

class VfsSyscallService {
public:
    // 부팅 시 한 번 호출 - 위 14개 endpoint를 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_VFS_SYSCALL_H
