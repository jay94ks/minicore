// libmc/include/mc/vfs_client.h — VFS의 OP_OPEN 클라이언트
// (docs/spec/fs-protocol.md v3/v4, docs/design/foundations.md ADR-170).
#pragma once

#include <stdint.h>

// M40(user-service-manager.md §M40) 실행 중 발견 — 이 파일의 함수는
// (다른 mc/*_client.h와 마찬가지로) .c로 구현돼 C 링크 심벌을
// 낸다. C++ 소비자(servers/*.cpp)가 extern "C" 없이 이 헤더를
// include하면 C++ 이름 맹글링으로 링크가 깨진다 — 지금까지는 C++
// 서버들이 이 계층의 함수를 직접 호출한 적이 없어(구조체/상수만
// 쓰거나 자체 syscall 트램폴린만 썼다) 드러나지 않았을 뿐이다.
#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}  // extern "C"
#endif
