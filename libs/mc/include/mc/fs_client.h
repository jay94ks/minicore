// libmc/include/mc/fs_client.h — OP_OPEN 이후 FS 서버에 직접 거는
// OP_READ/OP_WRITE/OP_LIST 클라이언트(docs/spec/fs-protocol.md
// v3/v4). ADR-170 §영향이 "실제로 필요해지는 시점에 추가"로 미뤄
// 뒀던 OP_WRITE 클라이언트는 M54(musl-userland-porting.md §M54,
// ADR-225)가 msh의 출력 리다이렉션(`>`)을 위해 처음 추가했다.
#pragma once

#include <stdint.h>

// M40 실행 중 발견(vfs_client.h의 같은 주석 참고) — C++ 소비자를
// 위한 extern "C".
#ifdef __cplusplus
extern "C" {
#endif

// EOF까지(또는 out_cap에 닿을 때까지) 순차적으로 읽어 out_buf에
// 이어 담는다(memfs의 읽기 커서가 자동으로 전진한다, fs-protocol.md
// v3 §2.3). 반환값은 실제로 읽은 총 바이트 수.
uint64_t mc_fs_read_all(uint32_t fs_handle, uint64_t open_file_id, uint8_t* out_buf,
                         uint64_t out_cap);

// M31(real-libc-syscall-layer.md §M31) — mc_fs_read_all과 달리 **단
// 한 번의** OP_READ만 보낸다(POSIX read()가 요구하는 부분 읽기
// 의미론 — musl의 __stdio_read가 자기 버퍼 크기만큼만 요청한다).
// count가 한 페이지(4096)를 넘으면 그만큼만 요청한다(pages[] 전송
// 한계) — 서버가 실제로 읽은 바이트 수(항상 요청한 양 이하)만큼만
// out_buf에 채우므로, mc_fs_read_all처럼 "서버가 더 읽었는데 일부만
// 복사"해서 데이터를 잃어버릴 위험이 없다. 0 반환 = EOF 또는 오류.
uint64_t mc_fs_read(uint32_t fs_handle, uint64_t open_file_id, uint8_t* out_buf, uint64_t count);

// M54(musl-userland-porting.md §M54, ADR-225) — mc_fs_read와 대칭인
// 단 한 번의 OP_WRITE(fs-protocol.md v3 §2.2, write_cursor 위치부터
// 이어 쓴다). count가 한 페이지(4096)를 넘으면 그만큼만 보낸다(호출자
// 가 남은 만큼 다시 불러야 한다 — write()의 짧은 쓰기 의미론과 같다).
// 반환값은 실제로 쓴 바이트 수(항상 요청한 양 이하), 0=오류.
uint64_t mc_fs_write(uint32_t fs_handle, uint64_t open_file_id, const uint8_t* data,
                      uint64_t count);

// fs-protocol.md v4 §2.4 — memfs 전용. 성공하면 0(OK)을 반환하고
// out_names_blob에 NUL로 구분된 이름 목록을, *out_count에 개수를
// 채운다.
uint64_t mc_fs_list(uint32_t fs_handle, uint8_t* out_names_blob, uint64_t out_cap,
                     uint32_t* out_count);

#ifdef __cplusplus
}  // extern "C"
#endif
