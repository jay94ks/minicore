// mc/svcmgr_protocol.h — 유저 서비스 유닛 모델(user-service-manager.md,
// docs/design/boot-and-drivers.md ADR-196 §결정2). 이 구조체가
// `@global/system/services` 테이블(registry-decisions.md ADR-197)의
// 값(binary 타입)으로 그대로 저장되는 정본이다 — cfgsrv 자신은
// 내용을 해석하지 않고 불투명한 blob으로만 다룬다.
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

// M42(user-service-manager.md §M42, docs/design/boot-and-drivers.md
// ADR-196 §결정7) — svcmgr 컨트롤 프로토콜. ADR-195의 순서(마크업
// 먼저, `tools/gen-wire-docs.py`로 추출·확인 후 구현 — M27이 첫
// 적용, 이번이 두 번째)를 그대로 따른다. 전부 name[32]로 대상
// 유닛을 가리킨다(현재 svcmgr가 실제로 아는 유닛 = 부팅 시 읽은
// @global/system/services 목록, M41).
//
// @wire-op label=1 name=list request=none reply="uint32 status; uint32 count" (pages[0]=NUL로 구분된 이름 목록)
#define MC_SVCMGR_OP_LIST 1u
// @wire-op label=2 name=status request="char name[32]" reply="uint32 status; uint8 running; uint32 thread_handle"
#define MC_SVCMGR_OP_STATUS 2u
// @wire-op label=3 name=start request="char name[32]" reply="uint32 status"
#define MC_SVCMGR_OP_START 3u
// @wire-op label=4 name=stop request="char name[32]" reply="uint32 status"
#define MC_SVCMGR_OP_STOP 4u
// @wire-op label=5 name=restart request="char name[32]" reply="uint32 status"
#define MC_SVCMGR_OP_RESTART 5u
// @wire-op label=6 name=register request="mc_svcmgr_service_unit unit (pages[0])" reply="uint32 status"
#define MC_SVCMGR_OP_REGISTER 6u
// @wire-op label=7 name=unregister request="char name[32]" reply="uint32 status"
#define MC_SVCMGR_OP_UNREGISTER 7u

#define MC_SVCMGR_STATUS_OK 0u
#define MC_SVCMGR_STATUS_NOT_FOUND 1u
#define MC_SVCMGR_STATUS_ALREADY_RUNNING 2u
#define MC_SVCMGR_STATUS_NOT_RUNNING 3u
