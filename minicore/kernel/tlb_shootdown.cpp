#include "tlb_shootdown.h"

#include "acpi.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "paging.h"
#include "smp.h"

namespace {

// PN-C7D62610(QU-C5B8A7D9 설계자 답변, 2026-09-16) - RM-28225668/
// SP-DE19BB1C §5-1이 확정한 커널 내부 IPI 전용 벡터 범위(0xE0~0xFD)로
// 재배선했다. 원래 PN-6D33BB03이 임시로 쓴 값은 0x25(하드웨어 IRQ와
// 안 겹치는 다음 빈 자리라는 이유만으로 고른 것 - kTimerVector(0x20)/
// kHpetVector(0x22)/kLegacyPitVector(0x23)/kSchedulerTickVector(0x24)
// 다음 자리)였으나, 이후 설계자가 "커널 내부 IPI는 전부 별도 현황판
// (RM-28225668)에서 관리하는 고정 범위에 모아 배정"하기로 확정하며
// 이 벡터도 그 범위 안(0xE0)으로 옮기라는 지시를 받았다.
constexpr kernel::uint32_t kTlbShootdownVector = 0xE0;

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

    // **버그 수정(PN-012E8C1A 검증 중 실측 발견, 2026-09-15)**: 예전
    // 주석은 "부팅 초기(AP가 아직 하나도 안 뜬 시점)엔 Acpi::cpuCount()
    // ==1이라 otherCount가 0에서 시작"이라고 가정했는데, 이 가정
    // 자체가 틀렸다 - `Acpi::cpuCount()`는 `Acpi::init()` 직후부터 이미
    // MADT가 보고하는 실제 하드웨어 코어 수를 그대로 반환하고,
    // `Smp::startApCores()`(smp.h - "인터럽트가 켜진(sti) 뒤에 호출해야
    // 한다")는 그보다 한참 뒤에야 실제로 AP를 깨운다. 그 사이(또는 AP
    // 기동이 일부 실패한 채로) 이 함수가 불리면, 아직 IDT/LAPIC조차 못
    // 띄운 AP에게 IPI를 보내 놓고 절대 오지 않을 ACK를 영원히 기다려
    // 그대로 멎어버린다(실측 - `KernelAddressSpaceManager::unmapRegion()`
    // 이라는 이 프로젝트 최초의 실제 소비자가, GRUB/SMP4 부팅 시퀀스
    // 중 `Smp::startApCores()` 호출보다 앞선 지점에서 이 함수를 부르자
    // 즉시 재현 - PVH 단일 코어에선 애초에 otherCount가 0이라 드러나지
    // 않았다). 아직 부팅되지 않은 코어는 그 어떤 매핑도 실행한 적이
    // 없어 TLB에 캐싱된 내용 자체가 없으므로 - 애초에 shootdown 대상이
    // 될 수 없다. `Smp::startedCount()`(실제로 `kApMain`까지 도달해
    // idle 루프에 들어간 AP 수)로 상한을 씌워, 아직 안 뜬 코어에는
    // IPI도 보내지 않고 그 몫의 ACK도 기다리지 않는다 - 부팅 완료
    // 이후(모든 AP가 정상 기동한 통상 경우)엔 `Smp::startedCount() ==
    // otherCount`라 동작이 전혀 안 바뀐다.
    //
    // **알려진 잔여 부정확성**(이번 수정 범위 밖 - 실제로 부딪힌 적
    // 없는 극단적 경우): `Smp::startApCores()`가 ACPI 목록 중간의
    // AP 하나를 기동 실패하고 그 뒤 AP는 성공하는 경우, `startedCount()`
    // 는 "성공한 개수"만 알 뿐 "정확히 어느 APIC ID들이 살아있는지"는
    // 모른다 - 아래는 ACPI 목록 순서상 앞쪽 항목들을 성공한 것으로
    // 가정하고 IPI를 보낸다. `Smp`가 아직 그런 정밀한 온라인 목록
    // 자체를 안 갖고 있어(smp.cpp의 `gApStartedCount`는 카운터일
    // 뿐인 배열/비트맵이 아님) 지금은 그 이상 정확히 알 방법이 없다 -
    // AP 기동 실패 자체가 지금까지 실측된 적 없는 경우라 새 DC 없이
    // 이 정도로 남겨 둔다(실제로 부딪히면 Smp에 온라인 APIC ID 목록을
    // 추가하는 후속 작업 필요).
    const uint32_t startedOtherCount = Smp::startedCount();
    if (startedOtherCount < otherCount) {
        otherCount = startedOtherCount;
    }
    gRequest.pendingAckCount.store(otherCount);

    // 이 코어 자신은 IPI 없이 바로 invalidate(§2.3).
    kInvalidateRange(virtStart, virtEnd);

    uint32_t sent = 0;
    for (uint32_t i = 0; i < Acpi::cpuCount() && sent < otherCount; ++i) {
        const uint32_t destApicId = Acpi::cpuApicId(i);
        if (destApicId == selfApicId) {
            continue;
        }
        Lapic::sendFixedIpi(destApicId, static_cast<uint8_t>(kTlbShootdownVector));
        ++sent;
    }

    // busy-wait - 부팅 초기(AP가 아직 하나도 안 뜬 시점)엔
    // otherCount/pendingAckCount가 0으로 캡핑돼 있어 이 루프가 즉시
    // 끝난다(위 수정 참고, SP-DE19BB1C §3).
    while (gRequest.pendingAckCount.load() != 0) {
        asm volatile("pause");
    }
}

}  // namespace kernel
