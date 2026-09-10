// mc/cfgsrv_client.h — cfgsrv의 레지스트리 프로토콜(docs/spec/registry.md,
// docs/design/registry-decisions.md ADR-169) 클라이언트. M19부터
// procsrv 자신의 self-test가 이 프로토콜을 직접 IPC로 인라인했지만
// (별도 libmc 함수가 없었다), user-service-manager.md §M41(ADR-197)
// 이 "이미 존재하는 클라이언트 코드를 재사용한다"고 예정해 둔
// 자리를 이번에 처음 실제로 채운다 — svcmgr가 `@global/system/
// services` 테이블을 읽는 첫 진짜 소비자다.
//
// mc_getpid 등과 같은 이유(procsrv_client.h 참고)로 여기서도
// C++ 소비자(servers/svcmgr)를 위해 extern "C"로 감싼다.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// docs/spec/registry.md §5 — reg_op.
#define MC_REG_OP_OPEN_TABLE 1u
#define MC_REG_OP_CREATE_TABLE 2u
#define MC_REG_OP_DELETE_TABLE 3u
#define MC_REG_OP_LIST_CHILDREN 4u
#define MC_REG_OP_GET_VALUE 5u
#define MC_REG_OP_SET_VALUE 6u
#define MC_REG_OP_DELETE_VALUE 7u
#define MC_REG_OP_LIST_VALUES 8u
#define MC_REG_OP_SET_PERMISSIONS 9u

// docs/spec/registry.md §5 — reg_error.
#define MC_REG_ERR_OK 0u
#define MC_REG_ERR_NOT_FOUND 1u
#define MC_REG_ERR_PERMISSION_DENIED 2u
#define MC_REG_ERR_ALREADY_EXISTS 3u
#define MC_REG_ERR_INVALID_PATH 4u
#define MC_REG_ERR_TYPE_MISMATCH 5u

// docs/spec/registry.md §2 — reg_value_type.
#define MC_REG_TYPE_STRING 0u
#define MC_REG_TYPE_INT64 1u
#define MC_REG_TYPE_BOOLEAN 2u
#define MC_REG_TYPE_BINARY 3u

// op=MC_REG_OP_OPEN_TABLE(있으면 열기) 또는 MC_REG_OP_CREATE_TABLE
// (없으면 만들기, 호출자 스키마와 일치하거나 uid==0이어야 성공).
// 성공하면 *out_table_id에 이후 값 오퍼레이션이 쓸 프로토콜-레벨
// 정수를 채운다(registry-decisions.md ADR-169 §결정4 — 진짜 커널
// 핸들이 아니다).
uint64_t mc_reg_open_or_create(uint32_t cfgsrv_handle, uint32_t op, uint32_t caller_uid,
                                const char* caller_username, const char* path,
                                uint64_t* out_table_id);

// binary 타입 값 조회/설정 — user-service-manager.md §M41이
// `mc_svcmgr_service_unit`(mc/svcmgr_protocol.h) 구조체를 불투명한
// blob으로 저장/조회하는 데 쓴다(ADR-197 §영향 — cfgsrv는 내용을
// 해석하지 않는다).
uint64_t mc_reg_get_binary(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                            const char* key, void* out_buf, uint64_t out_cap,
                            uint64_t* out_len);
uint64_t mc_reg_set_binary(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                            const char* key, const void* value, uint64_t value_len);

uint64_t mc_reg_delete_value(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                              const char* key);

// out_names_blob에 NUL로 구분된 key 이름들을 채우고 *out_count에
// 개수를 채운다.
uint64_t mc_reg_list_values(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                             char* out_names_blob, uint64_t out_cap, uint64_t* out_count);

#ifdef __cplusplus
}  // extern "C"
#endif
