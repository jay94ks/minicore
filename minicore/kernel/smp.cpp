#include "smp.h"

#include "acpi.h"
#include "async_task.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "libkenv/mem.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "rcu.h"
#include "scheduler.h"
#include "serial.h"
#include "syscall_fastpath.h"
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
// 없어 idle 루프만 도는 수준이라 놀럽히 잡아도 충분하다.
constexpr kernel::uint32_t kApStackOrder = 3;

// Timer가 100Hz(10ms/틱)이므로 50틱 = 500ms - 실제 AP 기동은 보통
// 수 ms면 끝나서 놀럽한 여유를 둔 값이다.
constexpr kernel::uint64_t kApReadyTimeoutTicks = 50;

kernel::AtomicU32 gApStartedCount;

// [신규, 2026-09-17, PN-907C5289] 코어 인덱스별 "온라인" 플래그 -
// nmi.cpp의 gNmiReason[kAcpiMaxCpus]와 동일한 "코어별 슬롯" 관례.
// gApStartedCount(단순 카운트)만으로는 Nmi::stopAllOtherCores()가
// "그 순간 정확히 어느 인덱스들이 이미 기동을 마쳤는지"를 알 수
// 없다 - AP는 순차 기동이라 실제로는 항상 인덱스 오름차순으로
// 완료되지만, BSP 자신의 MADT 인덱스가 항상 0이라는 보장이 없어
// "카운트만큼의 낮은 인덱스가 곧 온라인"이라고 가정하는 대신 실제
// 인덱스별 상태를 직접 기록한다.
kernel::AtomicU32 gCoreOnline[kernel::kAcpiMaxCpus];

// ap_trampoline.S의 BSP-AP 핸드오프 스크래치 - Smp::startApCores가
// SIPI를 보내기 직전에 채운다(순차 기동이라 공유해도 안전하다).
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
// 전혀 하지 않는다 - BSP가 이미 살아있는 커널 상태를 그대로 공유해
// 쓴다. 이 함수는 절대 반환하지 않는다(ap_trampoline.S가 반환 시
// hlt 루프로 방어하긴 하지만).
extern "C" void kApMain(kernel::uint32_t apIndex) {
    kernel::Idt::reloadOnThisCore();
    // ap_trampoline.S가 boot.S의 예전 gdt64(TSS 디스크립터 없음)를
    // 그대로 쓴 채로 여기 도달하므로, BSP가 만들어 둔 새 GDT로 이
    // 코어의 GDTR을 갈아 끼워야 loadTssForThisCore가 쓸 TSS 디스크립터를
    // 찾을 수 있다(gdt.h 참고).
    kernel::Gdt::reloadOnThisCore();
    kernel::Lapic::init();
    // Acpi::cpuApicId()/Lapic::id()로 자기 코어 인덱스를 찾아야 해서
    // 반드시 Lapic::init() 이후에 호출해야 한다.
    kernel::Gdt::loadTssForThisCore();
    // PN-124C105B("syscall 명령 경로") - STAR/LSTAR/SFMASK/EFER.SCE/
    // KERNEL_GS_BASE 전부 코어별 MSR이라 각 AP도 자기 몫을 스스로
    // 설정해야 한다(BSP의 kMain()과 동일한 관례).
    kernel::SyscallFastPath::initForThisCore();
    // SP-0666DB3C §12.4-1(PN-25587A7D) - BSP의 kMain()과 동일한 자리.
    kernel::Scheduler::initCoreIndexForThisCore();

    kernel::Serial::write("minicore: AP started, index=");
    kernel::Serial::writeHex(apIndex);
    kernel::Serial::write(" apic_id=");
    kernel::Serial::writeHex(kernel::Lapic::id());
    kernel::Serial::write("\n");

    kernel::Scheduler::startTickOnThisCore();
    // AsyncReactor는 더 이상 코어당 초기화가 필요 없다(2026-09-16
    // 재구조, PN-FEAAF154 - 전역 IDT 등록 하나만 BSP의 kMain()에서
    // 한 번 하면 끝) - 이 AP가 예전에 여기서 부르던
    // AsyncReactor::initForThisCore()는 삭제됐다.

    gApStartedCount.fetchAdd(1);
    kernel::Smp::markThisCoreOnline();
    // [신규, 2026-09-17, PN-495C11B7] RCU quiescent state 추적 시작 -
    // BSP의 kMain()과 대칭되는 지점.
    kernel::Rcu::initOnThisCore();

    // 이 지점부터 이 AP도 자기 코어의 스케줄러 디스패치 루프에
    // 들어간다 - 절대 반환하지 않는다(BSP의 kMain과 동일한 패턴,
    // PL-2D3184BC 5/6단계). AP는 아직 인터럽트가 비활성 상태로 여기
    // 도달하므로(ap_trampoline.S가 sti를 하지 않음), runLoop 자신의
    // "sti; hlt" idle 경로가 이 코어의 첫 sti 지점이 된다 -
    // enterIdleLoop()이 이 스택 전환 과정에서 지금의(IF=0) RFLAGS를
    // 그대로 보존해 넘기므로 이 순서는 그대로 유지된다(PN-2008220B,
    // scheduler.cpp의 enterIdleLoop() 문서 주석 참고). runLoop()을
    // 직접 부르지 않는다 - enterIdleLoop()이 이 지금의 저지대 부팅
    // 스택에서 코어 전용 안전한 idle 스택으로 먼저 옮겨 앉는다.
    kernel::Scheduler::enterIdleLoop();
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
        // 값(10ms/200us)보다 놀럽해도 정확성엔 문제없다(그냥 더
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

void Smp::markThisCoreOnline() {
    const uint32_t index = Scheduler::currentCoreIndex();
    if (index < kAcpiMaxCpus) {
        gCoreOnline[index].store(1);
    }
}

bool Smp::isCoreOnline(uint32_t coreIndex) {
    return coreIndex < kAcpiMaxCpus && gCoreOnline[coreIndex].load() != 0;
}

}  // namespace kernel
