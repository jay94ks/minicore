#include "livefs.h"

#include "channel.h"
#include "kernel_service_ring.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "named_object.h"
#include "process.h"
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
    entry.tierBChannelId = reinterpret_cast<uint64_t>(channel);
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
            kLiveFsStatImpl(static_cast<KernelFsStatArgs*>(argsRaw));
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
            // v1 미구현 - mount_table.h 문서 주석 참고(디렉터리 엔트리
            // 나열 ABI 자체가 아직 설계돼 있지 않음).
            auto* args = static_cast<KernelFsReaddirArgs*>(argsRaw);
            args->entryCount = 0;
            args->error = VfsError::InvalidArgument;
            break;
        }
    }
    co_return;
}

}  // namespace kernel
