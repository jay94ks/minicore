// minicore/devmgr: SP-9DD4F3EA §6("devmgr 메인 서비스 시퀀스")의 첫
// 실코드(PN-BD9AAE2F 3번/4번 항목, PN-A0F72A3A가 §3.2 드라이버 매칭/
// 자식 스폰까지 이어붙였다). **[뒤집힘, 2026-09-20, 설계자 답변
// QU-1FB6A7A4 - "블록 디바이스는 그냥 아예 fs한테 던져버려. 인식/
// 인식 해제까지 전부."]** AHCI(SP-C2670F69)를 비롯한 블록 스토리지
// 장치의 인식/드라이버 구동은 devmgr이 아니라 fs(minicore/fs)가
// 직접 전담하는 것으로 최종 확정됐다 - devmgr이 fork()로 스폰한
// 이름 없는 드라이버 자식을 fs가 나중에 Channel로 찾아 연결해야
// 하는 문제(QU-1FB6A7A4가 처음 지적한 설계 공백) 자체가 이 결정으로
// 사라진다(같은 프로세스 안이라 애초에 핸드오프가 필요 없음). v1
// 매칭 테이블은 그 결과 빈 상태 - §3.4(핫플러그)를 포함해 devmgr이
// 실제로 fork() 스폰할 후속 드라이버(예: 비-스토리지 PnP 장치)가
// 생기면 그때 다시 채운다. `EnumerateDevices` 호출 자체는 devmgr의
// 일반 PnP 열거 책임(SP-9DD4F3EA §3.1)이라 그대로 남겨 둔다.
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
// argv={"devmgr","--driver=..."}로 재스폰)은 폐기됐다 - `mc::fork()`가
// 그 역할을 대신한다(현재는 실제 fork() 소비자가 없다 - 위 문서
// 주석 참고). 이 전역은 여전히 SysV 진입 규약 자체를 보관하는
// 용도로 남겨 둔다.
mc::int32_t gArgc = 0;
char** gArgv = nullptr;
char** gEnvp = nullptr;

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

    // [뒤집힘, 2026-09-20, QU-1FB6A7A4] 예전엔 여기서 AHCI(§3.2 클래스
    // 매칭)를 찾아 fork()로 드라이버 자식을 스폰했다 - 이제 블록
    // 스토리지 장치는 fs가 직접 인식/구동한다(위 파일 문서 주석
    // 참고). devmgr의 매칭 테이블은 현재 비어 있다 - 다음 비-스토리지
    // PnP 드라이버가 필요해지면 여기(gDevices/gDeviceCount 순회)에
    // 그 매칭 루프를 다시 채운다.

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
