#include "device_registry.h"

#include "async_task.h"
#include "block_device.h"
#include "libkenv/mem.h"
#include "libkenv/spinlock.h"
#include "libkmm/slab.h"
#include "process.h"

namespace {

kernel::DeviceRegistryEntry gEntries[kernel::kMaxDeviceRegistryEntries];
kernel::IrqSpinlock gLock;

// [§3.4] 클래스별 단조 증가 카운터 - 이름 충돌 가능성을 원천 차단하는
// 대신 재사용하지 않는다(장치가 빠지면 번호에 구멍이 남을 수 있음 -
// v1은 이를 허용, Linux의 sda/sdb 관례와 동일한 태도).
kernel::uint32_t gNextIndexByClass[2] = {0, 0};  // [Unknown, Block]

const char* kClassPrefix(kernel::DeviceClass cls) {
    switch (cls) {
        case kernel::DeviceClass::Block:
            return "sd";
        default:
            return "dev";
    }
}

// 10진수 변환 - 장치 이름의 순번 접미사(§3.4)와 uid 디렉터리 이름(§3.2
// 합성 나열) 양쪽에 재사용한다.
kernel::uint32_t kFormatUint32Decimal(kernel::uint32_t value, char* outDigitsReversedScratch, char* outBuf,
                                      kernel::uint32_t maxLen) {
    kernel::uint32_t digitCount = 0;
    kernel::uint32_t remaining = value;
    do {
        outDigitsReversedScratch[digitCount++] = static_cast<char>('0' + (remaining % 10));
        remaining /= 10;
    } while (remaining > 0 && digitCount < 10);
    kernel::uint32_t pos = 0;
    for (kernel::uint32_t i = 0; i < digitCount && pos < maxLen; ++i) {
        outBuf[pos++] = outDigitsReversedScratch[digitCount - 1 - i];
    }
    return pos;
}

kernel::uint32_t kFormatDeviceName(kernel::DeviceClass cls, char* outName, kernel::uint32_t maxLen) {
    const char* prefix = kClassPrefix(cls);
    kernel::uint32_t prefixLen = 0;
    while (prefix[prefixLen] != '\0' && prefixLen < maxLen) {
        outName[prefixLen] = prefix[prefixLen];
        ++prefixLen;
    }
    const kernel::uint32_t index = gNextIndexByClass[static_cast<kernel::uint32_t>(cls)]++;
    char scratch[10];
    const kernel::uint32_t digitsWritten =
        kFormatUint32Decimal(index, scratch, outName + prefixLen, maxLen - prefixLen);
    return prefixLen + digitsWritten;
}

kernel::uint32_t kFormatUidName(kernel::Uid uid, char* outName) {
    char scratch[10];
    return kFormatUint32Decimal(uid, scratch, outName, kernel::kMaxDeviceNameLength);
}

bool kNameEquals(const char* name, kernel::uint32_t nameLength, const kernel::DeviceRegistryEntry& entry) {
    return entry.nameLength == nameLength && memcmp(entry.name, name, nameLength) == 0;
}

// --- 핫플러그 이벤트 구독자 테이블(§3.3) ---
// NamedObjectTable/DeviceOwnerTable과 동일한 "고정 슬롯 + 선형 스캔"
// v1 관례. WeakPtr이라 Close()가 명시적으로 등록 해제할 필요가 없다 -
// 브로드캐스트 시점에 lock()이 실패하는 죽은 구독자를 건너뛰고, 그
// 자리는 다음 register 호출이 자연히 재사용한다(DeviceOwnerTable의
// "owner.lock() 지연 GC" 관례와 동일, RM-F2DAFF66 §2-추가 기록 참고).
constexpr kernel::uint32_t kMaxDeviceEventSubscribers = 64;
kernel::WeakPtr<kernel::DeviceEventFdState> gDeviceEventSubscribers[kMaxDeviceEventSubscribers];
kernel::IrqSpinlock gSubscriberLock;

bool kRegisterDeviceEventSubscriber(const kernel::SharedPtr<kernel::DeviceEventFdState>& state) {
    kernel::IrqSpinlockGuard guard(gSubscriberLock);
    for (kernel::uint32_t i = 0; i < kMaxDeviceEventSubscribers; ++i) {
        if (!gDeviceEventSubscribers[i].lock()) {
            gDeviceEventSubscribers[i] = kernel::WeakPtr<kernel::DeviceEventFdState>(state);
            return true;
        }
    }
    return false;  // v1 상한 초과 - 이 fd는 등록되지 않아 향후 이벤트를 못 받는다(실측 후 조정)
}

void kBroadcastDeviceEvent(kernel::DeviceEventKind kind, kernel::DeviceClass cls, const char* name,
                            kernel::uint32_t nameLength) {
    kernel::DeviceEvent event;
    event.kind = kind;
    event.deviceClass = cls;
    memcpy(event.name, name, nameLength);
    event.nameLength = nameLength;

    kernel::SharedPtr<kernel::DeviceEventFdState> states[kMaxDeviceEventSubscribers];
    kernel::uint32_t stateCount = 0;
    {
        kernel::IrqSpinlockGuard guard(gSubscriberLock);
        for (kernel::uint32_t i = 0; i < kMaxDeviceEventSubscribers; ++i) {
            kernel::SharedPtr<kernel::DeviceEventFdState> state = gDeviceEventSubscribers[i].lock();
            if (state) {
                states[stateCount++] = state;
            }
        }
    }
    // signalfd.cpp의 kOnTimerfdFire와 동일한 관례 - 락은 큐 조작+대기자
    // 팝만 감싸고, 실제 깨우기(AsyncReactor::submitCompletion)는 락 밖에서
    // 한다.
    for (kernel::uint32_t i = 0; i < stateCount; ++i) {
        kernel::DeviceEventFdState* state = states[i].get();
        kernel::AsyncTask* wake = nullptr;
        kernel::EpollObserverQueue wakeEpollObservers;
        {
            kernel::SpinlockGuard guard(state->lock);
            state->queue.push(event);
            wake = state->pendingReaders.popFront();
            for (kernel::EpollObserverNode* n = state->epollReadObservers.popFront(); n;
                 n = state->epollReadObservers.popFront()) {
                wakeEpollObservers.pushBack(n);
            }
        }
        if (wake) {
            kernel::AsyncReactor::submitCompletion(wake, /*preemptive=*/true);
        }
        for (kernel::EpollObserverNode* n = wakeEpollObservers.popFront(); n; n = wakeEpollObservers.popFront()) {
            kernel::AsyncReactor::submitCompletion(n->task, /*preemptive=*/true);
        }
    }
}

}  // namespace

namespace kernel {

void DeviceRegistry::init() {
    IrqSpinlockGuard guard(gLock);
    for (uint32_t i = 0; i < kMaxDeviceRegistryEntries; ++i) {
        gEntries[i] = DeviceRegistryEntry{};
    }
    gNextIndexByClass[0] = 0;
    gNextIndexByClass[1] = 0;
}

bool DeviceRegistry::announce(DeviceClass cls, uint64_t backingHandle, Uid ownerUid, Gid ownerGid, uint16_t mode,
                               char* outName, uint32_t* outNameLength) {
    char name[kMaxDeviceNameLength] = {};
    uint32_t nameLength = 0;
    {
        IrqSpinlockGuard guard(gLock);
        int freeSlot = -1;
        for (uint32_t i = 0; i < kMaxDeviceRegistryEntries; ++i) {
            if (!gEntries[i].used) {
                freeSlot = static_cast<int>(i);
                break;
            }
        }
        if (freeSlot < 0) {
            return false;  // v1 상한(64) 초과
        }
        nameLength = kFormatDeviceName(cls, name, kMaxDeviceNameLength);
        DeviceRegistryEntry& entry = gEntries[freeSlot];
        entry = DeviceRegistryEntry{};
        entry.used = true;
        memcpy(entry.name, name, nameLength);
        entry.nameLength = nameLength;
        entry.deviceClass = cls;
        entry.backingHandle = backingHandle;
        entry.ownerUid = ownerUid;
        entry.ownerGid = ownerGid;
        entry.mode = mode;
    }
    memcpy(outName, name, nameLength);
    *outNameLength = nameLength;
    kBroadcastDeviceEvent(DeviceEventKind::Added, cls, name, nameLength);
    return true;
}

void DeviceRegistry::withdraw(const char* name, uint32_t nameLength) {
    DeviceClass cls = DeviceClass::Unknown;
    bool found = false;
    {
        IrqSpinlockGuard guard(gLock);
        for (uint32_t i = 0; i < kMaxDeviceRegistryEntries; ++i) {
            if (gEntries[i].used && kNameEquals(name, nameLength, gEntries[i])) {
                cls = gEntries[i].deviceClass;
                gEntries[i].used = false;
                found = true;
                break;
            }
        }
    }
    if (found) {
        kBroadcastDeviceEvent(DeviceEventKind::Removed, cls, name, nameLength);
    }
}

bool DeviceRegistry::findByName(const char* name, uint32_t nameLength, uint32_t* outRawIndex,
                                 DeviceRegistryEntry* outEntry) {
    IrqSpinlockGuard guard(gLock);
    for (uint32_t i = 0; i < kMaxDeviceRegistryEntries; ++i) {
        if (gEntries[i].used && kNameEquals(name, nameLength, gEntries[i])) {
            *outRawIndex = i;
            *outEntry = gEntries[i];
            return true;
        }
    }
    return false;
}

bool DeviceRegistry::resolveRaw(uint32_t rawIndex, DeviceRegistryEntry* outEntry) {
    if (rawIndex >= kMaxDeviceRegistryEntries) {
        return false;
    }
    IrqSpinlockGuard guard(gLock);
    if (!gEntries[rawIndex].used) {
        return false;
    }
    *outEntry = gEntries[rawIndex];
    return true;
}

bool DeviceRegistry::getByOwnerAndIndex(Uid uid, uint32_t index, DeviceRegistryEntry* outEntry) {
    IrqSpinlockGuard guard(gLock);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < kMaxDeviceRegistryEntries; ++i) {
        if (!gEntries[i].used || gEntries[i].ownerUid != uid) {
            continue;
        }
        if (seen == index) {
            if (outEntry) {
                *outEntry = gEntries[i];
            }
            return true;
        }
        ++seen;
    }
    return false;
}

bool DeviceRegistry::getNestedUidByIndex(uint32_t index, Uid* outUid) {
    IrqSpinlockGuard guard(gLock);
    // 고유 uid 집합을 등장 순서대로 나열 - v1은 O(n^2) 선형 스캔(상한
    // 64개라 단순성 우선, named_object.cpp와 동일한 v1 태도).
    uint32_t seen = 0;
    for (uint32_t i = 0; i < kMaxDeviceRegistryEntries; ++i) {
        if (!gEntries[i].used || gEntries[i].ownerUid == kRootUid) {
            continue;
        }
        bool isFirstOccurrence = true;
        for (uint32_t j = 0; j < i; ++j) {
            if (gEntries[j].used && gEntries[j].ownerUid == gEntries[i].ownerUid) {
                isFirstOccurrence = false;
                break;
            }
        }
        if (!isFirstOccurrence) {
            continue;
        }
        if (seen == index) {
            *outUid = gEntries[i].ownerUid;
            return true;
        }
        ++seen;
    }
    return false;
}

bool DeviceRegistry::tryMarkOpenedRaw(uint32_t rawIndex) {
    if (rawIndex >= kMaxDeviceRegistryEntries) {
        return false;
    }
    IrqSpinlockGuard guard(gLock);
    DeviceRegistryEntry& entry = gEntries[rawIndex];
    if (!entry.used || entry.opened) {
        return false;
    }
    entry.opened = true;
    return true;
}

void DeviceRegistry::markClosedRaw(uint32_t rawIndex) {
    if (rawIndex >= kMaxDeviceRegistryEntries) {
        return;
    }
    IrqSpinlockGuard guard(gLock);
    gEntries[rawIndex].opened = false;
}

}  // namespace kernel

namespace {

// --- DeviceRegistryFs(§3.2) - "/sys/dev" KernelFsDriver ---
//
// 핸들 인코딩: 실제 슬랩/BSS 포인터를 태그 비트와 섞는 resource_group.h
// 관례 대신, 이 드라이버는 전부 고정 크기 정수 공간(rawIndex 0..63,
// uid 0..2^32-1)만 다루므로 충돌 걱정 없는 높은 비트(40/41/42)를 각각
// 전용 태그로 쓴다 - 실제 메모리 주소를 전혀 인코딩하지 않는다(주소
// 배치 가정에 의존하지 않는 더 단순하고 안전한 선택).
constexpr kernel::uint64_t kDeviceRegistryRootDirHandle = 1ULL << 40;
constexpr kernel::uint64_t kDeviceRegistryFileHandleTagBit = 1ULL << 41;  // handle = tag | rawIndex(0..63)
constexpr kernel::uint64_t kDeviceRegistryUidDirTagBit = 1ULL << 42;      // handle = tag | uid(0..2^32-1)

bool kFindSlash(const char* s, kernel::uint32_t len, kernel::uint32_t* outIndex) {
    for (kernel::uint32_t i = 0; i < len; ++i) {
        if (s[i] == '/') {
            *outIndex = i;
            return true;
        }
    }
    return false;
}

bool kParseUid(const char* s, kernel::uint32_t len, kernel::Uid* outUid) {
    if (len == 0 || len > 10) {
        return false;
    }
    kernel::uint64_t value = 0;
    for (kernel::uint32_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        value = value * 10 + static_cast<kernel::uint64_t>(s[i] - '0');
        if (value > 0xFFFFFFFFULL) {
            return false;
        }
    }
    *outUid = static_cast<kernel::Uid>(value);
    return true;
}

// [§3.6] "/sys/dev/<uid>" 서브디렉터리 자체의 열람/진입을 그 uid
// 본인과 root로만 제한(구조적 통제, QU-C726B3C8 답변) - Open() 단계에서
// 판정한다(§3.6 문서 주석 - Readdir은 그 Open이 성공해야만 도달 가능한
// fd로만 호출되므로 별도 검사가 필요 없다).
bool kCallerMatchesUidOrRoot(kernel::AsyncTask* task, kernel::Uid uid) {
    kernel::SharedPtr<kernel::Task> submitter = task->submitterTask.lock();
    if (!submitter || !submitter->isUserLevel) {
        return false;
    }
    auto* thread = static_cast<kernel::UserThread*>(submitter.get());
    kernel::SharedPtr<kernel::Process> process = thread->process.lock();
    if (!process) {
        return false;
    }
    return process->uid == kernel::kRootUid || process->uid == uid;
}

// [§4 결정2] 배타적 Open(항목1) + 마운트된 장치는 Open 불가(항목2) -
// 둘 다 통과해야 실제로 파일 핸들을 내준다.
kernel::OpenResult kOpenDeviceFile(kernel::uint32_t rawIndex, const kernel::DeviceRegistryEntry& entry) {
    if (entry.deviceClass == kernel::DeviceClass::Block && kernel::kIsBackingHandleMounted(entry.backingHandle)) {
        return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::PermissionDenied};
    }
    if (!kernel::DeviceRegistry::tryMarkOpenedRaw(rawIndex)) {
        return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::AlreadyExists};
    }
    return kernel::OpenResult{kernel::FileHandle{kDeviceRegistryFileHandleTagBit | rawIndex}, false,
                               kernel::VfsError::None};
}

kernel::OpenResult kDeviceRegistryFsOpenImpl(kernel::AsyncTask* task, const char* relPath,
                                              kernel::uint32_t relPathLen) {
    if (relPathLen == 0) {
        return kernel::OpenResult{kernel::FileHandle{kDeviceRegistryRootDirHandle}, true, kernel::VfsError::None};
    }

    kernel::uint32_t slashIndex = 0;
    if (!kFindSlash(relPath, relPathLen, &slashIndex)) {
        // 단일 세그먼트 - flat 장치 이름(owner==root) 또는 uid 디렉터리.
        kernel::uint32_t rawIndex = 0;
        kernel::DeviceRegistryEntry entry;
        if (kernel::DeviceRegistry::findByName(relPath, relPathLen, &rawIndex, &entry)) {
            if (entry.ownerUid != kernel::kRootUid) {
                // uid 소유 장치는 평평한 이름으로 못 연다 - "<uid>/<name>"
                // 경로를 거쳐야만 §3.6 게이트를 통과한다.
                return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
            }
            return kOpenDeviceFile(rawIndex, entry);
        }
        kernel::Uid uid = 0;
        kernel::DeviceRegistryEntry probe;
        if (kParseUid(relPath, relPathLen, &uid) && kernel::DeviceRegistry::getByOwnerAndIndex(uid, 0, &probe)) {
            if (!kCallerMatchesUidOrRoot(task, uid)) {
                return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::PermissionDenied};
            }
            return kernel::OpenResult{kernel::FileHandle{kDeviceRegistryUidDirTagBit | uid}, true,
                                       kernel::VfsError::None};
        }
        return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
    }

    // "<uid>/<name>" (또는 "<uid>/") 형태.
    kernel::Uid uid = 0;
    if (!kParseUid(relPath, slashIndex, &uid)) {
        return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
    }
    if (!kCallerMatchesUidOrRoot(task, uid)) {
        return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::PermissionDenied};
    }
    const char* namePart = relPath + slashIndex + 1;
    const kernel::uint32_t namePartLen = relPathLen - slashIndex - 1;
    if (namePartLen == 0) {
        kernel::DeviceRegistryEntry probe;
        if (kernel::DeviceRegistry::getByOwnerAndIndex(uid, 0, &probe)) {
            return kernel::OpenResult{kernel::FileHandle{kDeviceRegistryUidDirTagBit | uid}, true,
                                       kernel::VfsError::None};
        }
        return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
    }
    kernel::uint32_t rawIndex = 0;
    kernel::DeviceRegistryEntry entry;
    if (!kernel::DeviceRegistry::findByName(namePart, namePartLen, &rawIndex, &entry) || entry.ownerUid != uid) {
        return kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
    }
    return kOpenDeviceFile(rawIndex, entry);
}

// [§4 결정2 항목3] Block 노드 raw 섹터 I/O - offset/len은 블록 크기
// 정렬을 강제한다(정렬 안 맞으면 InvalidArgument, §4 원문 그대로).
// `Unknown` 클래스는 raw I/O 의미론이 정의돼 있지 않아 NotSupported.
kernel::ReadResult kDeviceRegistryFsReadImpl(kernel::FileHandle handle, kernel::uint64_t offset, void* buf,
                                              kernel::uint32_t len) {
    if ((handle.value & kDeviceRegistryFileHandleTagBit) == 0) {
        return kernel::ReadResult{0, kernel::VfsError::InvalidHandle};  // 디렉터리 핸들로 Read 시도
    }
    const auto rawIndex = static_cast<kernel::uint32_t>(handle.value & ~kDeviceRegistryFileHandleTagBit);
    kernel::DeviceRegistryEntry entry;
    if (!kernel::DeviceRegistry::resolveRaw(rawIndex, &entry)) {
        return kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
    }
    if (entry.deviceClass != kernel::DeviceClass::Block) {
        return kernel::ReadResult{0, kernel::VfsError::NotSupported};
    }
    auto* device = reinterpret_cast<fs::BlockDevice*>(entry.backingHandle);
    const kernel::uint32_t blockSize = device->blockSize();
    if (blockSize == 0 || len == 0 || offset % blockSize != 0 || len % blockSize != 0) {
        return kernel::ReadResult{0, kernel::VfsError::InvalidArgument};
    }
    const kernel::uint64_t lba = offset / blockSize;
    const kernel::uint32_t count = len / blockSize;
    fs::BlockIoResult ioResult;
    kernel::AsyncTask* ioTask = device->submitReadBlocks(lba, buf, count, &ioResult);
    if (!ioTask) {
        return kernel::ReadResult{0, kernel::VfsError::InvalidArgument};
    }
    kernel::AsyncTaskAwaiter(ioTask).await();
    if (!ioResult.ok) {
        return kernel::ReadResult{0, kernel::VfsError::InvalidArgument};
    }
    return kernel::ReadResult{len, kernel::VfsError::None};
}

void kDeviceRegistryFsWriteImpl(kernel::KernelFsWriteArgs* args) {
    if ((args->handle.value & kDeviceRegistryFileHandleTagBit) == 0) {
        args->bytesWritten = 0;
        args->error = kernel::VfsError::InvalidHandle;
        return;
    }
    const auto rawIndex = static_cast<kernel::uint32_t>(args->handle.value & ~kDeviceRegistryFileHandleTagBit);
    kernel::DeviceRegistryEntry entry;
    if (!kernel::DeviceRegistry::resolveRaw(rawIndex, &entry)) {
        args->bytesWritten = 0;
        args->error = kernel::VfsError::InvalidHandle;
        return;
    }
    if (entry.deviceClass != kernel::DeviceClass::Block) {
        args->bytesWritten = 0;
        args->error = kernel::VfsError::NotSupported;
        return;
    }
    auto* device = reinterpret_cast<fs::BlockDevice*>(entry.backingHandle);
    const kernel::uint32_t blockSize = device->blockSize();
    if (blockSize == 0 || args->len == 0 || args->offset % blockSize != 0 || args->len % blockSize != 0) {
        args->bytesWritten = 0;
        args->error = kernel::VfsError::InvalidArgument;
        return;
    }
    const kernel::uint64_t lba = args->offset / blockSize;
    const kernel::uint32_t count = args->len / blockSize;
    fs::BlockIoResult ioResult;
    kernel::AsyncTask* ioTask = device->submitWriteBlocks(lba, args->buf, count, &ioResult);
    if (!ioTask) {
        args->bytesWritten = 0;
        args->error = kernel::VfsError::InvalidArgument;
        return;
    }
    kernel::AsyncTaskAwaiter(ioTask).await();
    if (!ioResult.ok) {
        args->bytesWritten = 0;
        args->error = kernel::VfsError::InvalidArgument;
        return;
    }
    args->bytesWritten = args->len;
    args->error = kernel::VfsError::None;
}

// [정직하게 기록] Stat은 SP §3.6이 명시적으로 게이트를 건 대상
// (Open/Readdir)이 아니다 - 경로 기반 KernelFsStatArgs에는 애초에
// 호출자 신원이 실려 오지 않는다(livefs의 named/kernel Stat과 동일한
// 한계). 존재 여부/메타데이터 노출 자체는 이번 범위에서 허용한다.
void kDeviceRegistryFsStatImpl(kernel::KernelFsStatArgs* args) {
    if (args->relPathLen == 0) {
        args->size = 0;
        args->type = kernel::FileType::Directory;
        args->uid = kernel::kRootUid;
        args->gid = kernel::kRootGid;
        args->mode = kernel::kPermOwnerRead | kernel::kPermOwnerExec | kernel::kPermGroupRead |
                     kernel::kPermGroupExec | kernel::kPermOtherRead | kernel::kPermOtherExec;
        args->error = kernel::VfsError::None;
        return;
    }
    kernel::uint32_t slashIndex = 0;
    if (!kFindSlash(args->relPath, args->relPathLen, &slashIndex)) {
        kernel::uint32_t rawIndex = 0;
        kernel::DeviceRegistryEntry entry;
        if (kernel::DeviceRegistry::findByName(args->relPath, args->relPathLen, &rawIndex, &entry) &&
            entry.ownerUid == kernel::kRootUid) {
            args->size = 0;
            args->type = kernel::FileType::Regular;
            args->uid = entry.ownerUid;
            args->gid = entry.ownerGid;
            args->mode = entry.mode;
            args->error = kernel::VfsError::None;
            return;
        }
        kernel::Uid uid = 0;
        kernel::DeviceRegistryEntry probe;
        if (kParseUid(args->relPath, args->relPathLen, &uid) &&
            kernel::DeviceRegistry::getByOwnerAndIndex(uid, 0, &probe)) {
            args->size = 0;
            args->type = kernel::FileType::Directory;
            args->uid = uid;
            args->gid = kernel::kRootGid;
            args->mode = kernel::kPermOwnerRead | kernel::kPermOwnerExec;  // §3.6 - 본인+root만 실제 진입 가능
            args->error = kernel::VfsError::None;
            return;
        }
        args->error = kernel::VfsError::NotFound;
        return;
    }
    kernel::Uid uid = 0;
    if (!kParseUid(args->relPath, slashIndex, &uid)) {
        args->error = kernel::VfsError::NotFound;
        return;
    }
    const char* namePart = args->relPath + slashIndex + 1;
    const kernel::uint32_t namePartLen = args->relPathLen - slashIndex - 1;
    kernel::uint32_t rawIndex = 0;
    kernel::DeviceRegistryEntry entry;
    if (namePartLen > 0 && kernel::DeviceRegistry::findByName(namePart, namePartLen, &rawIndex, &entry) &&
        entry.ownerUid == uid) {
        args->size = 0;
        args->type = kernel::FileType::Regular;
        args->uid = entry.ownerUid;
        args->gid = entry.ownerGid;
        args->mode = entry.mode;
        args->error = kernel::VfsError::None;
        return;
    }
    args->error = kernel::VfsError::NotFound;
}

void kDeviceRegistryFsReaddirImpl(kernel::KernelFsReaddirArgs* args) {
    if (args->dirHandle.value == kDeviceRegistryRootDirHandle) {
        const auto index = static_cast<kernel::uint32_t>(args->index);
        kernel::DeviceRegistryEntry entry;
        if (kernel::DeviceRegistry::getByOwnerAndIndex(kernel::kRootUid, index, &entry)) {
            memcpy(args->entry.name, entry.name, entry.nameLength);
            args->entry.nameLength = entry.nameLength;
            args->entry.isDirectory = false;
            args->hasMore = true;
            args->error = kernel::VfsError::None;
            return;
        }
        kernel::uint32_t rootCount = 0;
        {
            kernel::DeviceRegistryEntry tmp;
            while (kernel::DeviceRegistry::getByOwnerAndIndex(kernel::kRootUid, rootCount, &tmp)) {
                ++rootCount;
            }
        }
        kernel::Uid uid = 0;
        if (index >= rootCount && kernel::DeviceRegistry::getNestedUidByIndex(index - rootCount, &uid)) {
            args->entry.nameLength = kFormatUidName(uid, args->entry.name);
            args->entry.isDirectory = true;
            args->hasMore = true;
            args->error = kernel::VfsError::None;
            return;
        }
        args->hasMore = false;
        args->error = kernel::VfsError::None;
        return;
    }
    if (args->dirHandle.value & kDeviceRegistryUidDirTagBit) {
        const auto uid = static_cast<kernel::Uid>(args->dirHandle.value & 0xFFFFFFFFULL);
        kernel::DeviceRegistryEntry entry;
        if (kernel::DeviceRegistry::getByOwnerAndIndex(uid, static_cast<kernel::uint32_t>(args->index), &entry)) {
            memcpy(args->entry.name, entry.name, entry.nameLength);
            args->entry.nameLength = entry.nameLength;
            args->entry.isDirectory = false;
            args->hasMore = true;
            args->error = kernel::VfsError::None;
            return;
        }
        args->hasMore = false;
        args->error = kernel::VfsError::None;
        return;
    }
    args->hasMore = false;
    args->error = kernel::VfsError::InvalidHandle;
}

}  // namespace

namespace kernel {

DeviceRegistryFs& DeviceRegistryFs::instance() {
    static DeviceRegistryFs gInstance;
    return gInstance;
}

AsyncExecCoro DeviceRegistryFs::onExec(AsyncTask* task, void* argsRaw) {
    const auto op = *static_cast<const KernelFsOpCode*>(argsRaw);
    switch (op) {
        case KernelFsOpCode::Open: {
            auto* args = static_cast<KernelFsOpenArgs*>(argsRaw);
            args->result = kDeviceRegistryFsOpenImpl(task, args->relPath, args->relPathLen);
            break;
        }
        case KernelFsOpCode::Close: {
            auto* args = static_cast<KernelFsCloseArgs*>(argsRaw);
            if (args->handle.value & kDeviceRegistryFileHandleTagBit) {
                const auto rawIndex = static_cast<uint32_t>(args->handle.value & ~kDeviceRegistryFileHandleTagBit);
                DeviceRegistry::markClosedRaw(rawIndex);
            }
            break;
        }
        case KernelFsOpCode::Read: {
            auto* args = static_cast<KernelFsReadArgs*>(argsRaw);
            args->result = kDeviceRegistryFsReadImpl(args->handle, args->offset, args->buf, args->len);
            break;
        }
        case KernelFsOpCode::Write: {
            kDeviceRegistryFsWriteImpl(static_cast<KernelFsWriteArgs*>(argsRaw));
            break;
        }
        case KernelFsOpCode::Stat: {
            kDeviceRegistryFsStatImpl(static_cast<KernelFsStatArgs*>(argsRaw));
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
        // §3.6 확정대로 Chmod/Chown은 v1에 노출하지 않는다(livefs의
        // 읽기 전용 태도와 동일 - producer가 announce() 시점에 고정한
        // uid/gid/mode만 쓴다).
        case KernelFsOpCode::Chmod: {
            static_cast<KernelFsChmodArgs*>(argsRaw)->error = VfsError::PermissionDenied;
            break;
        }
        case KernelFsOpCode::Chown: {
            static_cast<KernelFsChownArgs*>(argsRaw)->error = VfsError::NotSupported;
            break;
        }
        case KernelFsOpCode::Readdir: {
            kDeviceRegistryFsReaddirImpl(static_cast<KernelFsReaddirArgs*>(argsRaw));
            break;
        }
    }
    co_return;
}

}  // namespace kernel

namespace {

// --- DeviceEventsOpen(§3.3) syscall handler ---
// signalfd.cpp의 SignalfdCreateHandler와 동일한 골격(fd 할당 +
// FileDescriptor 등록) - 차이는 이 fd가 프로세스별 상태가 아니라
// 전역 DeviceRegistry 이벤트를 구독한다는 것뿐이다.

kernel::SharedPtr<kernel::Process> kDeviceEventsProcessFromSubmitter(kernel::AsyncTask* task) {
    kernel::SharedPtr<kernel::Task> submitter = task->submitterTask.lock();
    if (!submitter || !submitter->isUserLevel) {
        return kernel::SharedPtr<kernel::Process>();
    }
    auto* thread = static_cast<kernel::UserThread*>(submitter.get());
    return thread->process.lock();
}

constexpr kernel::int32_t kMaxFileDescriptorValue = 1024;
kernel::int32_t kAllocateDeviceEventsFd(kernel::Process* process) {
    for (kernel::int32_t candidate = 0; candidate < kMaxFileDescriptorValue; ++candidate) {
        if (!process->fileDescriptors.find(
                [candidate](const kernel::Process::FileDescriptor& e) { return e.fd == candidate; })) {
            return candidate;
        }
    }
    return -1;
}

class DeviceEventsOpenHandler : public kernel::AsyncTaskHandler {
public:
    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<kernel::DeviceEventsOpenArgs*>(argsRaw);
        kernel::SharedPtr<kernel::Process> process = kDeviceEventsProcessFromSubmitter(task);
        if (!process) {
            args->error = kernel::ChannelError::InvalidHandle;
            co_return;
        }
        void* mem = kernel::GenericSlabAllocator::alloc(sizeof(kernel::DeviceEventFdState));
        if (!mem) {
            args->error = kernel::ChannelError::ResourceExhausted;
            co_return;
        }
        memset(mem, 0, sizeof(kernel::DeviceEventFdState));
        auto* raw = reinterpret_cast<kernel::DeviceEventFdState*>(mem);
        kernel::SharedPtr<kernel::DeviceEventFdState> state = kernel::kMakeShared<kernel::DeviceEventFdState>(raw);
        if (!state) {
            kernel::GenericSlabAllocator::free(mem, sizeof(kernel::DeviceEventFdState));
            args->error = kernel::ChannelError::ResourceExhausted;
            co_return;
        }
        if (!kRegisterDeviceEventSubscriber(state)) {
            args->error = kernel::ChannelError::ResourceExhausted;
            co_return;
        }
        process->fileDescriptors.ensureAllocator(&kernel::GenericSlabAllocator::alloc,
                                                  &kernel::GenericSlabAllocator::free);
        const kernel::int32_t newFd = kAllocateDeviceEventsFd(process.get());
        if (newFd < 0) {
            args->error = kernel::ChannelError::ResourceExhausted;
            co_return;
        }
        kernel::Process::FileDescriptor fdEntry;
        fdEntry.fd = newFd;
        fdEntry.kind = kernel::MountKind::DeviceEvents;
        fdEntry.deviceEvents = state;
        fdEntry.used = true;
        if (!process->fileDescriptors.insert(fdEntry)) {
            args->error = kernel::ChannelError::ResourceExhausted;
            co_return;
        }
        args->fd = newFd;
        args->error = kernel::ChannelError::None;
        co_return;
    }
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}
};

DeviceEventsOpenHandler gDeviceEventsOpenHandler;

}  // namespace

namespace kernel {

void DeviceEventsService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointDeviceEventsOpen, &gDeviceEventsOpenHandler);
}

}  // namespace kernel
