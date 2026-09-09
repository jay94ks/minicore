// libmc/include/mc/vfs_client.h — VFS의 OP_OPEN 클라이언트
// (docs/spec/fs-protocol.md v3/v4, docs/design/foundations.md ADR-170).
#pragma once

#include <stdint.h>

#define MC_FS_OP_OPEN 1u
#define MC_FS_OP_WRITE 2u
#define MC_FS_OP_READ 3u
#define MC_FS_OP_LIST 4u

#define MC_FS_PATH_BUDGET 24u  // regs[0..2], fs-protocol.md v3 §1.

#define MC_FS_STATUS_OK 0u
#define MC_FS_STATUS_NOT_FOUND 1u
#define MC_FS_STATUS_NO_SPACE 3u
#define MC_FS_STATUS_GUEST_DENIED 5u

// path를 vfs_handle로 연다(identity=0이면 guest/jail이 아니다,
// fs-protocol.md v3 §2.1). 성공하면 0(MC_FS_STATUS_OK)을 반환하고
// *out_open_file_id/*out_fs_handle을 채운다 — 이후 read/write/list는
// *out_fs_handle로 대상 FS 서버에 직접 건다(vfs를 다시 거치지 않음).
uint64_t mc_vfs_open(uint32_t vfs_handle, const char* path, uint64_t identity,
                      uint64_t* out_open_file_id, uint32_t* out_fs_handle);
