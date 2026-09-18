#include "named_object.h"

#include "libkenv/mem.h"
#include "libkenv/spinlock.h"

namespace {

struct NamedObjectSlot {
    bool used = false;
    kernel::uint64_t nameLength = 0;
    char name[kernel::kMaxNamedObjectNameLength] = {};
    kernel::NamedObjectKind kind{};
    kernel::uint64_t objectId = 0;
};

NamedObjectSlot gSlots[kernel::kMaxNamedObjects];
kernel::Spinlock gLock;

bool kNameEquals(const char* name, kernel::uint64_t nameLength, const NamedObjectSlot& slot) {
    return slot.nameLength == nameLength && memcmp(slot.name, name, nameLength) == 0;
}

}  // namespace

namespace kernel {

bool NamedObjectTable::reserve(const char* name, uint64_t nameLength, NamedObjectKind kind, uint64_t objectId) {
    if (nameLength == 0 || nameLength > kMaxNamedObjectNameLength) {
        return false;
    }
    SpinlockGuard guard(gLock);
    int freeSlot = -1;
    for (uint32_t i = 0; i < kMaxNamedObjects; ++i) {
        if (!gSlots[i].used) {
            if (freeSlot < 0) {
                freeSlot = static_cast<int>(i);
            }
            continue;
        }
        if (kNameEquals(name, nameLength, gSlots[i])) {
            return false;  // 이미 쓰이는 이름 - 종류 불문 실패(보안 정책)
        }
    }
    if (freeSlot < 0) {
        return false;  // 테이블 가득 참
    }
    NamedObjectSlot& slot = gSlots[freeSlot];
    slot.used = true;
    slot.nameLength = nameLength;
    memcpy(slot.name, name, nameLength);
    slot.kind = kind;
    slot.objectId = objectId;
    return true;
}

bool NamedObjectTable::resolve(const char* name, uint64_t nameLength, NamedObjectKind* outKind, uint64_t* outObjectId) {
    if (nameLength == 0 || nameLength > kMaxNamedObjectNameLength) {
        return false;
    }
    SpinlockGuard guard(gLock);
    for (uint32_t i = 0; i < kMaxNamedObjects; ++i) {
        if (gSlots[i].used && kNameEquals(name, nameLength, gSlots[i])) {
            *outKind = gSlots[i].kind;
            *outObjectId = gSlots[i].objectId;
            return true;
        }
    }
    return false;
}

void NamedObjectTable::release(const char* name, uint64_t nameLength) {
    if (nameLength == 0 || nameLength > kMaxNamedObjectNameLength) {
        return;
    }
    SpinlockGuard guard(gLock);
    for (uint32_t i = 0; i < kMaxNamedObjects; ++i) {
        if (gSlots[i].used && kNameEquals(name, nameLength, gSlots[i])) {
            gSlots[i].used = false;
            return;
        }
    }
}

bool NamedObjectTable::getByIndex(uint32_t index, char* outName, uint32_t* outNameLength) {
    SpinlockGuard guard(gLock);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < kMaxNamedObjects; ++i) {
        if (!gSlots[i].used) {
            continue;
        }
        if (seen == index) {
            memcpy(outName, gSlots[i].name, gSlots[i].nameLength);
            *outNameLength = static_cast<uint32_t>(gSlots[i].nameLength);
            return true;
        }
        ++seen;
    }
    return false;
}

}  // namespace kernel
