// libmc/include/mc/fs_client.h — OP_OPEN 이후 FS 서버에 직접 거는
// OP_READ/OP_LIST 클라이언트(docs/spec/fs-protocol.md v3/v4). 이번
// 라운드의 셸은 읽기만 하므로 OP_WRITE 클라이언트는 아직 없다
// (ADR-170 §영향 — 실제로 필요해지는 시점에 추가).
#pragma once

#include <stdint.h>

// EOF까지(또는 out_cap에 닿을 때까지) 순차적으로 읽어 out_buf에
// 이어 담는다(memfs의 읽기 커서가 자동으로 전진한다, fs-protocol.md
// v3 §2.3). 반환값은 실제로 읽은 총 바이트 수.
uint64_t mc_fs_read_all(uint32_t fs_handle, uint64_t open_file_id, uint8_t* out_buf,
                         uint64_t out_cap);

// fs-protocol.md v4 §2.4 — memfs 전용. 성공하면 0(OK)을 반환하고
// out_names_blob에 NUL로 구분된 이름 목록을, *out_count에 개수를
// 채운다.
uint64_t mc_fs_list(uint32_t fs_handle, uint8_t* out_names_blob, uint64_t out_cap,
                     uint32_t* out_count);
