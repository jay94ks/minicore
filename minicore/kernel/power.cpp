#include "power.h"

#include "acpi.h"
#include "libkenv/mem.h"
#include "libkenv/types.h"
#include "mount_table.h"
#include "paging.h"
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

}  // namespace kernel
