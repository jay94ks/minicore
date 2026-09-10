// libmc/include/mc/procsrv_client.h — procsrv의 wait/kill/self_register/
// fork_register(mc/procsrv_protocol.h) 클라이언트. M27은 procsrv
// 자신의 self-test에서만 이 프로토콜을 직접 IPC로 썼다(별도 libmc
// 함수가 없었다) — M32(real-libc-syscall-layer.md §M32, ADR-183
// §결정4)가 처음으로 이 프로토콜의 진짜 소비자(musl의 syscall_shim.c,
// libc/sysdeps/minicore/syscall_shim.c)를 만들면서 그 소비자가 부를
// 얇은 래퍼를 여기 둔다 — syscall_shim.c 자신은 이 파일의 함수만
// 부르고 IPC/프로토콜 로직을 직접 두지 않는다.
#pragma once

#include <stdint.h>

// 이 프로세스가 procsrv에게서 받은 자신의 pid(최초 호출 시
// self_register로 얻어 커널에 캐시한다, 이후 호출은 IPC 없이 그
// 캐시를 그대로 돌려준다 — getpid()는 POSIX상 실패하지 않는 가벼운
// 호출이어야 한다). procsrv_handle은 이 프로세스의 handle 테이블에서
// procsrv의 endpoint를 가리키는 핸들 번호(예: libc/sysdeps/minicore/
// syscall_shim.c::MC_PROCSRV_HANDLE). 캐시는 유저랜드 static이 아니라
// mc_procsrv_pid_get/set(mc/syscall.h, kernel_objects.hpp::thread::
// procsrv_pid)이다 — execve()가 owner_space(유저랜드 static이 사는
// 곳)를 통째로 새 이미지로 갈아엎어도, 커널 스레드 객체는 그대로
// 살아남아야 fork()의 자식이 곧바로 exec()해도 자기 pid를 잃지
// 않는다(procsrv_client.c 상단 주석에 더 자세히 적어 뒀다).
uint32_t mc_getpid(uint32_t procsrv_handle);

// mc_getpid()와 달리 캐시가 비어 있으면(이 프로세스가 fork()/
// getpid()를 한 번도 부른 적 없음) procsrv에 등록을 시도하지 않고
// 그냥 0을 돌려준다 — SYS_exit 경로(아래 mc_process_exit_report)가
// "애초에 procsrv가 모르는 프로세스라면 등록까지 해서 보고할 필요
// 없다"를 판단하는 데 쓴다.
uint32_t mc_getpid_cached(void);

// musl의 진짜 fork()(_Fork(), SYS_fork로 옴)가 기대하는 그대로:
// 자식에서는 0, 부모(성공)에서는 새 자식의 pid, 실패하면 음수
// errno를 반환한다(Linux syscall ABI와 동일한 규약 — syscall_shim.c
// 가 그대로 반환값을 돌려주면 된다). 실제 sys_fork(MC_SYSCALL_FORK)
// **이전에** procsrv에게 자식의 pid를 미리 확정받는다 — 그 값을
// 담은 지역변수가 sys_fork의 COW 복제로 부모/자식 양쪽에 똑같이
// 남으므로, 부모는 그 값을 그대로 반환하고 자식은 자신의 캐시된
// self pid를 그 값으로 갱신하기만 하면 된다(procsrv와 별도 IPC
// 왕복이 필요 없다 — 필요했다면 부모/자식이 "누가 진짜 자식인가"를
// 다시 조율해야 했을 것이다).
long mc_fork(uint32_t procsrv_handle);

// musl의 waitpid(target_pid, ...)/wait4(target_pid, ...)(target_pid>0
// 인 경우만 — 이 라운드는 "특정 자식 pid를 기다린다"만 지원한다,
// pid<=0의 "임의의 자식"/"프로세스 그룹" 의미론은 범위 밖이다,
// real-libc-syscall-layer.md §M32 스코프 참고)가 옮겨 붙는 대상.
// M27의 OP_WAIT는 논블로킹 폴링(procsrv가 단일 요청/응답 루프이기
// 때문, ADR-201/OPEN-67)이므로, 이 함수가 그 폴링을 대신 반복한다
// (servers/procsrv/main.cpp::run_as_m27_parent_a()의 재시도 루프와
// 같은 요령). 반환값은 MC_PROC_STATUS_*(mc/procsrv_protocol.h) —
// OK면 *out_exit_code를 채운다. 재시도 한도를 넘기면
// MC_PROC_STATUS_STILL_RUNNING을 그대로 돌려준다(호출자가 그것을
// 어떻게 다룰지는 syscall_shim.c의 선택 — 이 라운드는 그 한도 안에서
// 항상 끝난다고 가정한다).
uint32_t mc_wait(uint32_t procsrv_handle, uint32_t target_pid, int32_t* out_exit_code);

// musl의 진짜 exit()/_exit() 경로(SYS_exit/SYS_exit_group)가 procsrv의
// process_entry를 좀비로 만들어야 부모의 mc_wait()가 회수할 수
// 있다(M27의 OP_EXIT_REPORT를 그대로 재사용) — self pid가 아직 없는
// 프로세스(한 번도 mc_getpid()/mc_fork()를 부르지 않은 경우)라면
// procsrv가 모르는 pid라 조용히 무시되므로(handle_proc_exit_report의
// find_process 실패 분기) 호출해도 안전하다.
void mc_process_exit_report(uint32_t procsrv_handle, uint32_t pid, int32_t exit_code);
