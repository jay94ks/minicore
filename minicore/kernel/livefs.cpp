#include "livefs.h"

#include "channel.h"
#include "kernel_service_ring.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "named_object.h"
#include "procfs.h"
#include "process.h"
#include "resource_group.h"
#include "scheduler.h"
#include "syscall.h"

namespace {

kernel::KernelReservedEntry gEntries[kernel::kMaxKernelReservedEntries];

kernel::uint8_t gLiveFsCpioBuffer[kernel::kMaxLiveFsCpioSize];
kernel::uint64_t gLiveFsCpioSize = 0;
bool gLiveFsCpioFound = false;

// initrd.cpio 핸들은 상태가 없다(그 자체가 곧 "그 아카이브 전체"라는
// 뜻) - 실제 포인터/객체 대신 고정된 sentinel 값 하나로 충분하다.
constexpr kernel::uint64_t kInitrdCpioHandleValue = 1;

// [신규, 2026-09-19, PN-770A28FB] `/sys/live` 루트 디렉터리 핸들 -
// 실제 객체가 없는(named/kernel/proc/resourcegroup 네 하위 경로 +
// initrd.cpio 파일 하나를 나열하는) 고정 sentinel. `kProcFsHandleTagBit`
// (비트1)/`kProcFsGlobalHandleBit`(비트2)/`kResourceGroupHandleTagBit`
// (비트3)/`kResourceGroupRootHandleBit`(비트4)/`kInitrdCpioHandleValue`
// (=1) 어느 것과도 겹치지 않도록 비트5를 쓴다.
constexpr kernel::uint64_t kLiveFsRootDirHandleValue = 1ULL << 5;

// [신규, 2026-09-19, PN-770A28FB] `/sys/live/named`/`/sys/live/kernel`
// 디렉터리 핸들 - 위 루트 핸들과 같은 이유로 상태 없는 고정
// sentinel(각각 `NamedObjectTable`/`KernelReservedTable` 전체를
// 나열한다는 뜻일 뿐, 특정 인스턴스를 가리키지 않음). 비트6/7을 써서
// 루트 핸들(비트5)과도 겹치지 않는다.
constexpr kernel::uint64_t kLiveFsNamedDirHandleValue = 1ULL << 6;
constexpr kernel::uint64_t kLiveFsKernelDirHandleValue = 1ULL << 7;

bool kHasPrefix(const char* s, kernel::uint32_t sLen, const char* prefix, kernel::uint32_t prefixLen) {
    return sLen >= prefixLen && memcmp(s, prefix, prefixLen) == 0;
}

bool kEqualsExact(const char* s, kernel::uint32_t sLen, const char* other, kernel::uint32_t otherLen) {
    return sLen == otherLen && memcmp(s, other, otherLen) == 0;
}

}  // namespace

namespace kernel {

void KernelReservedTable::init() {
    for (uint32_t i = 0; i < kMaxKernelReservedEntries; ++i) {
        gEntries[i] = KernelReservedEntry{};
    }
}

bool KernelReservedTable::reserveForKernelService(const char* name, uint32_t nameLen) {
    if (nameLen == 0 || nameLen > kMaxKernelReservedNameLen) {
        return false;
    }

    uint32_t slot = kMaxKernelReservedEntries;
    for (uint32_t i = 0; i < kMaxKernelReservedEntries; ++i) {
        if (!gEntries[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxKernelReservedEntries) {
        return false;  // v1 상한 초과 - devmgr/fs/net/tty 4개보다 많이 예약할 일이 아직 없음
    }

    // Tier B - kCreateNamedChannel(§2.0a)로 만들되 이름 없이(name=nullptr)
    // 만든다: NamedObjectTable에 등록하지 않아야 "kernel/<name>" 문자열을
    // 아는 것만으로는 connectChannel()할 수 없다(§2.0 안전성 근거 그대로 -
    // 오직 이 표를 거쳐야만 ChannelId를 얻는다).
    ChannelError channelError = ChannelError::None;
    Channel* channel = kCreateNamedChannel(nullptr, 0, &channelError);
    if (!channel) {
        return false;
    }
    channel->exclusivePreemptive = true;

    // Tier A - 실사용 소비자가 없어도 설계자 지시로 자리를 만들어 둔다.
    // 할당 실패는 이 예약 전체를 실패시키지 않는다(Tier A는 "아직 아무도
    // 안 쓰는" 자리라 없어도 Tier B만으로 서비스 스폰 자체는 계속
    // 진행돼야 함) - tierA는 nullptr로 남는다.
    void* ringMem = GenericSlabAllocator::alloc(sizeof(KernelServiceSharedRingBuffer));
    KernelServiceSharedRingBuffer* ring = nullptr;
    if (ringMem) {
        ring = reinterpret_cast<KernelServiceSharedRingBuffer*>(ringMem);
        ring->reset();
    }

    KernelReservedEntry& entry = gEntries[slot];
    entry = KernelReservedEntry{};
    memcpy(entry.name, name, nameLen);
    entry.nameLen = nameLen;
    entry.tierA = ring;
    // [수정, PN-CE6A04AB] 더 이상 raw 포인터가 아니다 - kCreateNamedChannel()
    // 이 이미 안전한 ChannelId를 발급해 channel->channelId에 담아 뒀다.
    entry.tierBChannelId = channel->channelId;
    entry.used = true;
    return true;
}

KernelReservedEntry* KernelReservedTable::find(const char* name, uint32_t nameLen) {
    if (nameLen == 0 || nameLen > kMaxKernelReservedNameLen) {
        return nullptr;
    }
    for (uint32_t i = 0; i < kMaxKernelReservedEntries; ++i) {
        KernelReservedEntry& entry = gEntries[i];
        if (!entry.used || entry.nameLen != nameLen) {
            continue;
        }
        bool matches = true;
        for (uint32_t j = 0; j < nameLen; ++j) {
            if (entry.name[j] != name[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return &entry;
        }
    }
    return nullptr;
}

bool KernelReservedTable::getByIndex(uint32_t index, char* outName, uint32_t* outNameLength) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < kMaxKernelReservedEntries; ++i) {
        if (!gEntries[i].used) {
            continue;
        }
        if (seen == index) {
            memcpy(outName, gEntries[i].name, gEntries[i].nameLen);
            *outNameLength = gEntries[i].nameLen;
            return true;
        }
        ++seen;
    }
    return false;
}

LiveFs& LiveFs::instance() {
    static LiveFs gInstance;
    return gInstance;
}

bool LiveFs::captureCpioArchive(const void* archive, uint64_t archiveSize) {
    if (gLiveFsCpioFound) {
        return false;  // 이미 첫 CPIO 모듈을 채택했음 - 이후 모듈은 무시(§2.4 "하나의 initrd" 전제)
    }
    if (!archive || archiveSize == 0 || archiveSize > kMaxLiveFsCpioSize) {
        return false;
    }
    memcpy(gLiveFsCpioBuffer, archive, archiveSize);
    gLiveFsCpioSize = archiveSize;
    gLiveFsCpioFound = true;
    return true;
}

namespace {

// [수정, 2026-09-17, PN-BC04D3DC 변환 중 발견] 예전엔 `Scheduler::
// currentTask()`를 직접 불러 "kernel/" 분기의 호출자 신원을 얻었다 -
// 이 코드가 동기 가상함수로 직접 호출되던 시절(§9 syscall 배선이
// 없어 실제로는 아무도 안 부르던 상태)엔 우연히 안전했지만, 이제
// `onExec()`을 통해 실행되면(AsyncReactor가 나중에 idle 경로에서
// 실행) `Scheduler::currentTask()`가 제출자가 아니라 리액터 자신의
// 컨텍스트를 가리키는 그 익숙한 버그(async_task.h의 submitterTask
// 문서 주석, PN-DB5153B6/PN-9CC66142/PN-523B779F가 이미 겪은 것과
// 동일한 클래스)를 그대로 재현하게 된다 - `task->submitterTask.
// lock()` 체이닝으로 고쳤다(pnp.cpp의 kProcessFromSubmitterForPnp와
// 동일한 패턴).
kernel::OpenResult kLiveFsOpenImpl(kernel::AsyncTask* task, const char* relPath, kernel::uint32_t relPathLen,
                                    kernel::uint32_t /*flags*/) {
    static constexpr char kNamedPrefix[] = "named/";
    static constexpr char kKernelPrefix[] = "kernel/";
    static constexpr char kInitrdCpioPath[] = "initrd.cpio";
    static constexpr char kProcPrefix[] = "proc/";
    static constexpr char kResourceGroupPrefix[] = "resourcegroup/";

    // [신규, 2026-09-19, PN-770A28FB] "/sys/live" 자체(마운트 경로와
    // 정확히 일치, 접두사 없는 빈 relPath) - 루트 디렉터리.
    if (relPathLen == 0) {
        return kernel::OpenResult{kernel::FileHandle{kLiveFsRootDirHandleValue}, true, kernel::VfsError::None};
    }

    // [신규, 2026-09-19, PN-770A28FB] "named"/"kernel" 자신(끝에 "/"
    // 없이 정확히 일치) - 그 하위 이름을 나열하는 디렉터리.
    if (kEqualsExact(relPath, relPathLen, kNamedPrefix, sizeof(kNamedPrefix) - 2)) {
        return kernel::OpenResult{kernel::FileHandle{kLiveFsNamedDirHandleValue}, true, kernel::VfsError::None};
    }
    if (kEqualsExact(relPath, relPathLen, kKernelPrefix, sizeof(kKernelPrefix) - 2)) {
        return kernel::OpenResult{kernel::FileHandle{kLiveFsKernelDirHandleValue}, true, kernel::VfsError::None};
    }

    if (kHasPrefix(relPath, relPathLen, kProcPrefix, sizeof(kProcPrefix) - 1)) {
        const char* rest = relPath + (sizeof(kProcPrefix) - 1);
        const kernel::uint32_t restLen = relPathLen - (sizeof(kProcPrefix) - 1);
        return kernel::ProcFs::open(task, rest, restLen, 0);
    }

    if (kHasPrefix(relPath, relPathLen, kResourceGroupPrefix, sizeof(kResourceGroupPrefix) - 1)) {
        const char* rest = relPath + (sizeof(kResourceGroupPrefix) - 1);
        const kernel::uint32_t restLen = relPathLen - (sizeof(kResourceGroupPrefix) - 1);
        return kernel::ResourceGroupFs::open(rest, restLen);
    }

    if (kHasPrefix(relPath, relPathLen, kNamedPrefix, sizeof(kNamedPrefix) - 1)) {
        const char* name = relPath + (sizeof(kNamedPrefix) - 1);
        const kernel::uint32_t nameLen = relPathLen - (sizeof(kNamedPrefix) - 1);
        kernel::NamedObjectKind kind = kernel::NamedObjectKind::Channel;
        kernel::uint64_t objectId = 0;
        if (!kernel::NamedObjectTable::resolve(name, static_cast<kernel::uint64_t>(nameLen), &kind, &objectId)) {
            return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
        }
        return kernel::OpenResult{kernel::FileHandle{objectId}, false, kernel::VfsError::None};
    }

    if (kEqualsExact(relPath, relPathLen, kInitrdCpioPath, sizeof(kInitrdCpioPath) - 1)) {
        if (!gLiveFsCpioFound) {
            return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
        }
        return kernel::OpenResult{kernel::FileHandle{kInitrdCpioHandleValue}, false, kernel::VfsError::None};
    }

    if (kHasPrefix(relPath, relPathLen, kKernelPrefix, sizeof(kKernelPrefix) - 1)) {
        const char* name = relPath + (sizeof(kKernelPrefix) - 1);
        const kernel::uint32_t nameLen = relPathLen - (sizeof(kKernelPrefix) - 1);
        kernel::KernelReservedEntry* entry = kernel::KernelReservedTable::find(name, nameLen);
        if (!entry) {
            return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
        }

        // 호출자 신원 검사(SP-00CA7175 §2.0/RM-C65F7760) - 이 표를
        // 아는 것만으로는 부족하고, 호출자 자신이 정확히 그 이름으로
        // 스폰된 KernelService 프로세스여야 한다.
        kernel::SharedPtr<kernel::Task> submitter = task->submitterTask.lock();
        if (!submitter) {
            return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::PermissionDenied};
        }
        auto* callerThread = static_cast<kernel::UserThread*>(submitter.get());
        kernel::SharedPtr<kernel::Process> callerProcess = callerThread->process.lock();
        if (!callerProcess || callerProcess->role != kernel::ProcessRole::KernelService ||
            callerProcess->spawnNameLen != nameLen || memcmp(callerProcess->spawnName, name, nameLen) != 0) {
            return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::PermissionDenied};
        }
        return kernel::OpenResult{kernel::FileHandle{entry->tierBChannelId}, false, kernel::VfsError::None};
    }

    return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
}

kernel::ReadResult kLiveFsReadImpl(kernel::FileHandle handle, kernel::uint64_t offset, void* buf,
                                    kernel::uint32_t len) {
    if (handle.value & kernel::kProcFsHandleTagBit) {
        return kernel::ProcFs::read(handle, offset, buf, len);
    }

    if (handle.value & kernel::kResourceGroupHandleTagBit) {
        return kernel::ResourceGroupFs::read(handle, offset, buf, len);
    }

    if (handle.value == kInitrdCpioHandleValue) {
        if (offset >= gLiveFsCpioSize) {
            return kernel::ReadResult{0, kernel::VfsError::None};  // EOF
        }
        const kernel::uint64_t available = gLiveFsCpioSize - offset;
        const kernel::uint32_t toCopy = static_cast<kernel::uint32_t>(available < len ? available : len);
        memcpy(buf, gLiveFsCpioBuffer + offset, toCopy);
        return kernel::ReadResult{toCopy, kernel::VfsError::None};
    }

    // named/kernel 핸들은 실제로는 Channel(objectId/channelId)이다 -
    // "이 핸들에서 바이트를 읽는다"는 의미 자체가 아직 설계돼 있지
    // 않다(§9 착수 시 Channel 기반 fd의 read()가 무엇을 뜻하는지부터
    // 재확정 필요, SP-2AAD7C8D §9.1의 KernelDriver 분기 공백 참고) -
    // 지금은 실패로 명확히 응답한다.
    return kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
}

// [v1] initrd.cpio만 크기 조회가 의미 있다(named/kernel 핸들은
// Channel이라 "파일 크기" 개념 자체가 아직 정의돼 있지 않음 -
// kLiveFsReadImpl의 같은 주석 참고).
void kLiveFsStatImpl(kernel::KernelFsStatArgs* args) {
    static constexpr char kInitrdCpioPath[] = "initrd.cpio";
    static constexpr char kNamedDirName[] = "named";
    static constexpr char kKernelDirName[] = "kernel";
    if (args->relPathLen == 0) {
        // [신규, 2026-09-19, PN-770A28FB] 루트 디렉터리 자신.
        args->size = 0;
        args->isDirectory = true;
        args->error = kernel::VfsError::None;
        return;
    }
    if (kEqualsExact(args->relPath, args->relPathLen, kNamedDirName, sizeof(kNamedDirName) - 1) ||
        kEqualsExact(args->relPath, args->relPathLen, kKernelDirName, sizeof(kKernelDirName) - 1)) {
        // [신규, 2026-09-19, PN-770A28FB] "named"/"kernel" 자신.
        args->size = 0;
        args->isDirectory = true;
        args->error = kernel::VfsError::None;
        return;
    }
    if (kEqualsExact(args->relPath, args->relPathLen, kInitrdCpioPath, sizeof(kInitrdCpioPath) - 1)) {
        if (!gLiveFsCpioFound) {
            args->error = kernel::VfsError::NotFound;
            return;
        }
        args->size = gLiveFsCpioSize;
        args->isDirectory = false;
        args->error = kernel::VfsError::None;
        return;
    }
    args->error = kernel::VfsError::InvalidArgument;
}

// [신규, 2026-09-19, PN-770A28FB] `/sys/live` 루트 나열 - 고정 배열이라
// `index`는 그 배열의 원소 번호 그대로.
struct LiveFsRootEntry {
    const char* name;
    kernel::uint32_t nameLength;
    bool isDirectory;
};

constexpr LiveFsRootEntry kLiveFsRootEntries[] = {
    {"named", 5, true},
    {"kernel", 6, true},
    {"initrd.cpio", 11, false},
    {"proc", 4, true},
    {"resourcegroup", 13, true},
};
constexpr kernel::uint32_t kLiveFsRootEntryCount =
    static_cast<kernel::uint32_t>(sizeof(kLiveFsRootEntries) / sizeof(kLiveFsRootEntries[0]));

void kLiveFsReaddirImpl(kernel::KernelFsReaddirArgs* args) {
    if (args->dirHandle.value == kLiveFsRootDirHandleValue) {
        if (args->index >= kLiveFsRootEntryCount) {
            args->hasMore = false;
            args->error = kernel::VfsError::None;  // 정상 종료(Read의 EOF와 동일한 뜻)
            return;
        }
        const LiveFsRootEntry& entry = kLiveFsRootEntries[static_cast<kernel::uint32_t>(args->index)];
        memcpy(args->entry.name, entry.name, entry.nameLength);
        args->entry.nameLength = entry.nameLength;
        args->entry.isDirectory = entry.isDirectory;
        args->hasMore = true;
        args->error = kernel::VfsError::None;
        return;
    }

    // [신규, 2026-09-19, PN-770A28FB] "named"/"kernel" 나열 -
    // `NamedObjectTable`/`KernelReservedTable`이 각각 실제 저장소를
    // 순회한다. 두 테이블 모두 "이름 붙은 오브젝트 = 파일"이라
    // isDirectory는 항상 false(Channel 엔드포인트 자신은 디렉터리가
    // 아님).
    if (args->dirHandle.value == kLiveFsNamedDirHandleValue) {
        kernel::uint32_t nameLength = 0;
        if (!kernel::NamedObjectTable::getByIndex(static_cast<kernel::uint32_t>(args->index), args->entry.name,
                                                   &nameLength)) {
            args->hasMore = false;
            args->error = kernel::VfsError::None;
            return;
        }
        args->entry.nameLength = nameLength;
        args->entry.isDirectory = false;
        args->hasMore = true;
        args->error = kernel::VfsError::None;
        return;
    }
    if (args->dirHandle.value == kLiveFsKernelDirHandleValue) {
        kernel::uint32_t nameLength = 0;
        if (!kernel::KernelReservedTable::getByIndex(static_cast<kernel::uint32_t>(args->index), args->entry.name,
                                                      &nameLength)) {
            args->hasMore = false;
            args->error = kernel::VfsError::None;
            return;
        }
        args->entry.nameLength = nameLength;
        args->entry.isDirectory = false;
        args->hasMore = true;
        args->error = kernel::VfsError::None;
        return;
    }

    // [v1 축소 범위] 이 핸들이 가리키는 대상이 디렉터리가 아니거나
    // 아직 나열을 지원하지 않는 디렉터리(proc//resourcegroup/ 안쪽 -
    // 각 서비스가 개념상 pid/그룹별 동적 목록이라 후속 과제로 남김)다.
    args->hasMore = false;
    args->error = kernel::VfsError::InvalidHandle;
}

}  // namespace

AsyncExecCoro LiveFs::onExec(AsyncTask* task, void* argsRaw) {
    const auto op = *static_cast<const KernelFsOpCode*>(argsRaw);
    switch (op) {
        case KernelFsOpCode::Open: {
            auto* args = static_cast<KernelFsOpenArgs*>(argsRaw);
            args->result = kLiveFsOpenImpl(task, args->relPath, args->relPathLen, args->flags);
            break;
        }
        case KernelFsOpCode::Close: {
            // v1: 세 하위 경로 전부 상태 없는 핸들(그 자체가 곧 대상
            // 식별자)이라 LiveFs 자신이 정리할 자원이 없다 - 실제
            // 자원(Channel 등)의 수명은 그 자원 자신이 관리한다.
            break;
        }
        case KernelFsOpCode::Read: {
            auto* args = static_cast<KernelFsReadArgs*>(argsRaw);
            args->result = kLiveFsReadImpl(args->handle, args->offset, args->buf, args->len);
            break;
        }
        case KernelFsOpCode::Write: {
            // v1 축소 범위 - mount_table.h 문서 주석 참고(LiveFs 전체가
            // 본질적으로 읽기 전용).
            auto* args = static_cast<KernelFsWriteArgs*>(argsRaw);
            args->bytesWritten = 0;
            args->error = VfsError::PermissionDenied;
            break;
        }
        case KernelFsOpCode::Stat: {
            auto* args = static_cast<KernelFsStatArgs*>(argsRaw);
            static constexpr char kProcPrefix[] = "proc/";
            static constexpr char kResourceGroupPrefix[] = "resourcegroup/";
            if (kHasPrefix(args->relPath, args->relPathLen, kProcPrefix, sizeof(kProcPrefix) - 1)) {
                KernelFsStatArgs procArgs = *args;
                procArgs.relPath = args->relPath + (sizeof(kProcPrefix) - 1);
                procArgs.relPathLen = args->relPathLen - (sizeof(kProcPrefix) - 1);
                ProcFs::stat(task, &procArgs);
                args->size = procArgs.size;
                args->isDirectory = procArgs.isDirectory;
                args->error = procArgs.error;
            } else if (kHasPrefix(args->relPath, args->relPathLen, kResourceGroupPrefix,
                                   sizeof(kResourceGroupPrefix) - 1)) {
                ResourceGroupFs::stat(args->relPath + (sizeof(kResourceGroupPrefix) - 1),
                                      args->relPathLen - (sizeof(kResourceGroupPrefix) - 1), args);
            } else {
                kLiveFsStatImpl(args);
            }
            break;
        }
        case KernelFsOpCode::Mkdir: {
            static_cast<KernelFsMkdirArgs*>(argsRaw)->error = VfsError::PermissionDenied;
            break;
        }
        case KernelFsOpCode::Rmdir: {
            static_cast<KernelFsRmdirArgs*>(argsRaw)->error = VfsError::PermissionDenied;
            break;
        }
        case KernelFsOpCode::Unlink: {
            static_cast<KernelFsUnlinkArgs*>(argsRaw)->error = VfsError::PermissionDenied;
            break;
        }
        case KernelFsOpCode::Readdir: {
            // [갱신, 2026-09-19, PN-770A28FB] 루트 디렉터리 나열 -
            // kLiveFsReaddirImpl 문서 주석 참고(v1 스코프).
            kLiveFsReaddirImpl(static_cast<KernelFsReaddirArgs*>(argsRaw));
            break;
        }
    }
    co_return;
}

}  // namespace kernel
