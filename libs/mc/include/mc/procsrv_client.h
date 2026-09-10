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

// M40(user-service-manager.md §M40) 실행 중 발견 — 이 파일의 함수는
// .c로 구현돼 C 링크 심벌을 낸다. C++ 소비자(servers/svcmgr 등)가
// extern "C" 없이 이 헤더를 include하면 C++ 이름 맹글링으로 링크가
// 깨진다 — 지금까지는 C++ 서버들이 이 계층의 함수를 직접 호출한
// 적이 없어(procsrv 자신은 이 프로토콜을 IPC로 직접 구현하지,
// 클라이언트로서 부르지 않는다 — musl의 syscall_shim.c(C)가 유일한
// 기존 소비자였다) 드러나지 않았을 뿐이다.
#ifdef __cplusplus
extern "C" {
#endif

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

// M36(real-libc-syscall-layer.md §M36) — mc_fork()가 부모 관점으로
// 성공했을 때(가장 최근 호출 한 번) 커널이 함께 내준
// object_kind::thread 핸들(k_right_can_signal 부여 —
// process_ops.hpp::fork_current() 참고)을 캐시해 둔 것. musl의 진짜
// fork()는 이 핸들을 모른다(POSIX ABI에 그런 자리가 없다) — 그래서
// 별도로 꺼내 쓸 수 있게 이 접근자를 둔다. pid는 procsrv만 아는
// 개념이라(ADR-201) 커널 레벨에서 "pid로 자식을 찾아 시그널 보내기"
// (SYS_kill의 pid 인자)는 여전히 이 라운드에서 지원하지 않는다 —
// 자식에게 직접 시그널을 보내려면(예: sigaction+kill 왕복 테스트)
// mc_fork() 직후 이 핸들을 mc_signal_send()에 그대로 넘긴다.
uint32_t mc_last_fork_child_thread_handle(void);

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

// M40(user-service-manager.md §M40, ADR-192 §결정3/ADR-196 §결정5) —
// svcmgr가 자기 pid를 확정한 뒤 딱 한 번 부른다. M27이
// parent_pid=k_parent_none으로 잠정 등록해 둔 initrun의 고아들
// (커널 서버 전부)을 svcmgr_pid로 재부모화한다. 반환값은
// 실제로 재부모화된 프로세스 수(MC_PROC_STATUS_OK가 아니면 0 —
// 이 op은 실패 경로가 없어 항상 OK다).
uint32_t mc_adopt_orphans(uint32_t procsrv_handle, uint32_t svcmgr_pid);

// M43(user-service-manager.md §M43, docs/design/security-model.md
// ADR-218) — svcmgr가 백그라운드 스레드에서 반복 호출한다(비블로킹
// 폴링, procsrv가 단일 요청-응답 루프라 OPEN-67과 같은 이유로 진짜
// 블로킹을 못 한다). 대기 중인 로그인 이벤트가 있으면 true를 반환하고
// *out_uid/*out_username_packed를 채운다 — 없으면 false(에러 아님).
uint8_t mc_poll_login_event(uint32_t procsrv_handle, uint32_t* out_uid,
                             uint64_t* out_username_packed);

// M43 — ADR-217의 badge(이 핸들이 initrun의 svcmgr+procsrv 조합
// 전용 하드코딩 특수 케이스로 실제로 스탬핑해 준 것)로 procsrv가
// "진짜 svcmgr"임을 판정한 뒤, username_packed 계정에게 유효한
// 서비스 위임(ADR-218)이 있으면 그 계정 몫으로 procsrv 자신이
// 컴파일 시점에 심어 둔 데모 ELF를 spawn한다(elf_data/elf_size는
// 요청에 싣지 않는다 — svcmgr의 포인터는 procsrv의 주소공간에서
// 무의미하다). ADR-193의 준비완료 핸드셰이크는 연결하지 않는다
// (OPEN-73). 성공하면 MC_PROC_STATUS_OK와 *out_thread_handle을 채운다.
uint32_t mc_spawn_delegated_unit(uint32_t procsrv_handle, uint64_t username_packed,
                                  uint32_t* out_thread_handle);

#ifdef __cplusplus
}  // extern "C"
#endif
