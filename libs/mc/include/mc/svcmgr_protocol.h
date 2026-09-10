// mc/svcmgr_protocol.h — 유저 서비스 유닛 모델(user-service-manager.md,
// docs/design/boot-and-drivers.md ADR-196 §결정2). 이 구조체가
// `@global/system/services` 테이블(registry-decisions.md ADR-197)의
// 값(binary 타입)으로 그대로 저장되는 정본이다 — cfgsrv 자신은
// 내용을 해석하지 않고 불투명한 blob으로만 다룬다.
//
// M42(컨트롤 프로토콜)가 이 헤더에 `@wire-op` 마크업으로 op_list/
// op_start/... 를 추가할 자리다(ADR-195 방법론) — 이 구조체 자체는
// IPC 메시지가 아니라 저장 포맷이라 마크업 대상이 아니다.
#pragma once

#include <stdint.h>

#define MC_SVCMGR_MAX_DEPENDS 4u

typedef struct {
    char name[32];
    char exec_path[256];      // VFS 절대경로.
    char args[192];           // NUL로 구분된 argv, 마지막도 NUL. 0=인자 없음.
    uint8_t enabled;          // 부팅 시 자동 시작 여부(bool).
    uint32_t depends_on_count;
    char depends_on[MC_SVCMGR_MAX_DEPENDS][32];  // 이름으로 참조.
} mc_svcmgr_service_unit;
