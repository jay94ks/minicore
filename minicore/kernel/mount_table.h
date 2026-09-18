#ifndef MINICORE_KERNEL_MOUNT_TABLE_H
#define MINICORE_KERNEL_MOUNT_TABLE_H

#include "async_task.h"
#include "libkenv/types.h"

namespace kernel {

// SP-7CC5693A §2.1 - VFS 커널 서브시스템의 마운트 테이블. 커널은 파일
// 내용이나 디렉터리 엔트리를 전혀 모른다 - 아는 것은 "이 절대 경로
// 접두사는 이 담당자(유저랜드 fs 서비스의 Channel, 또는 커널 자신이
// 구현한 드라이버)가 처리한다"는 매핑뿐이다(§4-A "커널은 라우팅
// 대상으로만 안다" 원칙).
constexpr uint32_t kMaxMountPathLen = 64;   // "/usr/*/mnt"류도 넉넉히 담는 v1 상한(NamedObjectTable과 같은 관례)
constexpr uint32_t kMaxMountEntries = 16;   // SP-8B6B8D25 §4가 나열한 고정 경로 개수 + 여유(v1 상한, 실측 후 조정)

enum class MountKind : uint8_t {
    Channel = 0,       // 유저랜드 fs 서비스 - IPC(Channel)로 라우팅
    KernelDriver = 1,  // 커널이 직접 구현한 드라이버(livefs 등) - IPC 없이 그 자리에서 직접 호출
};

// **[v1 잠정 결정, 2026-09-16, PN-71C2B857]** SP-7CC5693A §2.1의
// `KernelFsDriver` pseudocode가 참조하는 FileHandle/OpenResult/
// ReadResult는 그 문서 어디에도 구체 정의가 없다 - SP-2AAD7C8D §9.1이
// 이미 "이 타입들은 §9(표준 파일 API/fd 테이블) 착수 시점에 확정한다"고
// 명시적으로 열어 둔 자리다(§9.6 목록엔 없지만 §9.1 본문에 그렇게
// 적혀 있음). 지금은 `KernelFsDriver`를 실제로 호출하는 syscall 경로
// (Open/Read, SP-2AAD7C8D §9.3) 자체가 아직 없어(§9 미착수) 이 값들을
// 소비하는 곳이 없으므로, 인터페이스 컴파일을 위한 최소 잠정 형태만
// 정의해 둔다 - **§9 착수 시 그 문서의 최종 `FileDescriptor`/fd 설계에
// 맞춰 이 타입들을 다시 확정/치환하는 것을 전제로 한다**(RM-23F4B687
// §4 취지 - 구현 세부 수준의 결정, 새 DC 불필요).
enum class VfsError : uint32_t {
    None = 0,
    NotFound,
    InvalidHandle,
    PermissionDenied,
    InvalidArgument,
};

struct FileHandle {
    uint64_t value = 0;
};

struct OpenResult {
    FileHandle handle;
    bool isDirectory = false;
    VfsError error = VfsError::None;
};

struct ReadResult {
    uint32_t bytesRead = 0;
    VfsError error = VfsError::None;
};

// [수정, 2026-09-17, PN-BC04D3DC, SP-7CC5693A §2.1 갱신 - 설계자 지시
// 2건("해당 드라이버와 쌍을 이루는 구조", "비동기 프레임워크를 기반으로
// 동작하도록 시그니처 재검토")] `KernelFsDriver`가 평범한 가상함수
// 인터페이스(open/close/read 직접 호출)에서 `AsyncTaskHandler`를
// 상속하는 형태로 바뀌었다 - SP-F682B889 §3.1이 확립한 다른 모든
// syscall 핸들러(Channel/Pnp 등)와 동일한 모양(args 구조체 +
// onExec/onFailure/onCancel)을 맞춘다. §3.2 FileSystemDriver(open/
// close/read/write/stat/mkdir/rmdir/unlink/readdir 9개)와 같은
// 오퍼레이션 집합을 갖되 block device가 없으므로 mount(BlockDevice*)
// 는 없다.
enum class KernelFsOpCode : uint32_t {
    Open,
    Close,
    Read,
    Write,
    Stat,
    Mkdir,
    Rmdir,
    Unlink,
    Readdir,
};

// 9개 KernelFsXxxArgs 전부 첫 필드가 `op`로 시작한다 - onExec()가
// `args`를 이 태그만으로 먼저 읽어(모든 구조체의 첫 멤버이므로 어떤
// 구체 타입으로 들어와도 안전) 실제 op별 구조체로 재캐스팅해 분기한다.
struct KernelFsOpenArgs {
    KernelFsOpCode op = KernelFsOpCode::Open;
    const char* relPath = nullptr;
    uint32_t relPathLen = 0;
    uint32_t flags = 0;
    // out
    OpenResult result;
};

struct KernelFsCloseArgs {
    KernelFsOpCode op = KernelFsOpCode::Close;
    FileHandle handle;
};

struct KernelFsReadArgs {
    KernelFsOpCode op = KernelFsOpCode::Read;
    FileHandle handle;
    uint64_t offset = 0;
    void* buf = nullptr;
    uint32_t len = 0;
    // out
    ReadResult result;
};

// [v1 축소 범위] LiveFs(현재 유일한 KernelFsDriver 구현체)의 세
// 하위 경로(named/initrd.cpio/kernel/<name>)가 전부 본질적으로
// 읽기 전용 뷰라, write/mkdir/rmdir/unlink는 항상 PermissionDenied를
// 반환한다(실제 쓰기 가능한 KernelFsDriver 구현체가 생기면 그때
// 이 가정을 재검토). Readdir ABI는 아래 `KernelFsReaddirArgs` 참고 -
// [갱신, 2026-09-19, PN-770A28FB] 더 이상 미구현이 아니다.
struct KernelFsWriteArgs {
    KernelFsOpCode op = KernelFsOpCode::Write;
    FileHandle handle;
    uint64_t offset = 0;
    const void* buf = nullptr;
    uint32_t len = 0;
    // out
    uint32_t bytesWritten = 0;
    VfsError error = VfsError::None;
};

struct KernelFsStatArgs {
    KernelFsOpCode op = KernelFsOpCode::Stat;
    const char* relPath = nullptr;
    uint32_t relPathLen = 0;
    // out
    uint64_t size = 0;
    bool isDirectory = false;
    VfsError error = VfsError::None;
};

struct KernelFsMkdirArgs {
    KernelFsOpCode op = KernelFsOpCode::Mkdir;
    const char* relPath = nullptr;
    uint32_t relPathLen = 0;
    // out
    VfsError error = VfsError::None;
};

struct KernelFsRmdirArgs {
    KernelFsOpCode op = KernelFsOpCode::Rmdir;
    const char* relPath = nullptr;
    uint32_t relPathLen = 0;
    // out
    VfsError error = VfsError::None;
};

struct KernelFsUnlinkArgs {
    KernelFsOpCode op = KernelFsOpCode::Unlink;
    const char* relPath = nullptr;
    uint32_t relPathLen = 0;
    // out
    VfsError error = VfsError::None;
};

// [갱신, 2026-09-19, PN-770A28FB] `SP-7CC5693A` §3.2가 이미 스케치해
// 둔 미래 `FileSystemDriver::readdir()`(디렉터리를 `Open()`으로 먼저
// 열어 얻은 핸들에 순차적으로 인덱스를 하나씩 조회, POSIX
// opendir()+readdir()과 같은 결)와 정합성을 맞춘 스트리밍 방식으로
// 확정 - `EnumerateDevices`류 배치 방식은 채택하지 않는다(PN-770A28FB
// 조사 결론). 커서(`index`)는 `Process::FileDescriptor::offset`을
// 그대로 재사용한다(Read가 바이트 오프셋으로 쓰는 것과 동일한 관례 -
// 새 커서 상태 테이블을 별도로 두지 않는다, RM-23F4B687 §4) - 값
// 자체는 "이 디렉터리에서 몇 번째 엔트리를 요청하는지"를 뜻하는
// 0-based 인덱스.
struct VfsDirEntry {
    char name[64] = {};
    uint32_t nameLength = 0;
    bool isDirectory = false;
};

struct KernelFsReaddirArgs {
    KernelFsOpCode op = KernelFsOpCode::Readdir;
    FileHandle dirHandle;  // in: Open()이 돌려준 디렉터리 핸들
    uint64_t index = 0;    // in: 0부터 시작하는 조회 인덱스
    // out
    VfsDirEntry entry;
    bool hasMore = false;  // true면 entry가 이번 인덱스의 유효한 항목, false면 이미 끝(entry 무의미, Read의 EOF와 동일한 뜻)
    VfsError error = VfsError::None;
};

class KernelFsDriver : public AsyncTaskHandler {
public:
    // `MountTable::mountKernel()`이 `AsyncCallbackRegistry::
    // registerHandler(this)`로 발급받은 코드를 여기 채운다 - 실제
    // syscall 배선(§9 착수 이후)이 `AsyncTask::submit(subjectCode(),
    // ...)`로 이 드라이버에 op를 제출하는 데 쓴다. 0도 유효한
    // subjectCode일 수 있어(가장 먼저 등록되는 핸들러가 받음) 별도
    // `_registered` 플래그로 "아직 등록 안 됨"을 구분한다.
    AsyncTaskSubjectCode subjectCode() const { return _subjectCode; }
    bool hasSubjectCode() const { return _registered; }
    void setSubjectCode(AsyncTaskSubjectCode code) {
        _subjectCode = code;
        _registered = true;
    }

private:
    AsyncTaskSubjectCode _subjectCode = 0;
    bool _registered = false;
};

struct MountEntry {
    char path[kMaxMountPathLen] = {};
    uint32_t pathLen = 0;
    MountKind kind = MountKind::Channel;
    uint64_t ownerChannelId = 0;       // kind == Channel일 때만 유효
    KernelFsDriver* kernelDriver = nullptr;  // kind == KernelDriver일 때만 유효
    bool used = false;
};

class MountTable {
public:
    // 부팅 시 한 번(BSP) - 테이블을 빈 상태로 리셋한다.
    static void init();

    // 최장 접두사 일치로 path를 담당하는 마운트를 찾는다(§2.1) - 마운트
    // 경로 자체와 정확히 같거나, 마운트 경로 + '/'로 시작해야 매치로
    // 인정한다("/sys/livex"가 "/sys/live" 마운트에 잘못 매치되는 것을
    // 방지). kind까지 함께 반환 - 호출부가 Channel이면 그 채널ID로
    // 유저에게 라우팅 정보를 돌려주고, KernelDriver면 이 자리에서 곧장
    // 그 드라이버의 open/read를 대행 호출한다(IPC 왕복 없음).
    // outRelOffset에는 path 안에서 마운트 경로 접두사(+ 있으면 구분자
    // '/')를 뺀 나머지가 시작하는 위치를 채운다.
    static bool resolve(const char* path, uint32_t pathLen,
                         MountKind* outKind, uint64_t* outChannelId,
                         KernelFsDriver** outKernelDriver, uint32_t* outRelOffset);

    // 유저랜드 fs 서비스용(§2.2 Mount syscall이 이걸 호출) - 이미
    // 마운트된 경로거나 테이블이 가득 찼으면 false.
    static bool mount(const char* path, uint32_t pathLen, uint64_t channelId);

    // 커널 자신이 부팅 시퀀스(kmain.cpp)에서 직접 호출 - syscall이
    // 아니다(유저 프로세스가 커널 드라이버를 마운트시킬 이유가 없음).
    static bool mountKernel(const char* path, uint32_t pathLen, KernelFsDriver* driver);

    // 정확히 일치하는 경로의 마운트를 해제한다 - 없으면 false.
    static bool unmount(const char* path, uint32_t pathLen);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_MOUNT_TABLE_H
