// mc/pipesrv_protocol.h — 익명 파이프 서버(user-service-manager.md
// 이후, docs/plan/musl-userland-porting.md §M51) 프로토콜 정본.
// ADR-195(docs/design/build-system.md)의 `@wire-op` 마크업 컨벤션을
// 그대로 따른다.
//
// 설계 근거(간단히): 이 서버는 절대 IPC 호출을 붙들고 대기하지
// 않는다(procsrv의 OP_WAIT과 같은 이유, OPEN-67 — 단일
// 요청-응답 루프인 서버가 "나중에 조건이 맞으면 회신"하려면 서버
// 자신이 멀티스레드/재진입을 다뤄야 해서 훨씬 복잡해진다). 대신
// 파이프가 비어 있거나(read) 가득 찼으면(write) 즉시
// `MC_PIPE_STATUS_WOULD_BLOCK`을 돌려주고, **호출자**(libc/sysdeps/
// minicore/syscall_shim.c)가 `mc_yield()`+재시도로 블로킹을
// 흉내낸다 — `mc_wait()`(procsrv 클라이언트)가 이미 쓰는 것과 같은
// 요령이다.
#ifndef MC_PIPESRV_PROTOCOL_H
#define MC_PIPESRV_PROTOCOL_H

#include <stdint.h>

// 파이프 하나당 고정 버퍼 크기(YAGNI — 계획 문서가 이미 이 값으로
// 단순화하기로 정했다).
#define MC_PIPE_CAPACITY 4096u

// read_id/write_id는 서버가 내주는 불투명한 정수다(진짜 커널 핸들이
// 아니다 — registry-decisions.md ADR-169 §결정4의 "프로토콜-레벨
// 정수"와 같은 정신). 0은 항상 무효.
// @wire-op label=1 name=create request=none reply="uint32 status; uint64 read_id; uint64 write_id"
#define MC_PIPE_OP_CREATE 1u
// pages[0]에 최대 requested_len바이트를 채워 돌려준다. len==0이고
// status==OK면 진짜 EOF(쓰기 쪽 refcount가 0이 됐다는 뜻) —
// WOULD_BLOCK과는 다른 상태다.
// @wire-op label=2 name=read request="uint64 id; uint64 requested_len" reply="uint32 status; uint64 len" (pages[0]=데이터, 출력)
#define MC_PIPE_OP_READ 2u
// pages[0]의 len바이트를 쓴다. 부분 쓰기를 허용한다(POSIX 파이프
// write()도 반드시 전체를 쓸 필요가 없다) — 실제로 쓴 바이트 수만
// reply에 담긴다.
// @wire-op label=3 name=write request="uint64 id; uint64 len" reply="uint32 status; uint64 written" (pages[0]=데이터, 입력)
#define MC_PIPE_OP_WRITE 3u
// 이 id 하나를 닫는다(refcount 감소) — dup()/fork()로 늘어난 각
// 참조를 각자 닫아야 진짜로 끝까지 닫힌다(§dup 참고).
// @wire-op label=4 name=close request="uint64 id" reply="uint32 status"
#define MC_PIPE_OP_CLOSE 4u
// id 하나를 추가로 참조한다고 알린다(refcount 증가) — dup2()나
// fork()로 같은 id를 한 프로세스 이상(또는 한 프로세스의 fd 슬롯
// 두 개 이상)이 들고 있게 될 때, 그 프로세스가 자신의 참조 하나를
// close()해도 나머지가 안 끊기도록 하기 위해 호출자가 명시적으로
// 불러야 한다(이 서버는 fork()/dup2()를 스스로 관찰할 수 없다 —
// libc/sysdeps/minicore/syscall_shim.c가 그 시점마다 이 오퍼레이션을
// 대신 불러 준다).
// @wire-op label=5 name=dup request="uint64 id" reply="uint32 status"
#define MC_PIPE_OP_DUP 5u

#define MC_PIPE_STATUS_OK 0u
#define MC_PIPE_STATUS_NOT_FOUND 1u
// read: 비어 있고 쓰기 쪽이 아직 하나 이상 열려 있음. write: 가득
// 찼고 읽기 쪽이 아직 하나 이상 열려 있음. 호출자가 재시도해야 한다.
#define MC_PIPE_STATUS_WOULD_BLOCK 2u
// write 전용 — 읽기 쪽 refcount가 이미 0(모든 리더가 닫음). 재시도
// 해도 절대 성립하지 않는다(POSIX EPIPE와 같은 자리).
#define MC_PIPE_STATUS_BROKEN_PIPE 3u
// create 전용 — 동시에 열 수 있는 파이프 수(MC_PIPE_MAX_PIPES)를
// 넘었음.
#define MC_PIPE_STATUS_TABLE_FULL 4u

#endif  // MC_PIPESRV_PROTOCOL_H
