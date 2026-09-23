#include "power.h"

#include "acpi.h"
#include "async_task.h"
#include "interrupt_subscription.h"
#include "ioapic.h"
#include "lapic.h"
#include "libkenv/mem.h"
#include "libkenv/types.h"
#include "logger.h"
#include "mount_table.h"
#include "paging.h"
#include "scheduler.h"
#include "syscall.h"
#include "task.h"
#include "x86_64/io_port.h"

namespace {

// [SP-0C7A4F3B §3] `\_S5` 패키지 안의 ComputationalData 하나를 읽는다
// - ACPI 스펙 §20.2.5. `ZeroOp`(0x00)/`OneOp`(0x01)/`OnesOp`(0xFF)는
// 그 자체가 값(각각 0/1/0xFFFFFFFF), `BytePrefix`(0x0A)/
// `WordPrefix`(0x0B)/`DWordPrefix`(0x0C)는 그 뒤 1/2/4바이트가 값 -
// SLP_TYPa/SLP_TYPb는 항상 0-7 범위(3비트 필드, ACPI 스펙 §4.8.3.2)
// 라 실무에서는 ZeroOp/OneOp 또는 BytePrefix만 나타나지만 스펙이
// 정의한 다른 폭도 방어적으로 지원한다.
bool kAmlReadSmallInt(const kernel::uint8_t*& p, const kernel::uint8_t* end, kernel::uint32_t* outValue) {
    if (p >= end) {
        return false;
    }
    const kernel::uint8_t op = *p;
    if (op == 0x00) {
        *outValue = 0;
        ++p;
        return true;
    }
    if (op == 0x01) {
        *outValue = 1;
        ++p;
        return true;
    }
    if (op == 0xFF) {
        *outValue = 0xFFFFFFFFu;
        ++p;
        return true;
    }
    if (op == 0x0A) {
        if (p + 1 >= end) return false;
        *outValue = p[1];
        p += 2;
        return true;
    }
    if (op == 0x0B) {
        if (p + 2 >= end) return false;
        *outValue = static_cast<kernel::uint32_t>(p[1]) | (static_cast<kernel::uint32_t>(p[2]) << 8);
        p += 3;
        return true;
    }
    if (op == 0x0C) {
        if (p + 4 >= end) return false;
        *outValue = static_cast<kernel::uint32_t>(p[1]) | (static_cast<kernel::uint32_t>(p[2]) << 8) |
                    (static_cast<kernel::uint32_t>(p[3]) << 16) | (static_cast<kernel::uint32_t>(p[4]) << 24);
        p += 5;
        return true;
    }
    return false;
}

// DSDT 바이트 전체에서 `\_S5` NameString(ASCII "_S5_" 4바이트 -
// AML NameString은 항상 4글자 고정 폭, ACPI 스펙 §20.2.2)을 찾아
// 그 뒤 PackageOp(0x12)+PkgLength(§20.2.4)+NumElements를 건너뛴 뒤
// 첫 두 ComputationalData(SLP_TYPa/SLP_TYPb)를 읽는다. 일반 AML
// 인터프리터가 아니므로(SP-0C7A4F3B §1) `Scope`/`If` 등으로 감싸여
// 있거나 별칭으로만 존재하는 DSDT는 못 찾을 수 있다 - 그때는
// false(호출부는 "S5 정보 없음"으로 안전하게 취급).
bool kFindS5SleepType(const kernel::uint8_t* dsdt, kernel::uint32_t len, kernel::uint32_t* outTypA,
                       kernel::uint32_t* outTypB) {
    if (len < 4) {
        return false;
    }
    const kernel::uint8_t* end = dsdt + len;
    for (kernel::uint32_t i = 0; i + 4 <= len; ++i) {
        if (dsdt[i] == '_' && dsdt[i + 1] == 'S' && dsdt[i + 2] == '5' && dsdt[i + 3] == '_') {
            const kernel::uint8_t* p = dsdt + i + 4;
            if (p >= end || *p != 0x12) {
                continue;  // PackageOp가 아님 - 우연히 같은 4바이트가 나온 오검출
            }
            ++p;  // PackageOp
            if (p >= end) continue;
            const kernel::uint8_t lead = *p;
            const kernel::uint32_t numExtra = (lead >> 6) & 0x3;
            if (p + 1 + numExtra >= end) continue;
            p += 1 + numExtra;  // PkgLength 인코딩 전체를 건너뜀(실제 길이값 자체는 안 씀)
            if (p >= end) continue;
            ++p;  // NumElements
            kernel::uint32_t typA = 0;
            kernel::uint32_t typB = 0;
            const kernel::uint8_t* cursor = p;
            if (!kAmlReadSmallInt(cursor, end, &typA)) continue;
            if (!kAmlReadSmallInt(cursor, end, &typB)) continue;
            *outTypA = typA;
            *outTypB = typB;
            return true;
        }
    }
    return false;
}

bool gHasS5 = false;
kernel::uint32_t gSlpTypA = 0;
kernel::uint32_t gSlpTypB = 0;

constexpr kernel::uint16_t kPm1SlpEnBit = 1U << 13;
constexpr kernel::uint16_t kPm1SlpTypShift = 10;
// ACPI 스펙 §4.8.3.1(PM1 Status)/§4.8.3.2(PM1 Enable) 공통 - 전원
// 버튼 상태/활성화 비트, 두 레지스터에서 같은 비트 위치.
constexpr kernel::uint16_t kPm1PwrBtnBit = 1U << 8;

// GAS(Generic Address Structure) addressSpaceId 값(ACPI 스펙
// §5.2.3.2) - 이 프로젝트가 실제로 다루는 건 System I/O뿐이지만
// System Memory도 방어적으로 지원한다(hasResetRegister()가 있는
// 실제 하드웨어에서 어느 쪽이 나올지 이 프로젝트가 아직 실측 못함 -
// §2 참고, 이 머신은 Reset Register 자체가 없음).
constexpr kernel::uint8_t kGasAddressSpaceSystemMemory = 0;
constexpr kernel::uint8_t kGasAddressSpaceSystemIo = 1;

void kWriteResetRegister() {
    const kernel::uint64_t addr = kernel::Acpi::resetRegisterAddress();
    const kernel::uint8_t value = kernel::Acpi::resetRegisterValue();
    if (kernel::Acpi::resetRegisterAddressSpaceId() == kGasAddressSpaceSystemIo) {
        kernel::arch::kOutB(static_cast<kernel::uint16_t>(addr), value);
    } else if (kernel::Acpi::resetRegisterAddressSpaceId() == kGasAddressSpaceSystemMemory) {
        auto* mmio = reinterpret_cast<volatile kernel::uint8_t*>(kernel::kPhysToVirt(addr));
        *mmio = value;
    }
}

// 8042 키보드 컨트롤러 리셋 - Reset Register가 없는 머신(이 프로젝트가
// 실측한 QEMU `pc`/i440fx 기본 머신 포함, SP-0C7A4F3B §2)의 표준
// 폴백. 포트 0x64(커맨드 레지스터)에 0xFE를 쓰면 CPU RESET 라인이
// 토글된다 - PS/2 컨트롤러 자체의 오래된 관례로, 이 프로젝트가 새로
// 고안한 값이 아니다(사실상 모든 x86 PC/에뮬레이터가 지원).
void kReset8042() {
    // 커맨드 레지스터가 입력을 받을 준비가 됐는지(상태 레지스터
    // bit1=입력 버퍼 가득 참) 짧게 폴링 - 이 프로젝트에 8042 드라이버
    // 자체가 없어 최소한의 안전장치만 둔다(무한 대기 방지 상한).
    for (kernel::uint32_t i = 0; i < 0x10000; ++i) {
        if ((kernel::arch::kInB(0x64) & 0x02) == 0) {
            break;
        }
    }
    kernel::arch::kOutB(0x64, 0xFE);
}

// [신규, 2026-09-23, SP-0C7A4F3B §1 항목5] `ahci.cpp`의
// `kSubscribeAhciInterrupt()`와 완전히 동일한 패턴(SyscallRegistry::
// resolveSubjectCode로 이미 등록된 SubscribeInterrupt/WaitInterrupt
// 핸들러의 subjectCode를 재사용 + PreemptionGuard 아래 제출 +
// preemptive=false로 그 안에서 확인된 self-IPI race를 피함 +
// AsyncTaskWaitGroup::waitAll()로 완료까지 블로킹) - 이 파일 자신의
// 전용 KernelThread(kPowerKernelMain)가 유일한 호출자라 exclusive=false.
bool kSubscribePowerButtonInterrupt(const kernel::SharedPtr<kernel::Task>& self, kernel::uint32_t vector) {
    kernel::AsyncTaskSubjectCode subjectCode = 0;
    if (!kernel::SyscallRegistry::resolveSubjectCode(kernel::kSyscallEndpointSubscribeInterrupt, &subjectCode)) {
        return false;
    }
    kernel::SubscribeInterruptArgs args;
    args.vector = vector;
    args.exclusive = false;
    kernel::AsyncTask* task = nullptr;
    {
        kernel::PreemptionGuard guard;
        task = kernel::AsyncTask::submit(subjectCode, 0, &args, /*autoFree=*/false, /*preemptive=*/false);
        if (!task) {
            return false;
        }
        task->submitterTask = kernel::TaskOwnerRef::capture(kernel::WeakPtr<kernel::Task>(self));
    }
    kernel::AsyncTaskWaitGroup group;
    group.add(task);
    group.waitAll();
    return args.error == kernel::InterruptSubscriptionError::None;
}

// 위와 같은 패턴, `WaitInterrupt` 전용 - `subjectCode`는 루프 밖에서
// 한 번만 구해 재사용한다(매 반복 다시 조회할 이유가 없음).
bool kWaitPowerButtonInterrupt(const kernel::SharedPtr<kernel::Task>& self, kernel::AsyncTaskSubjectCode subjectCode,
                                 kernel::uint32_t vector) {
    kernel::WaitInterruptArgs args;
    args.vector = vector;
    kernel::AsyncTask* task = nullptr;
    {
        kernel::PreemptionGuard guard;
        task = kernel::AsyncTask::submit(subjectCode, 0, &args, /*autoFree=*/false, /*preemptive=*/false);
        if (!task) {
            return false;
        }
        task->submitterTask = kernel::TaskOwnerRef::capture(kernel::WeakPtr<kernel::Task>(self));
    }
    kernel::AsyncTaskWaitGroup group;
    group.add(task);
    group.waitAll();
    return args.error == kernel::InterruptSubscriptionError::None;
}

// PM1x_STS의 PWRBTN_STS 비트를 확인하고, 서 있으면 그 비트만 W1C로
// 클리어한 뒤 true를 반환한다(그 외 상태 비트는 절대 건드리지
// 않음 - 쓰기 값 자체에 PWRBTN 비트 하나만 세팅).
bool kCheckAndClearPowerButtonStatus(kernel::uint32_t eventBlock) {
    if (eventBlock == 0) {
        return false;
    }
    const kernel::uint16_t sts = kernel::arch::kInW(static_cast<kernel::uint16_t>(eventBlock));
    if ((sts & kPm1PwrBtnBit) == 0) {
        return false;
    }
    kernel::arch::kOutW(static_cast<kernel::uint16_t>(eventBlock), kPm1PwrBtnBit);
    return true;
}

}  // namespace

namespace kernel {

void Power::init() {
    if (!Acpi::hasFadt() || Acpi::dsdtPhysAddress() == 0 || Acpi::dsdtLength() == 0) {
        gHasS5 = false;
        return;
    }
    const auto* dsdt = reinterpret_cast<const uint8_t*>(kPhysToVirt(Acpi::dsdtPhysAddress()));
    gHasS5 = kFindS5SleepType(dsdt, Acpi::dsdtLength(), &gSlpTypA, &gSlpTypB);
}

bool Power::hasS5() { return gHasS5; }

ChannelError Power::shutdown() {
    if (!Acpi::hasFadt() || !gHasS5) {
        return ChannelError::NotFound;
    }
    if (Acpi::pm1aControlBlock() == 0) {
        return ChannelError::NotSupported;
    }

    MountTable::unmountAllForShutdown();

    // 이미 ACPI 모드가 아니면(SCI_EN이 꺼져 있으면) SMI_CMD에
    // ACPI_ENABLE을 써서 전환한다 - SMI_CMD==0이면 이 펌웨어는 애초에
    // SMI 기반 모드 전환 자체가 없는 것(이미 항상 ACPI 모드)이라
    // 건너뛴다(ACPI 스펙 §16.1.3).
    if (Acpi::smiCommandPort() != 0 && Acpi::acpiEnableValue() != 0) {
        arch::kOutB(static_cast<uint16_t>(Acpi::smiCommandPort()), Acpi::acpiEnableValue());
    }

    const uint16_t valueA = static_cast<uint16_t>((gSlpTypA << kPm1SlpTypShift) | kPm1SlpEnBit);
    arch::kOutW(static_cast<uint16_t>(Acpi::pm1aControlBlock()), valueA);
    if (Acpi::pm1bControlBlock() != 0) {
        const uint16_t valueB = static_cast<uint16_t>((gSlpTypB << kPm1SlpTypShift) | kPm1SlpEnBit);
        arch::kOutW(static_cast<uint16_t>(Acpi::pm1bControlBlock()), valueB);
    }

    // 성공하면 여기 도달하지 않는다(QEMU는 이 시점에 프로세스가
    // 종료됨, 실제 하드웨어는 전원이 꺼짐) - 혹시 안 꺼졌다면(에뮬레이터
    // 차이 등) 호출부에 에러로 알린다.
    return ChannelError::NotSupported;
}

void Power::reboot() {
    MountTable::unmountAllForShutdown();

    if (Acpi::hasResetRegister()) {
        kWriteResetRegister();
    }
    kReset8042();

    // 둘 다 트리거만 하고 실제 리셋까지 하드웨어 지연이 있을 수 있다 -
    // 정상 동작이면 리셋이 이 루프를 끊는다.
    while (true) {
        asm volatile("hlt");
    }
}

namespace {

class ShutdownHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<ShutdownArgs*>(argsRaw);
        args->error = Power::shutdown();
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class RebootHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void*) override {
        Power::reboot();
        co_return;  // 도달하지 않음(reboot()는 성공 시 무한 hlt 루프)
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ShutdownHandler gShutdownHandler;
RebootHandler gRebootHandler;

}  // namespace

void PowerService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointShutdown, &gShutdownHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointReboot, &gRebootHandler);
}

// [신규, 2026-09-23, SP-0C7A4F3B §1 항목5] devmgr/fs와 동일한 패턴의
// Process 없는 순수 커널 KernelThread entry - kmain.cpp가
// `Acpi::hasFadt()`+`Acpi::sciInterruptGsi()!=0`+`Power::hasS5()`일
// 때만 스폰한다(그 외엔 애초에 자동 종료를 시작할 방법이 없으니
// 스레드 하나를 낭비할 이유가 없음). IOAPIC 리다이렉션/PM1_EN 세팅을
// 이 함수 자신이 직접 하고(같은 이유로 devmgr/fs도 자기 초기화를
// 자기 entry 안에서 함), 그 뒤 영원히 WaitInterrupt로 대기하다
// PWRBTN_STS가 서면 클리어 후 `Power::shutdown()`을 부른다 - ISR
// 안에서 직접 마운트 순회/레지스터 조작을 하지 않는다(이 프로젝트의
// 기존 인터럽트 컨텍스트 규율과 동일 - `InterruptSubscription`이 ISR
// 쪽 처리와 이 Task 레벨 처리를 이미 완전히 분리해 준다).
void kPowerKernelMain(void* /*arg*/) {
    auto* self = static_cast<KernelThread*>(Scheduler::currentTask());
    SharedPtr<Task> selfShared = self->weakAsTask().lock();

    if (!InterruptDelegation::allow(kAcpiSciVector)) {
        for (;;) {
            asm volatile("pause");
        }
    }

    const Acpi::IsaIrqRouting route = Acpi::resolveIsaIrq(Acpi::sciInterruptGsi());
    if (!IoApic::setRedirection(route.gsi, kAcpiSciVector, Lapic::id(), route.polarity, route.triggerMode)) {
        for (;;) {
            asm volatile("pause");
        }
    }

    // PM1_EN은 PM1_EVT_BLK 뒤쪽 절반(길이 PM1_EVT_LEN/2)에 있다(ACPI
    // 스펙 §4.8.3) - 기존 값을 보존한 채(read-modify-write) PWRBTN_EN
    // 비트만 세팅해 다른 활성화된 이벤트(있다면)를 건드리지 않는다.
    const uint16_t pm1aEnAddr = static_cast<uint16_t>(Acpi::pm1aEventBlock() + Acpi::pm1EventBlockLength() / 2);
    arch::kOutW(pm1aEnAddr, static_cast<uint16_t>(arch::kInW(pm1aEnAddr) | kPm1PwrBtnBit));
    if (Acpi::pm1bEventBlock() != 0) {
        const uint16_t pm1bEnAddr = static_cast<uint16_t>(Acpi::pm1bEventBlock() + Acpi::pm1EventBlockLength() / 2);
        arch::kOutW(pm1bEnAddr, static_cast<uint16_t>(arch::kInW(pm1bEnAddr) | kPm1PwrBtnBit));
    }

    if (!kSubscribePowerButtonInterrupt(selfShared, kAcpiSciVector)) {
        for (;;) {
            asm volatile("pause");
        }
    }

    AsyncTaskSubjectCode waitSubjectCode = 0;
    if (!SyscallRegistry::resolveSubjectCode(kSyscallEndpointWaitInterrupt, &waitSubjectCode)) {
        for (;;) {
            asm volatile("pause");
        }
    }

    for (;;) {
        if (!kWaitPowerButtonInterrupt(selfShared, waitSubjectCode, kAcpiSciVector)) {
            break;  // 구독 자체가 깨짐(NotSubscribed 등) - 더 대기할 수 없음
        }
        // SCI는 여러 소스가 공유하는 레벨 트리거 신호라, 실제로
        // 전원 버튼이 눌렸는지는 PM1_STS를 직접 봐야 안다(다른
        // 이벤트로 SCI가 떴을 수도 있음 - 그 경우 아무 것도 안 하고
        // 다음 WaitInterrupt로 돌아간다).
        if (kCheckAndClearPowerButtonStatus(Acpi::pm1aEventBlock())) {
            Power::shutdown();  // 성공하면 안 돌아옴 - 실패 시에만 루프 계속
        }
        if (kCheckAndClearPowerButtonStatus(Acpi::pm1bEventBlock())) {
            Power::shutdown();
        }
    }

    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace kernel
