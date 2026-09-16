#include "tlb_shootdown.h"

#include "acpi.h"
#include "idt.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "paging.h"
#include "scheduler.h"
#include "smp.h"
#include "task.h"

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

// [PN-D132A1E9/QU-DE2828A1, 2026-09-16] 요청자 코어 인덱스로 나뉜
// 슬롯 배열 - 코어 수만큼(kAcpiMaxCpus) 두면 락이 필요 없다. 하나의
// 물리 코어는 broadcast()를 동기적으로 호출하면 그 스레드 자신이
// pendingAckCount==0이 될 때까지 블로킹되므로, 한 번에 최대 하나의
// 요청만 낼 수 있다 - 즉 "요청자 코어 인덱스 = 슬롯 인덱스"가 항상
// 유일하게 정해져 자기 슬롯에 쓰는 데 동시성 보호가 필요 없다. IPI
// 자체는 데이터를 못 옮기므로, 실제 무효화 범위는 이 공유 메모리에
// 적어 두고 IPI는 "그 메모리를 확인하라"는 신호로만 쓴다.
struct TlbShootdownRequest {
    kernel::uint64_t virtStart = 0;
    kernel::uint64_t virtEnd = 0;
    kernel::uint64_t targetPml4Phys = 0;  // 0 = 커널 영역(전체 온라인 코어 대상)
    kernel::AtomicU32 pendingAckCount;
};

TlbShootdownRequest gRequests[kernel::kAcpiMaxCpus];

// [PN-D132A1E9/QU-DE2828A1] 수신자별 Target Pending Mask - 비트 i가
// 서 있으면 "요청자 슬롯 i(gRequests[i])가 이 코어를 향해 아직
// 처리하지 않은 요청을 갖고 있다"는 뜻이다. 요청자는 대상 코어들의
// 이 마스크에 자기 코어 인덱스 비트를 세우고 IPI를 보낸다 - 수신
// 코어의 ISR은 자기 몫의 마스크만 스캔/처리/해제하므로, 서로 다른
// 코어가 동시에(서로 다른 프로세스에 대해) broadcast()를 불러도 요청이
// 뒤섞이지 않는다. kAcpiMaxCpus(32)가 uint32_t 하나의 비트 폭과
// 정확히 맞아떨어져 비트마스크 하나로 전체 슬롯을 표현할 수 있다.
kernel::AtomicU32 gPendingMask[kernel::kAcpiMaxCpus];

void kInvalidateRange(kernel::uint64_t virtStart, kernel::uint64_t virtEnd) {
    for (kernel::uint64_t addr = virtStart; addr < virtEnd; addr += 4096) {
        asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
    }
}

// EOI는 이 함수가 직접 보내지 않는다 - HPET/PIT 핸들러와 같은 관례로,
// 반환 후 kIsrHandler(idt.cpp)가 대신 보낸다.
void kTlbShootdownHandler(kernel::InterruptFrame*) {
    const kernel::uint32_t myCore = kernel::Scheduler::currentCoreIndex();
    // 이 IPI가 도착한 시점에 나를 향해 대기 중인 요청 전부를 처리한다
    // - 그 사이 또(서로 다른 요청자로부터) 새로 비트가 세워졌으면
    // 다시 스캔해 마저 처리한다(무한정 머무르지 않는다 - 요청자 수가
    // 코어 수를 넘을 수 없어 유한).
    kernel::uint32_t mask = gPendingMask[myCore].load();
    while (mask != 0) {
        for (kernel::uint32_t i = 0; i < kernel::kAcpiMaxCpus; ++i) {
            const kernel::uint32_t bit = 1u << i;
            if (!(mask & bit)) {
                continue;
            }
            TlbShootdownRequest& req = gRequests[i];
            const bool shouldInvalidate = (req.targetPml4Phys == 0) ||
                                           (req.targetPml4Phys == kernel::Paging::currentPml4Phys());
            if (shouldInvalidate) {
                kInvalidateRange(req.virtStart, req.virtEnd);
            }
            gPendingMask[myCore].fetchAnd(~bit);
            // "그냥 바로 ACK"도 이 한 줄로 통일 - invalidate 여부와 무관하게 항상 감소.
            req.pendingAckCount.fetchSub(1);
        }
        mask = gPendingMask[myCore].load();
    }
}

}  // namespace

namespace kernel {

void TlbShootdown::init() {
    Idt::registerHandler(kTlbShootdownVector, kTlbShootdownHandler);
}

void TlbShootdown::broadcast(uint64_t virtStart, uint64_t virtEnd, uint64_t targetPml4Phys) {
    const uint32_t selfCore = Scheduler::currentCoreIndex();
    TlbShootdownRequest& req = gRequests[selfCore];
    req.virtStart = virtStart;
    req.virtEnd = virtEnd;
    req.targetPml4Phys = targetPml4Phys;

    uint32_t targetCoreIndices[kAcpiMaxCpus];
    uint32_t targetCount = 0;
    bool selfNeedsInvalidate = false;

    if (targetPml4Phys == 0) {
        // 커널 영역 - 모든 온라인 코어가 대상(상위 절반은 모든
        // PML4가 공유하므로 어느 코어가 지금 무엇을 실행 중이든 항상
        // 영향받는다). 호출한 코어 자신은 항상 무조건 무효화.
        selfNeedsInvalidate = true;

        // **버그 수정(PN-012E8C1A 검증 중 실측 발견, 2026-09-15)**:
        // `Acpi::cpuCount()`는 `Acpi::init()` 직후부터 이미 MADT가
        // 보고하는 실제 하드웨어 코어 수를 그대로 반환하지만,
        // `Smp::startApCores()`는 그보다 한참 뒤에야 실제로 AP를
        // 깨운다 - 그 사이(또는 AP 기동이 일부 실패한 채로) 이
        // 함수가 불리면, 아직 IDT/LAPIC조차 못 띄운 AP에게 IPI를
        // 보내 놓고 절대 오지 않을 ACK를 영원히 기다려 그대로
        // 멎어버린다. `Smp::startedCount()`(실제로 `kApMain`까지
        // 도달해 idle 루프에 들어간 AP 수)로 상한을 씌워, 아직 안 뜬
        // 코어에는 IPI도 보내지 않고 그 몫의 ACK도 기다리지 않는다.
        //
        // **알려진 잔여 부정확성**(실제로 부딪힌 적 없는 극단적
        // 경우): `Smp::startApCores()`가 ACPI 목록 중간의 AP 하나를
        // 기동 실패하고 그 뒤 AP는 성공하는 경우, `startedCount()`는
        // "성공한 개수"만 알 뿐 "정확히 어느 APIC ID들이 살아있는지"는
        // 모른다 - 아래는 ACPI 목록 순서상 앞쪽 항목들을 성공한
        // 것으로 가정한다(Smp가 아직 온라인 APIC ID 비트맵을 안 갖고
        // 있어 지금은 그 이상 정확히 알 방법이 없다).
        const uint32_t startedOtherCount = Smp::startedCount();
        for (uint32_t i = 0; i < Acpi::cpuCount() && targetCount < startedOtherCount; ++i) {
            if (i == selfCore) {
                continue;
            }
            targetCoreIndices[targetCount++] = i;
        }
    } else {
        // [PN-D132A1E9/QU-DE2828A1] 유저 영역 - 시스템 전체
        // 브로드캐스트가 아니라, 이 PML4를 지금 실제로 실행 중인
        // 코어들(Active CPU Mask)만 대상으로 한다.
        for (uint32_t i = 0; i < Acpi::cpuCount(); ++i) {
            if (i == selfCore) {
                continue;
            }
            const Task* t = Scheduler::taskOnCore(i);
            if (t && t->isUserLevel && t->userPml4Phys == targetPml4Phys) {
                targetCoreIndices[targetCount++] = i;
            }
        }
        // 발신 코어 자신은 커널 영역과 달리 무조건이 아니다 - 지금
        // 실제로 이 프로세스의 주소공간을 보고 있을 때만 무효화한다.
        selfNeedsInvalidate = (Paging::currentPml4Phys() == targetPml4Phys);
    }

    req.pendingAckCount.store(targetCount);

    if (selfNeedsInvalidate) {
        kInvalidateRange(virtStart, virtEnd);
    }

    for (uint32_t k = 0; k < targetCount; ++k) {
        const uint32_t coreIndex = targetCoreIndices[k];
        gPendingMask[coreIndex].fetchOr(1u << selfCore);
        Lapic::sendFixedIpi(Acpi::cpuApicId(coreIndex), static_cast<uint8_t>(kTlbShootdownVector));
    }

    // busy-wait - targetCount==0(예: 부팅 초기 AP 미기동, 또는 이
    // 프로세스를 지금 실행 중인 다른 코어가 하나도 없음)이면
    // pendingAckCount가 0에서 시작해 이 루프가 즉시 끝난다.
    while (req.pendingAckCount.load() != 0) {
        asm volatile("pause");
    }
}

}  // namespace kernel
