// devmgr 커널 서비스: SP-9DD4F3EA §6("devmgr 메인 서비스 시퀀스")의 첫
// 실코드(PN-BD9AAE2F 3번/4번 항목, PN-A0F72A3A가 §3.2 드라이버 매칭/
// 자식 스폰까지 이어붙였다). **[뒤집힘, 2026-09-20, 설계자 답변
// QU-1FB6A7A4 - "블록 디바이스는 그냥 아예 fs한테 던져버려. 인식/
// 인식 해제까지 전부."]** AHCI(SP-C2670F69)를 비롯한 블록 스토리지
// 장치의 인식/드라이버 구동은 devmgr이 아니라 fs(kFsKernelMain,
// fs.cpp)가 직접 전담하는 것으로 최종 확정됐다 - devmgr이 fork()로 스폰한
// 이름 없는 드라이버 자식을 fs가 나중에 Channel로 찾아 연결해야
// 하는 문제(QU-1FB6A7A4가 처음 지적한 설계 공백) 자체가 이 결정으로
// 사라진다(같은 프로세스 안이라 애초에 핸드오프가 필요 없음). v1
// 매칭 테이블은 그 결과 빈 상태 - §3.4(핫플러그)를 포함해 devmgr이
// 실제로 fork() 스폰할 후속 드라이버(예: 비-스토리지 PnP 장치)가
// 생기면 그때 다시 채운다. `EnumerateDevices` 호출 자체는 devmgr의
// 일반 PnP 열거 책임(SP-9DD4F3EA §3.1)이라 그대로 남겨 둔다.
//
// **[전환, 2026-09-20, SP-43331889/QU-23B339AB/QU-ECEE5990 - devmgr을
// 유저랜드 프로세스에서 Process 없는 순수 커널 Task로 완전 흡수]**
// 이 파일은 더 이상 유저랜드 ELF(예전엔 `crt0.S`가 SysV 진입 스택을
// 받아 넘겨주던 `kDevmgrMain(argc,argv,envp)`였다)가 아니다 -
// `kmain.cpp`가 `kSpawnKernelThread(kDevmgrKernelMain, nullptr)`로
// 직접 띄우는 ring0 `KernelThread`의 entry 함수다. `libmc`(트랩 기반
// syscall 왕복)는 더 이상 쓰지 않고 `kernel::pnp.h`가 노출하는 동기
// 함수를 그대로 호출한다(같은 주소공간, 트랩 자체가 무의미 -
// SP-43331889 §3 "설계자 의견 - 커널 내부에선 syscall을 사용하지
// 말고 직접 호출하도록해"). argc/argv/envp 개념 자체가 없다(ELF
// 로드가 아니므로). **[정리, 2026-09-21, 설계자 지시, PN-D6A05E78]**
// 예전 유저랜드 진입점 `crt0.S`와 이 디렉터리 자체(`minicore/devmgr`)를
// 완전히 제거하고 이 파일을 `minicore/kernel/devmgr.cpp`로 옮겼다 -
// `minicore_kernel` 소스 목록(CMakeLists.txt)에 직접 들어간다.
#include "devmgr_service.h"
#include "pnp.h"

namespace kernel {

namespace {

// v1 상한 - 실측 후 조정(RM-23F4B687 §4, kernel/pnp.cpp의
// kMaxCachedPciDevices=256과는 별개로 devmgr 자신의 로컬 캐시 크기).
constexpr uint32_t kMaxDevices = 64;
DeviceDescriptor gDevices[kMaxDevices];
uint32_t gDeviceCount = 0;

}  // namespace

// [교체, 2026-09-20, SP-43331889 §7] `kSpawnKernelThread()`가 새
// `KernelThread`의 TaskTcb에 이 함수 포인터를 직접 실어 최초 진입
// 시 호출한다(Task::init() 재사용 - SP-43331889 §2 참고) - `arg`는
// 현재 안 씀(항상 nullptr로 스폰).
void kDevmgrKernelMain(void* /*arg*/) {
    uint32_t capacity = kMaxDevices;
    kEnumerateDevicesSync(0, &capacity, gDevices, &gDeviceCount);
    // capacity는 kEnumerateDevicesSync()이 "실제로 채운 개수"로
    // 덮어쓴다(gDeviceCount에 이미 그 값이 들어간다 - 트랩 시절의
    // "capacity를 in/out으로 겸용"하던 args 구조체가 없어져 두 값이
    // 이제 분리됐다).

    // [뒤집힘, 2026-09-20, QU-1FB6A7A4] 예전엔 여기서 AHCI(§3.2 클래스
    // 매칭)를 찾아 fork()로 드라이버 자식을 스폰했다 - 이제 블록
    // 스토리지 장치는 fs가 직접 인식/구동한다(위 파일 문서 주석
    // 참고). devmgr의 매칭 테이블은 현재 비어 있다 - 다음 비-스토리지
    // PnP 드라이버가 필요해지면 여기(gDevices/gDeviceCount 순회)에
    // 그 매칭 루프를 다시 채운다(§5의 `kSpawnUserModeDriver()`로).

    // [수정, 2026-09-20, SP-43331889 §7-1] devmgr은 이제 Process가
    // 없는 순수 KernelThread다 - 예전 essential=true Process가 죽으면
    // 커널이 자동으로 패닉하던 안전망(process.h의 `ProcessStartFlags::
    // essential` 문서 주석) 자체가 없어졌다. `kTaskOnFallingToEnd()`의
    // Kernel-Level 분기(scheduler.cpp)는 entry가 반환하면 정리 없이
    // 그냥 조용히 퇴역시킬 뿐이다 - 그러니 이 무한 대기가 예전보다
    // 오히려 더 중요해졌다(유일한 안전장치). §7 핫플러그 이벤트 대기
    // (syscall 기반 블로킹)가 아직 없는 지금은 그 자리를 대신하는
    // TEMP 자리표시자.
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace kernel
