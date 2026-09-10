// libmc/include/mc/ps2_client.h — servers/drivers/ps2의 OP_READ_KEY
// 클라이언트(docs/design/foundations.md ADR-170).
#pragma once

#include <stdint.h>

// M40 실행 중 발견(vfs_client.h의 같은 주석 참고) — C++ 소비자를
// 위한 extern "C".
#ifdef __cplusplus
extern "C" {
#endif

#define MC_PS2_OP_READ_KEY 1u

typedef struct {
    int got_key;    // 0/1.
    uint8_t ascii;  // got_key==1일 때만 유효.
} mc_ps2_key;

mc_ps2_key mc_ps2_read_key(uint32_t ps2_handle);

#ifdef __cplusplus
}  // extern "C"
#endif
