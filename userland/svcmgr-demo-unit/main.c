// userland/svcmgr-demo-unit/main.c — svcmgr의 자기테스트가 스폰할
// **하드코딩된 데모 유닛 하나**(user-service-manager.md §M40~M42,
// docs/design/boot-and-drivers.md ADR-196 §결정2/3/7). 실제 유닛
// 레지스트리(@global/system/services, ADR-197)는 M41이 읽지만,
// exec_path별로 다른 실행 이미지를 로드하는 것은 여전히 범위 밖이라
// (M41 done 참고) 등록된 유닛이 몇 개든 전부 이 하나의 바이너리를
// 실행한다.
//
// libk+libmc만 링크한 순수 minicore 네이티브 실행파일이다(musl
// 불필요, ADR-006/007/132의 "2a" 계층) — ADR-193의 준비완료 신호
// (mc_signal_ready())를 보낸 뒤, M42가 op_stop/op_start로 "정지된
// 서비스를 다시 살릴 수 있다"를 검증하려면 이 프로세스가 준비완료
// 이후에도 계속 살아있어야 한다 — 그래서 M40/M41 시절의 "신호만
// 보내고 곧바로 끝남"에서 무한 대기 루프로 바꿨다(sys_yield만
// 반복 — 실제로 하는 일은 없다, 이 유닛의 존재 이유는 여전히
// "svcmgr가 이 유닛의 생명주기를 얼마나 잘 다루는가"를 확인하는
// 것뿐이다). sys_process_kill(ADR-178, op_stop이 그대로 재사용)로
// 강제 종료되는 것이 유일한 정상 종료 경로다.
#include <mc/lifecycle_client.h>
#include <mc/syscall.h>

void mc_main(const void* argv_or_null) {
    (void)argv_or_null;
    mc_signal_ready();
    for (;;) {
        mc_yield();
    }
}
