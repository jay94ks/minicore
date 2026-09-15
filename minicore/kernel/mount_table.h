#ifndef MINICORE_KERNEL_MOUNT_TABLE_H
#define MINICORE_KERNEL_MOUNT_TABLE_H

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

// KernelDriver 종류 마운트가 구현해야 하는 최소 인터페이스(§2.1) -
// §3.2 FileSystemDriver(open/close/read)와 같은 모양이지만 block
// device가 없으므로 mount(BlockDevice*)는 없다.
class KernelFsDriver {
public:
    virtual ~KernelFsDriver() = default;

    virtual OpenResult open(const char* relPath, uint32_t relPathLen, uint32_t flags) = 0;
    virtual void close(FileHandle handle) = 0;
    virtual ReadResult read(FileHandle handle, uint64_t offset, void* buf, uint32_t len) = 0;
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
