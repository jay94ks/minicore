#ifndef MINICORE_KERNEL_POWER_H
#define MINICORE_KERNEL_POWER_H

#include "channel.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

// SP-0C7A4F3B - 커널 Shutdown/Reboot 서브시스템(DC-F367AD5D/QU-7C3AB7A2
// 답변 - "새 syscall + ACPI 전원 이벤트 둘 다 구현"). `Acpi`(acpi.h)가
// 이미 파싱해 둔 FADT PM1/Reset Register/DSDT 위치를 바탕으로 실제
// 하드웨어 레지스터를 조작한다 - `Acpi` 자신은 순수 파싱/노출만 하고
// 이 클래스가 유일하게 부작용(전원 끄기/리셋)을 일으킨다.
class Power {
public:
    // `Acpi::init()` 이후 BSP에서 한 번 호출 - DSDT를 스캔해 `\_S5`
    // 패키지의 SLP_TYPa/SLP_TYPb를 찾아 캐싱한다(SP-0C7A4F3B §3). 이
    // 프로젝트엔 범용 AML 인터프리터가 없으므로 못 찾을 수 있다 -
    // 그 경우 hasS5()가 false로 남고 shutdown()은 항상 실패를 반환한다
    // (하드웨어를 추측으로 조작하지 않는다).
    static void init();

    static bool hasS5();

    // ACPI PM1 제어 레지스터에 SLP_TYPa|SLP_EN(bit13)을 써서 전원을
    // 끈다(ACPI 스펙 §16.1.1) - 성공하면 이 함수는 돌아오지 않는다
    // (QEMU는 이 시점에 프로세스 자체가 종료됨). 실패 시에만 반환:
    // `NotFound`(FADT/S5 정보 없음), `NotSupported`(PM1 블록 자체가 0).
    static ChannelError shutdown();

    // ACPI Reset Register(지원 시) 또는 8042 키보드 컨트롤러(포트
    // 0x64에 0xFE)로 재시작한다 - 이 프로젝트가 실제로 검증한 QEMU
    // 기본 머신(`pc`/i440fx, SeaBIOS)은 Reset Register가 없어(실측
    // 확인, SP-0C7A4F3B §2) 8042 경로가 실제 사용 경로다. 두 방법
    // 다 트리거만 하고 실제 리셋이 일어날 때까지 잠깐의 지연이 있을
    // 수 있어 호출 직후 무한 hlt 루프로 대기한다 - 정상 동작이면 이
    // 함수도 결국 돌아오지 않는다.
    static void reboot();
};

// [신규] Shutdown/Reboot syscall(그룹 10 "Power", RM-48E1E610) - 인자
// 없음, 성공하면 애초에 호출자에게 응답이 돌아갈 일이 없다(프로세스를
//포함해 시스템 전체가 꺼지거나 재시작되므로).
struct ShutdownArgs {
    ChannelError error = ChannelError::None;
};
struct RebootArgs {
    ChannelError error = ChannelError::None;
};

constexpr SyscallEndpointId kSyscallEndpointShutdown = kMakeSyscallEndpointId(10, 0);
constexpr SyscallEndpointId kSyscallEndpointReboot = kMakeSyscallEndpointId(10, 1);

class PowerService {
public:
    // 부팅 시 한 번 호출 - Shutdown/Reboot endpoint를 등록한다.
    static void registerSyscallEndpoints();
};

// [신규, 2026-09-23, SP-0C7A4F3B §1 항목5] ACPI 전원 버튼(SCI) - 이
// 벡터 하나로 시스템 전체에 정확히 하나뿐인 SCI를 받는다(RM-28225668
// "커널 내부 IPI/최적화 벡터" 범위를 그대로 재사용 - LAPIC IPI가
// 아니라 IOAPIC이 라우팅하는 외부 인터럽트라는 점만 다르고, "이
// 벡터는 커널 자신만 처리한다"는 성격은 동일해 같은 범위에 등록).
constexpr uint32_t kAcpiSciVector = 0xE5;

// devmgr/fs와 동일한 패턴의 Process 없는 순수 커널 KernelThread
// entry(kmain.cpp가 hasFadt()+sciInterruptGsi()!=0+hasS5()일 때만
// 스폰) - IOAPIC 리다이렉션+PM1_EN.PWRBTN_EN 세팅까지 이 함수 안에서
// 직접 하고, 그 뒤 WaitInterrupt로 영원히 대기하다 PWRBTN_STS가
// 서면 클리어 후 `Power::shutdown()`을 부른다(power.cpp 참고).
void kPowerKernelMain(void* arg);

}  // namespace kernel

#endif  // MINICORE_KERNEL_POWER_H
