#include "mount_table.h"

namespace {

kernel::MountEntry gEntries[kernel::kMaxMountEntries];

// path가 entry(mount 경로)에 매치되면 true를 반환하고 relOffset을
// 채운다 - 정확히 같거나, entry 뒤에 '/'가 이어지는 경우만 매치로
// 인정한다(kernel::MountTable::resolve 헤더 주석 참고).
bool kMatchesMount(const kernel::MountEntry& entry, const char* path, kernel::uint32_t pathLen,
                    kernel::uint32_t* outRelOffset) {
    if (pathLen < entry.pathLen) {
        return false;
    }
    for (kernel::uint32_t i = 0; i < entry.pathLen; ++i) {
        if (path[i] != entry.path[i]) {
            return false;
        }
    }
    if (pathLen == entry.pathLen) {
        *outRelOffset = pathLen;
        return true;
    }
    if (path[entry.pathLen] != '/') {
        return false;
    }
    *outRelOffset = entry.pathLen + 1;
    return true;
}

}  // namespace

namespace kernel {

void MountTable::init() {
    for (uint32_t i = 0; i < kMaxMountEntries; ++i) {
        gEntries[i] = MountEntry{};
    }
}

bool MountTable::resolve(const char* path, uint32_t pathLen, MountKind* outKind,
                          uint64_t* outChannelId, KernelFsDriver** outKernelDriver,
                          uint32_t* outRelOffset) {
    if (!path || pathLen == 0) {
        return false;
    }

    // 최장 접두사 일치 - 전체 선형 스캔 중 가장 긴 pathLen을 가진
    // 매치를 채택한다(마운트 개수가 v1 상한 수준에서는 무의미한
    // 비용, §2.1 주석 그대로).
    MountEntry* best = nullptr;
    uint32_t bestRelOffset = 0;
    for (uint32_t i = 0; i < kMaxMountEntries; ++i) {
        MountEntry& entry = gEntries[i];
        if (!entry.used) {
            continue;
        }
        uint32_t relOffset = 0;
        if (!kMatchesMount(entry, path, pathLen, &relOffset)) {
            continue;
        }
        if (!best || entry.pathLen > best->pathLen) {
            best = &entry;
            bestRelOffset = relOffset;
        }
    }
    if (!best) {
        return false;
    }

    if (outKind) {
        *outKind = best->kind;
    }
    if (outChannelId) {
        *outChannelId = best->ownerChannelId;
    }
    if (outKernelDriver) {
        *outKernelDriver = best->kernelDriver;
    }
    if (outRelOffset) {
        *outRelOffset = bestRelOffset;
    }
    return true;
}

namespace {

MountEntry* kFindMountSlot(const char* path, uint32_t pathLen) {
    if (!path || pathLen == 0 || pathLen > kMaxMountPathLen) {
        return nullptr;
    }

    MountEntry* freeSlot = nullptr;
    for (uint32_t i = 0; i < kMaxMountEntries; ++i) {
        MountEntry& entry = gEntries[i];
        if (!entry.used) {
            if (!freeSlot) {
                freeSlot = &entry;
            }
            continue;
        }
        if (entry.pathLen != pathLen) {
            continue;
        }
        bool matches = true;
        for (uint32_t j = 0; j < pathLen; ++j) {
            if (entry.path[j] != path[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return nullptr;  // 이미 그 정확한 경로가 마운트돼 있음
        }
    }
    return freeSlot;  // 없으면 nullptr(테이블 가득 참)
}

}  // namespace

bool MountTable::mount(const char* path, uint32_t pathLen, uint64_t channelId) {
    MountEntry* slot = kFindMountSlot(path, pathLen);
    if (!slot) {
        return false;
    }
    *slot = MountEntry{};
    for (uint32_t i = 0; i < pathLen; ++i) {
        slot->path[i] = path[i];
    }
    slot->pathLen = pathLen;
    slot->kind = MountKind::Channel;
    slot->ownerChannelId = channelId;
    slot->used = true;
    return true;
}

bool MountTable::mountKernel(const char* path, uint32_t pathLen, KernelFsDriver* driver) {
    if (!driver) {
        return false;
    }
    MountEntry* slot = kFindMountSlot(path, pathLen);
    if (!slot) {
        return false;
    }
    *slot = MountEntry{};
    for (uint32_t i = 0; i < pathLen; ++i) {
        slot->path[i] = path[i];
    }
    slot->pathLen = pathLen;
    slot->kind = MountKind::KernelDriver;
    slot->kernelDriver = driver;
    slot->used = true;
    // [신규, PN-BC04D3DC] driver가 이미(다른 마운트 경로로) 등록된
    // 적 있으면 재등록하지 않는다 - AsyncCallbackRegistry::
    // registerHandler는 매 호출마다 새 subjectCode를 발급하므로,
    // 같은 드라이버 인스턴스를 두 마운트 경로에 걸면(이론상 가능 -
    // 지금은 LiveFs 하나뿐이라 실사용 없음) 재호출 시 이전 코드가
    // 조용히 새는 것을 막는다.
    if (!driver->hasSubjectCode()) {
        driver->setSubjectCode(AsyncCallbackRegistry::registerHandler(driver));
    }
    return true;
}

bool MountTable::unmount(const char* path, uint32_t pathLen) {
    if (!path || pathLen == 0) {
        return false;
    }
    for (uint32_t i = 0; i < kMaxMountEntries; ++i) {
        MountEntry& entry = gEntries[i];
        if (!entry.used || entry.pathLen != pathLen) {
            continue;
        }
        bool matches = true;
        for (uint32_t j = 0; j < pathLen; ++j) {
            if (entry.path[j] != path[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            entry = MountEntry{};
            return true;
        }
    }
    return false;
}

}  // namespace kernel
