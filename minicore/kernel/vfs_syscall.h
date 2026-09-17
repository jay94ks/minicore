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

// [갱신, SP-E9B44929] Vfs 그룹(3).
constexpr SyscallEndpointId kSyscallEndpointMount = kMakeSyscallEndpointId(3, 0);
constexpr SyscallEndpointId kSyscallEndpointUnmount = kMakeSyscallEndpointId(3, 1);
constexpr SyscallEndpointId kSyscallEndpointResolvePath = kMakeSyscallEndpointId(3, 2);
constexpr SyscallEndpointId kSyscallEndpointSignalUserlandReady = kMakeSyscallEndpointId(3, 3);
constexpr SyscallEndpointId kSyscallEndpointWaitForUserlandReady = kMakeSyscallEndpointId(3, 4);

class VfsSyscallService {
public:
    // 부팅 시 한 번 호출 - 위 5개 endpoint를 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_VFS_SYSCALL_H
