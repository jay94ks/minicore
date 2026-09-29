#include "socket_bind_table.h"

#include "libkenv/mem.h"
#include "libkenv/spinlock.h"

namespace {

struct SocketBindSlot {
    bool used = false;
    kernel::uint64_t nameLength = 0;
    char name[kernel::kMaxSocketBindNameLength] = {};
    kernel::uint64_t channelId = 0;
};

SocketBindSlot gSlots[kernel::kMaxSocketBindings];
// [named_object.cpp gLock과 동일한 판단] 임계구역이 kMaxSocketBindings
// (128)개 고정 슬롯의 선형 스캔뿐이라 짧고 유계.
kernel::IrqSpinlock gLock;

bool kNameEquals(const char* name, kernel::uint64_t nameLength, const SocketBindSlot& slot) {
    return slot.nameLength == nameLength && memcmp(slot.name, name, nameLength) == 0;
}

}  // namespace

namespace kernel {

bool SocketBindTable::reserve(const char* name, uint64_t nameLength, uint64_t channelId) {
    if (nameLength == 0 || nameLength > kMaxSocketBindNameLength) {
        return false;
    }
    IrqSpinlockGuard guard(gLock);
    int freeSlot = -1;
    for (uint32_t i = 0; i < kMaxSocketBindings; ++i) {
        if (!gSlots[i].used) {
            if (freeSlot < 0) {
                freeSlot = static_cast<int>(i);
            }
            continue;
        }
        if (kNameEquals(name, nameLength, gSlots[i])) {
            return false;  // 이미 쓰이는 이름
        }
    }
    if (freeSlot < 0) {
        return false;  // 테이블 가득 참
    }
    SocketBindSlot& slot = gSlots[freeSlot];
    slot.used = true;
    slot.nameLength = nameLength;
    memcpy(slot.name, name, nameLength);
    slot.channelId = channelId;
    return true;
}

bool SocketBindTable::resolve(const char* name, uint64_t nameLength, uint64_t* outChannelId) {
    if (nameLength == 0 || nameLength > kMaxSocketBindNameLength) {
        return false;
    }
    IrqSpinlockGuard guard(gLock);
    for (uint32_t i = 0; i < kMaxSocketBindings; ++i) {
        if (gSlots[i].used && kNameEquals(name, nameLength, gSlots[i])) {
            *outChannelId = gSlots[i].channelId;
            return true;
        }
    }
    return false;
}

void SocketBindTable::release(const char* name, uint64_t nameLength) {
    if (nameLength == 0 || nameLength > kMaxSocketBindNameLength) {
        return;
    }
    IrqSpinlockGuard guard(gLock);
    for (uint32_t i = 0; i < kMaxSocketBindings; ++i) {
        if (gSlots[i].used && kNameEquals(name, nameLength, gSlots[i])) {
            gSlots[i].used = false;
            return;
        }
    }
}

bool SocketBindTable::getByIndex(uint32_t index, char* outName, uint32_t* outNameLength) {
    IrqSpinlockGuard guard(gLock);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < kMaxSocketBindings; ++i) {
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
