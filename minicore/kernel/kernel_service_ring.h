#ifndef MINICORE_KERNEL_KERNEL_SERVICE_RING_H
#define MINICORE_KERNEL_KERNEL_SERVICE_RING_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"

namespace kernel {

// Tier A(SP-00CA7175 §2.1) - 커널 서비스별 전용 공유메모리 링버퍼.
// syscall submit/wait 왕복 자체를 생략하는 대용량 트래픽 전용 채널 -
// 실제 소비자가 아직 없어도 설계자 지시로 자리만 먼저 만들어 둔다.
// capacity/방향(단방향 vs 양방향)은 실사용 소비자가 정해질 때 확정할
// 구현 세부로 남긴다(RM-23F4B687 §4) - v1은 한 페이지(4KiB) 고정.
constexpr uint64_t kKernelServiceRingBufferCapacity = 4096;

struct KernelServiceSharedRingBuffer {
    AtomicU32 writePos;
    AtomicU32 readPos;
    uint32_t capacity = 0;
    uint8_t data[kKernelServiceRingBufferCapacity];

    // 이 버퍼도 raw slab 메모리 위에 reinterpret_cast로 앉혀지므로
    // (KernelReservedTable::reserveForKernelService만 호출) 기본
    // 멤버 초기화식이 실행되지 않는다 - 명시적으로 리셋한다.
    void reset() {
        writePos.store(0);
        readPos.store(0);
        capacity = kKernelServiceRingBufferCapacity;
    }
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_KERNEL_SERVICE_RING_H
