// userland/svcmgr-demo-unit/main.c — svcmgr의 M40 자기테스트가 스폰할
// **하드코딩된 데모 유닛 하나**(user-service-manager.md §M40,
// docs/design/boot-and-drivers.md ADR-196 §결정2/3). 실제 유닛
// 레지스트리(@global/system/services, ADR-197)는 M41 대상이라 이
// 라운드는 아직 읽지 않는다 — svcmgr가 이 바이트를 자신의 컴파일
// 시점 데이터로 직접 심어(tools/bin2c.py, servers/procsrv/CMakeLists.txt
// 의 musl-exec-target 패턴과 동일) VFS 없이 곧바로 process_spawn한다.
//
// libk+libmc만 링크한 순수 minicore 네이티브 실행파일이다(musl
// 불필요, ADR-006/007/132의 "2a" 계층) — ADR-193의 준비완료 신호
// (mc_signal_ready())만 보내고 끝난다. 실제 유닛이 하는 일(어떤
// 서비스를 실행하는가)은 이 마일스톤의 관심사가 아니다 — "스폰한
// 쪽이 spawn 시점 전용 endpoint로 준비완료를 기다렸다가 받으면
// 다음으로 진행한다"는 패턴 자체가 실제로 동작하는지만 증명한다.
#include <mc/lifecycle_client.h>

void mc_main(const void* argv_or_null) {
    (void)argv_or_null;
    mc_signal_ready();
}
