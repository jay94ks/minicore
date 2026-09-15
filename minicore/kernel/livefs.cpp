#include "livefs.h"

#include "channel.h"
#include "kernel_service_ring.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace {

kernel::KernelReservedEntry gEntries[kernel::kMaxKernelReservedEntries];

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

}  // namespace kernel
