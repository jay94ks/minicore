#include "tlb_shootdown.h"

#include "acpi.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "paging.h"

namespace {

// isr.S 동적 벡터(33-254) 대역 - kTimerVector(0x20, timer.h)/
// kHpetVector(0x22, hpet.h)/kLegacyPitVector(0x23, timer.cpp)/
// kSchedulerTickVector(0x24, scheduler.h) 다음 자리(SP-DE19BB1C §5-2 -
// 정확한 번호는 구현 시점에 확정해도 되는 것으로 명시됨).
constexpr kernel::uint32_t kTlbShootdownVector = 0x25;

// IPI 자체는 데이터를 못 옮기므로, 실제 무효화 범위는 공유 메모리에
// 적어 두고 IPI는 "그 메모리를 확인하라"는 신호로만 쓴다
// (SP-DE19BB1C §2.1). `KernelAddressSpaceManager`의 전역 락(아직
// 미구현)이 커널 영역 매핑 변경 자체를 이미 직렬화할 것이므로 요청
// 슬롯은 하나면 충분하다 - 동시에 두 개의 커널 영역 매핑 변경이
// 진행 중일 수 없다는 전제.
struct TlbShootdownRequest {
    kernel::uint64_t virtStart = 0;
    kernel::uint64_t virtEnd = 0;
    kernel::uint64_t targetPml4Phys = 0;  // 0 = 커널 영역(v1 전용) - §5-1 유저 영역 확장 전까지 항상 0
    kernel::AtomicU32 pendingAckCount;
};

TlbShootdownRequest gRequest;

void kInvalidateRange(kernel::uint64_t virtStart, kernel::uint64_t virtEnd) {
    for (kernel::uint64_t addr = virtStart; addr < virtEnd; addr += 4096) {
        asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
    }
}

// EOI는 이 함수가 직접 보내지 않는다 - HPET/PIT 핸들러와 같은 관례로,
// 반환 후 kIsrHandler(idt.cpp)가 대신 보낸다.
void kTlbShootdownHandler(kernel::InterruptFrame*) {
    const bool shouldInvalidate = (gRequest.targetPml4Phys == 0) ||
                                   (gRequest.targetPml4Phys == kernel::Paging::currentPml4Phys());
    if (shouldInvalidate) {
        kInvalidateRange(gRequest.virtStart, gRequest.virtEnd);
    }
    // "그냥 바로 ACK"도 이 한 줄로 통일 - invalidate 여부와 무관하게 항상 감소.
    gRequest.pendingAckCount.fetchSub(1);
}

}  // namespace

namespace kernel {

void TlbShootdown::init() {
    Idt::registerHandler(kTlbShootdownVector, kTlbShootdownHandler);
}

void TlbShootdown::broadcast(uint64_t virtStart, uint64_t virtEnd) {
    gRequest.virtStart = virtStart;
    gRequest.virtEnd = virtEnd;
    gRequest.targetPml4Phys = 0;  // v1 - 커널 영역 전용(§5-1)

    const uint32_t selfApicId = Lapic::id();
    uint32_t otherCount = 0;
    for (uint32_t i = 0; i < Acpi::cpuCount(); ++i) {
        if (Acpi::cpuApicId(i) != selfApicId) {
            ++otherCount;
        }
    }
    gRequest.pendingAckCount.store(otherCount);

    // 이 코어 자신은 IPI 없이 바로 invalidate(§2.3).
    kInvalidateRange(virtStart, virtEnd);

    for (uint32_t i = 0; i < Acpi::cpuCount(); ++i) {
        const uint32_t destApicId = Acpi::cpuApicId(i);
        if (destApicId == selfApicId) {
            continue;
        }
        Lapic::sendFixedIpi(destApicId, static_cast<uint8_t>(kTlbShootdownVector));
    }

    // busy-wait - 부팅 초기(AP가 아직 하나도 안 뜬 시점)엔
    // Acpi::cpuCount()==1이라 otherCount/pendingAckCount가 0에서
    // 시작해 이 루프가 즉시 끝난다(SP-DE19BB1C §3).
    while (gRequest.pendingAckCount.load() != 0) {
        asm volatile("pause");
    }
}

}  // namespace kernel
