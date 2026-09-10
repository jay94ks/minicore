// mc/pipesrv_client.h — pipesrv(mc/pipesrv_protocol.h) 클라이언트.
// docs/plan/musl-userland-porting.md §M51. 유일한 소비자는
// libc/sysdeps/minicore/syscall_shim.c(C)이므로 extern "C"로 감싼다
// (mc/procsrv_client.h가 M40에 겪은 것과 같은 이유로 처음부터
// 갖춰 둔다).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 성공하면 1, 실패(테이블 가득 참)면 0을 반환한다.
uint8_t mc_pipe_create(uint32_t pipesrv_handle, uint64_t* out_read_id, uint64_t* out_write_id);

// 반환값은 MC_PIPE_STATUS_*. status==OK일 때만 *out_len이 유효하다
// (0이면 진짜 EOF). WOULD_BLOCK이면 호출자가 mc_yield() 후 재시도.
uint32_t mc_pipe_read(uint32_t pipesrv_handle, uint64_t id, uint64_t requested_len, void* out_buf,
                       uint64_t* out_len);

// 반환값은 MC_PIPE_STATUS_*. status==OK일 때만 *out_written이 유효
// (부분 쓰기 허용). WOULD_BLOCK이면 재시도, BROKEN_PIPE면 재시도해도
// 소용없다(모든 리더가 닫음).
uint32_t mc_pipe_write(uint32_t pipesrv_handle, uint64_t id, const void* buf, uint64_t len,
                       uint64_t* out_written);

uint32_t mc_pipe_close(uint32_t pipesrv_handle, uint64_t id);

// fork()/dup2()로 같은 id를 추가로 참조하게 될 때 반드시 불러야
// 한다(mc/pipesrv_protocol.h의 op_dup 주석 참고).
uint32_t mc_pipe_dup(uint32_t pipesrv_handle, uint64_t id);

#ifdef __cplusplus
}  // extern "C"
#endif
