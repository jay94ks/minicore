#include "smp.h"

#include "acpi.h"
#include "idt.h"
#include "lapic.h"
#include "libkenv/mem.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "serial.h"
#include "timer.h"

namespace {

// ap_trampoline.S(16비트 진입점)의 실행 주소(VMA) - SIPI 벡터 =
// 물리주소/4096. QEMU의 PVH 로더가 이 물리주소를 위한 별도 PT_LOAD
// 세그먼트를 게스트 메모리에 안 올려서(2026-09-14 실측), 링커는 이
// 바이트를 :boot 세그먼트 안(이미 로드됨이 검증된 곳)에 저장해 두고,
// startApCores()가 AP를 깨우기 전에 진짜 이 주소로 직접 복사한다.
constexpr kernel::uint64_t kApTrampolineVaddr = 0x8000UL;
constexpr kernel::uint32_t kApTrampolineVector = 0x08;

// linker.ld가 정의 - 트램폴린 바이트가 실제로 로드된 위치(물리=가상,
// boot.S의 저지대 identity map 범위 안)와 그 크기.
extern "C" char ap_trampoline16_lma[];
extern "C" char ap_trampoline16_size[];

void kCopyApTrampolineToRuntimeAddress() {
    const auto size = reinterpret_cast<kernel::uint64_t>(ap_trampoline16_size);
    memcpy(reinterpret_cast<void*>(kApTrampolineVaddr), ap_trampoline16_lma, size);
}

// AP 하나당 스택 크기(4KiB << 3 = 32KiB) - 아직 스레드/프로세스가
// 없어 idle 루프만 도는 수준이라 넉넉히 잡아도 충분하다.
constexpr kernel::uint32_t kApStackOrder = 3;

// Timer가 100Hz(10ms/틱)이므로 50틱 = 500ms - 실제 AP 기동은 보통
// 수 ms면 끝나서 넉넉한 여유를 둔 값이다.
constexpr kernel::uint64_t kApReadyTimeoutTicks = 50;

kernel::AtomicU32 gApStartedCount;

// ap_trampoline.S의 BSP-AP 핸드오프 스크래치 - Smp::startApCores가
// SIPI를 보내기 직전에 채운다(순차 기동이라 공유해도 안전).
extern "C" kernel::uint64_t ap_boot_stack_top;
extern "C" kernel::uint32_t ap_boot_index;

void kBusyWaitOneTick() {
    const kernel::uint64_t start = kernel::Timer::tickCount();
    while (kernel::Timer::tickCount() == start) {
        asm volatile("pause");
    }
}

bool kWaitApReady(kernel::uint32_t expectedCount) {
    const kernel::uint64_t deadline = kernel::Timer::tickCount() + kApReadyTimeoutTicks;
    while (kernel::Timer::tickCount() < deadline) {
        if (gApStartedCount.load() >= expectedCount) {
            return true;
        }
        asm volatile("pause");
    }
    return gApStartedCount.load() >= expectedCount;
}

}  // namespace

// ap_trampoline.S의 ap_long_mode_entry가 호출한다 - edi = 이 AP의
// Acpi 코어 목록 인덱스(Smp::startApCores가 미리 기록해 둔 값).
// kMain과 달리 IDT 재빌드/.bss 초기화/전역 서브시스템 재초기화를
// 전혀 하지 않는다 - BSP가 이미 살아있는 커널 상태를 그대로 공유해서
// 쓴다. 이 함수는 절대 반환하지 않는다(ap_trampoline.S가 반환 시
// hlt 루프로 방어하긴 하지만).
extern "C" void kApMain(kernel::uint32_t apIndex) {
    kernel::Idt::reloadOnThisCore();
    kernel::Lapic::init();

    kernel::Serial::write("minicore: AP started, index=");
    kernel::Serial::writeHex(apIndex);
    kernel::Serial::write(" apic_id=");
    kernel::Serial::writeHex(kernel::Lapic::id());
    kernel::Serial::write("\n");

    gApStartedCount.fetchAdd(1);

    for (;;) {
        asm volatile("hlt");
    }
}

namespace kernel {

void Smp::startApCores() {
    kCopyApTrampolineToRuntimeAddress();

    const uint32_t bspApicId = Lapic::id();
    const uint32_t cpuCount = Acpi::cpuCount();
    uint32_t expectedStarted = 0;

    for (uint32_t i = 0; i < cpuCount; ++i) {
        const uint32_t apicId = Acpi::cpuApicId(i);
        if (apicId == bspApicId) {
            continue;  // 자기 자신(BSP)은 건너뜀
        }

        const uint64_t stackPhys = PageFrameAllocator::allocOrder(kApStackOrder);
        if (!stackPhys) {
            Serial::write("minicore: SMP - AP stack alloc failed, skip apic_id=");
            Serial::writeHex(apicId);
            Serial::write("\n");
            continue;
        }
        ap_boot_stack_top = kPhysToVirt(stackPhys) + (4096UL << kApStackOrder);
        ap_boot_index = i;

        // INIT-SIPI-SIPI(Intel MP 스펙 관례) - 어서트 -> 디어서트
        // 사이/SIPI 사이에 지연을 둔다. Timer 1틱(10ms)이 스펙 권장
        // 값(10ms/200us)보다 넉넉해도 정확성엔 문제없다(그냥 더
        // 기다리는 것뿐).
        Lapic::sendInitIpi(apicId, true);
        kBusyWaitOneTick();
        Lapic::sendInitIpi(apicId, false);
        kBusyWaitOneTick();

        Lapic::sendStartupIpi(apicId, kApTrampolineVector);
        kBusyWaitOneTick();

        ++expectedStarted;
        if (!kWaitApReady(expectedStarted)) {
            // 구식 CPU는 SIPI를 두 번 받아야 반응하는 경우가 있다 -
            // 한 번 더 보내고 마지막으로 한 번 더 기다린다.
            Lapic::sendStartupIpi(apicId, kApTrampolineVector);
            if (!kWaitApReady(expectedStarted)) {
                Serial::write("minicore: SMP - AP apic_id=");
                Serial::writeHex(apicId);
                Serial::write(" did not respond (timeout)\n");
            }
        }
    }

    Serial::write("minicore: SMP - APs started=");
    Serial::writeHex(gApStartedCount.load());
    Serial::write(" expected=");
    Serial::writeHex(expectedStarted);
    Serial::write("\n");
}

uint32_t Smp::startedCount() {
    return gApStartedCount.load();
}

}  // namespace kernel
