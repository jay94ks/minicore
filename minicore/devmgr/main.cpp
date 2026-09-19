// minicore/devmgr: SP-9DD4F3EA §6("devmgr 메인 서비스 시퀀스")의 첫
// 실코드(PN-BD9AAE2F 3번/4번 항목, PN-A0F72A3A가 §3.2 드라이버 매칭/
// 자식 스폰까지 이어붙였다). v1 매칭 테이블은 AHCI(클래스 매칭) 항목
// 하나뿐 - §3.4(핫플러그)는 아직 없다.
#include "libmc/channel.h"
#include "libmc/pnp.h"
#include "libmc/syscall.h"

namespace {

// v1 상한 - 실측 후 조정(RM-23F4B687 §4, kernel/pnp.cpp의
// kMaxCachedPciDevices=256과는 별개로 devmgr 자신의 로컬 캐시 크기).
constexpr mc::uint32_t kMaxDevices = 64;
mc::DeviceDescriptor gDevices[kMaxDevices];
mc::uint32_t gDeviceCount = 0;

// [신규, PN-A0F72A3A 착수 순서 3번] crt0.S가 실제 SysV 진입 스택에서
// 꺼내 넘겨준 값 - 최초 부팅 시 스폰(kSpawnServiceProcesses)은 항상
// argc=0/argv=[NULL]이다. **[정정, 2026-09-19, QU-FB7A0CFF 답변]**
// "드라이버 모드 재진입"을 이 값으로 구분하는 원안(자기 자신을
// argv={"devmgr","--driver=..."}로 재스폰)은 폐기됐다 - 이제
// `mc::fork()`(아래) 반환값이 그 역할을 대신한다. 이 전역은 여전히
// SysV 진입 규약 자체를 보관하는 용도로 남겨 둔다.
mc::int32_t gArgc = 0;
char** gArgv = nullptr;
char** gEnvp = nullptr;

// [신규, PN-A0F72A3A 착수 순서 3번(구현)] SP-9DD4F3EA §3.2/§4a-1
// 매칭 테이블 - v1은 클래스 매칭 항목 하나뿐(AHCI: classCode=1
// "Mass Storage"/subclass=6 "SATA"). progIf(AHCI 1.0=1)는 아직 구분
// 안 함 - 정확 일치(vendorId+deviceId) 항목도 아직 없다(후보가 이
// 하나뿐이라 §4a-1의 특이도 순 정렬/등록 순서 규칙은 실질적으로
// 적용될 기회가 없음, 후속 드라이버 추가 시 필요).
constexpr mc::uint32_t kPciClassMassStorage = 1;
constexpr mc::uint32_t kPciSubclassSata = 6;

bool kMatchesAhci(const mc::DeviceDescriptor& dev) { return dev.classCode == kPciClassMassStorage && dev.subclass == kPciSubclassSata; }

// [신규, PN-A0F72A3A 착수 순서 4번] `DeviceDescriptor::mmioBases[6]`은
// BAR 인덱스 그대로(mmioBases[0]=BAR0 ... mmioBases[5]=BAR5, 커널 쪽
// kFillMmioBases()/pnp.cpp 참고) - AHCI의 ABAR는 관례상 BAR5라
// mmioBases[0]이 아니라 mmioBases[5]에 들어 있는 경우가 흔하다(실측:
// QEMU ich9-ahci). 첫 번째 실제로 존재하는(0이 아닌) BAR를 찾아
// 쓴다 - "이 장치의 메모리 매핑 BAR 아무거나 하나"면 충분한 v1
// 스코프(§3.3 RequestIoPermissionArgs 문서 주석과 동일한 전제).
mc::uint64_t kFirstMmioBase(const mc::DeviceDescriptor& dev) {
    for (mc::uint64_t base : dev.mmioBases) {
        if (base != 0) {
            return base;
        }
    }
    return 0;
}

// [신규, PN-A0F72A3A 착수 순서 4번] `mc::fork()`로 분리된 드라이버
// 자식 프로세스 안에서 실행 - SP-9DD4F3EA §3.2 "자식 쪽" 단계
// (`RequestIoPermission`으로 BAR/IRQ 확보 -> 자체 Channel 개설)까지만
// 다룬다. 실제 AHCI HBA 레지스터 초기화/커맨드 리스트/DMA는
// `SP-C2670F69` §2-3 몫 - 이번 증분 범위 밖(PN-A0F72A3A 착수 순서
// 7번으로 분리 예정). fork() 자식은 devmgr과 별개 프로세스라
// essential이 기본 `false`(`Process::allocate()`의 zero-init 기본값,
// `kHandleForkSyscall`이 `startFlags`를 전혀 안 건드림) - 크래시해도
// devmgr/다른 드라이버를 끌고 내려가지 않는다(§3.2가 요구하는 장치
// 격리를 그대로 만족).
[[noreturn]] void kRunAhciDriverChild(const mc::DeviceDescriptor& dev) {
    mc::RequestIoPermissionArgs ioArgs;
    ioArgs.bus = dev.bus;
    ioArgs.device = dev.device;
    ioArgs.function = dev.function;
    ioArgs.mmioBase = kFirstMmioBase(dev);
    mc::SyscallToken ioToken = mc::submit(mc::kSyscallEndpointRequestIoPermission, &ioArgs);
    if (ioToken != 0) {
        mc::wait(ioToken);
    }

    if (ioArgs.error == mc::ChannelError::None) {
        // 이름 없이 개설(§6 6단계 정정 - SP-B071E628 §5-A/§5-B 확정대로
        // 커널 서비스/그 드라이버 자식은 pubreg를 아예 모른다).
        mc::OpenChannelArgs openArgs;
        mc::SyscallToken openToken = mc::submit(mc::kSyscallEndpointOpenChannel, &openArgs);
        if (openToken != 0) {
            mc::wait(openToken);
        }
    }

    // BAR 확보/Channel 개설 성공 여부와 무관하게 계속 살아있는다(§3.2 -
    // 드라이버 프로세스도 devmgr과 동일하게 절대 스스로 종료하지 않는다
    // 는 "살아있는 서비스" 계약을 따른다, essential 여부와는 별개 축).
    // 이후(HBA 초기화 등)는 SP-C2670F69 착수 세션이 이어받는다.
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace

// [교체, PN-A0F72A3A 착수 순서 3번] `_start()` 자신은 이제 crt0.S가
// 맡는다(SysV 스택 -> 호출 규약 레지스터 변환) - 이 함수가 그 변환된
// argc/argv/envp를 실제로 받는 진짜 진입점이다.
extern "C" void kDevmgrMain(mc::int32_t argc, char** argv, char** envp) {
    gArgc = argc;
    gArgv = argv;
    gEnvp = envp;

    mc::EnumerateDevicesArgs args;
    args.startIndex = 0;
    args.capacity = kMaxDevices;
    args.outDevices = gDevices;

    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointEnumerateDevices, &args);
    if (token != 0 && mc::wait(token) && args.error == mc::ChannelError::None) {
        gDeviceCount = args.capacity;  // capacity는 EnumerateDevices onExec()이 "실제로 채운 개수"로 덮어쓴다
    }

    // [교체, 2026-09-19, PN-A0F72A3A 착수 순서 4번] 예전엔 여기서 메모리
    // 매핑 BAR가 있는 장치마다 무조건 RequestIoPermission을 시험 삼아
    // 불렀다(실제 드라이버가 없던 시절의 syscall 왕복 검증용 TEMP 스텁,
    // PN-BD9AAE2F가 이미 검증 완료해 더 이상 필요 없음) - 이제 진짜
    // §3.2 매칭 루프로 대체한다: devmgr 자신은 더 이상 IO 권한을 직접
    // 쥐지 않는다(매칭된 드라이버 자식만 쥔다, QU-FB7A0CFF 답변 -
    // "devmgr 자체가 드라이버로 동작하는 것은 아니고, 내장형 드라이버만
    // 그렇게 하도록").
    for (mc::uint32_t i = 0; i < gDeviceCount; ++i) {
        if (!kMatchesAhci(gDevices[i]) || kFirstMmioBase(gDevices[i]) == 0) {
            continue;  // 매칭 실패 또는 이 드라이버가 기대하는 조건 미충족(§4a-1 "probe 실패") - 다음 장치로
        }
        mc::int64_t pid = mc::fork();
        if (pid == 0) {
            kRunAhciDriverChild(gDevices[i]);  // 반환하지 않음
        }
        // pid<0(fork 실패)이든 pid>0(부모, 자식 스폰 성공)이든 - 실패해도
        // 이 장치를 건너뛰고 계속 진행한다(RM-23F4B687 §4 - 장치 하나
        // 실패가 devmgr 전체를 막으면 안 됨, §4a-1 "모든 후보 실패 시
        // 로그만 남기고 건너뜀"과 동일한 원칙).
    }

    // [수정, 2026-09-18, PN-11B3D2BB] devmgr는 `kSpawnServiceProcesses()`
    // 가 `ProcessStartFlags::essential = true`로 스폰하는 KernelService다
    // (kmain.cpp) - `essential==true`인 프로세스가 실행을 마치면(크래시든
    // 정상 종료든 무관하게) 커널이 "죽었다"고 보고 즉시 패닉한다
    // (process.h의 `ProcessStartFlags::essential` 문서 주석, scheduler.cpp
    // "PANIC - essential service died" 분기). `fs`(minicore/fs/main.cpp)
    // 가 이미 하고 있는 것과 같은 관례로, 실제 서비스 루프(§7 핫플러그
    // 대기)가 아직 없는 지금은 그 자리를 대신할 최소한의 무한 대기로
    // 막아 둔다 - 절대 종료하지 않는다는 essential 계약만 만족시키는
    // TEMP 자리표시자, §7이 실제 핫플러그 이벤트 대기(syscall 기반
    // 블로킹)로 대체할 것.
    for (;;) {
        asm volatile("pause");
    }
}
