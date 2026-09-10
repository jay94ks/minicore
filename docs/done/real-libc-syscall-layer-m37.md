# 완료 보고: real-libc-syscall-layer M37 — pthread 최소 구현

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M37
**관련 결정**: [kernel-scheduler.md](../design/kernel-scheduler.md) ADR-187, ADR-212
**실행일**: 2026-09-10

## 완료한 것

1. 새 커널 syscall `sys_thread_create`(`kernel/arch/x86_64/
   process_ops.cpp::thread_create`) — `fork_current()`와 달리 호출자의
   레지스터를 복제하지 않는다. `owner_space`/`handle_table`을
   클론하지 않고 호출자와 그대로 같은 포인터를 공유한다(ADR-187
   §결정1 — 이게 pthread를 fork()와 구분 짓는 핵심). `tls_fs_base`/
   `clear_child_tid_uaddr`를 받는다.
2. 새 커널 syscall `sys_futex`(`kernel/arch/x86_64/futex.cpp`) —
   `FUTEX_WAIT`/`FUTEX_WAKE`만. `object::address_space`에 대기열
   하나(`futex_waiters`)+스핀락(`futex_lock`)을 추가했다.
3. `MC_SYSCALL_THREAD_EXIT`가 `clear_child_tid_uaddr`(0이 아니면)
   에 0을 쓰고 `futex_wake(1)`한다 — Linux의 `CLONE_CHILD_CLEARTID`
   흉내.
4. `object::address_space`에 `heap_lock`(스핀락)을 추가해 `brk()`/
   `mmap_anon()`을 보호한다(ADR-180이 M37을 선행 조건으로 미리
   지적해 둔 것).
5. musl 자신의 진짜 `pthread_create()`/`pthread_join()`/
   `pthread_mutex_lock/unlock/trylock/timedlock()`이 처음 링크됐다.
   `__clone`(hidden asm)은 이 커널의 syscall ABI와 안 맞아 순수 C
   대체(`libc/sysdeps/minicore/clone_shim.c`)로 갈아 끼웠다.
6. musl-hello에 자기테스트를 추가했다 — 워커 둘을 `pthread_create()`
   로 만들어(M34 덕분에 서로 다른 코어에서 동시 실행 가능) 공유
   카운터를 `pthread_mutex_t`로 보호한 채 각각 10만 번씩 증가시킨
   뒤 `pthread_join()`으로 합류해 정확한 합계(20만)를 확인한다.

## 실행 중 발견한 것

계획(ADR-187) 자체의 범위는 좁히지 않았지만, 실행 중 진짜 버그
2건과 예상보다 큰 포팅 작업 1건을 발견했다.

### 1. musl의 `__clone`도 M36의 `__restore_rt`와 같은 ABI 불일치

`third_party/musl/src/thread/x86_64/clone.s`는 raw `syscall` 명령을
진짜 Linux ABI(번호를 RAX에 싣는다)로 직접 실행해, M28의 우회
대상인 `__syscallN`(`arch/x86_64/syscall_arch.h`)을 전혀 거치지
않는다 — M36에서 `__restore_rt`가 겪은 것과 정확히 같은 문제다.
다만 이번엔 한 줄만 고치는 패치로 끝나지 않았다 — 진짜 `clone(2)`
의 자식 쪽 분기는 "부모와 같은 명령어 스트림을 이어 간다"는
의미론에 의존하는데, 이 커널의 `sys_thread_create`는 애초에 새
스레드가 `entry_rip`로 곧바로 진입하는 다른 모양이라 그 트릭 자체가
필요 없다. `clone.s`를 소스 목록에서 완전히 빼고
`libc/sysdeps/minicore/clone_shim.c`(순수 C)로 대체했다 — musl이
넘기는 `stack` 인자 자체도 정렬돼 있지 않다는 것(원본 asm이
`and $-16,%rsi; sub $8,%rsi`로 "누군가 call한 것처럼" 만드는 게 asm
쪽 책임이었다)을 원본을 읽고 확인해, 대체 코드에도 같은 계산을
옮겼다 — 안 그러면 `func`(musl의 `start`/`start_c11`) 내부의 정렬
요구 지역변수가 깨질 수 있었다.

### 2. `SYS_exit`/`SYS_exit_group` 混同 — 진짜 회귀 버그

M32부터 `syscall_shim.c`가 `case SYS_exit: case SYS_exit_group:`로
둘을 합쳐 항상 `mc_process_exit_report()`(프로세스 전체가 끝났다고
procsrv에 보고)를 부르고 있었다 — 단일 스레드 프로세스만 있던
M27~M36까지는 "이 스레드가 끝남 = 이 프로세스가 끝남"이 항상 참이라
드러나지 않았다. musl의 `__pthread_exit`(다른 스레드는 계속
살아있는 채로 이 스레드 하나만 끝날 때)은 raw `SYS_exit`(그룹
아님)을 직접 쓰는데, 진짜 프로세스 종료(`_exit()`/`exit()`/`main()`
정상 반환)는 항상 `SYS_exit_group`을 쓴다는 것을 musl 소스를 읽고
확인했다. 둘을 갈랐다 — `SYS_exit_group`만 procsrv에 보고하고
`SYS_exit`은 이 스레드만 조용히 버린다. 고치지 않았다면 워커
pthread 하나가 끝날 때마다 procsrv에 "이 프로세스 전체가 죽었다"고
잘못 보고했을 것이다.

### 3. `__lock`/`__unlock`을 실제로 되돌려야 했다

M30/M31이 "아직 스레드가 하나뿐"이라는 이유로 no-op으로 대체해 둔
`__lock`/`__unlock`(`sysdeps/minicore/lock_shim.c`)은 musl의
`__tl_lock`/`__tl_unlock`(pthread_create.c의 스레드 목록 락)이
실제로 쓰는 자리다. 두 워커의 `pthread_exit()`이 거의 동시에
끝나는 시나리오(대칭적인 워크로드라 실제로 겹칠 수 있다)에서
no-op을 그대로 뒀다면 스레드 목록(이중 연결 리스트) 자체가 깨질
위험이 있었다 — QEMU에서 실제로 재현하기 전에 musl 소스 분석으로
먼저 발견해 미리 고쳤다(musl 원본 `src/thread/__lock.c`, 진짜
futex 기반 congestion 처리로 되돌림). `pthread_mutex_lock/unlock`
자체(자기테스트가 직접 쓰는 카운터 보호)는 `LOCK`/`UNLOCK`을 거치지
않고 순수 원자 연산+`__wake`/`__futexwait`만 쓴다는 것도 이 분석
중에 확인했다.

### 4. 그 외 예상보다 컸던 링크 의존성 사슬

`pthread_create.c` 하나가 `pthread_join.c`/`pthread_mutex_lock.c`/
`pthread_mutex_unlock.c`/`pthread_mutex_trylock.c`/
`pthread_mutex_timedlock.c`/`__timedwait.c`/`pthread_setcancelstate.c`/
`pthread_testcancel.c`/`__unmapself.c`/`__lock.c`/`__wait.c`/
`vmlock.c`/`clock_gettime.c`/`mprotect.c`/`misc/syscall.c`까지
연쇄적으로 끌어왔다(M28의 "링커가 알려 주는 대로 하나씩" 방법론을
그대로 반복). `__mprotect`(guard page용)는 이 커널의 `sys_mmap_anon`
이 `PROT_NONE`을 구분하지 않고 항상 읽기/쓰기로 매핑하는 것과
musl 자신이 `mprotect`의 `ENOSYS` 실패를 명시적으로 허용하는 코드
경로(`if (__mprotect(...) && errno != ENOSYS) fail;`) 덕분에, 커널이
`SYS_mprotect`를 전혀 구현하지 않아도(기본 미구현 처리, -ENOSYS)
문제없이 동작한다 — 이번 라운드는 진짜 페이지 보호를 구현하지
않았다.

## 검증

x86_64 전체 재빌드 성공. QEMU에서 확인: "musl pthread_create ok=1"
→ "musl pthread_join ok=1" → "musl pthread mutex counter ok=1"
(카운터 정확히 200000, 손실 없음). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 110) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0) |

## 남겨 둔 것

real-libc-syscall-layer.md M38(SDK 내보내기)부터 계속 진행한다.
`FUTEX_CMP_REQUEUE` 등 고급 futex 연산, `SCHED_FIFO`/`SCHED_RR`,
`pthread_cancel`, 스택 guard page의 진짜 강제는 ADR-187이 이미
명시한 대로 범위 밖이다. `handle_table` 무동기화(OPEN-68)는 이번
자기테스트가 두 워커 모두 IPC/핸들 조작을 전혀 안 해 여전히
실사용으로 검증되지 않았다 — 다음에 실제로 건드리는 시나리오가
생기면 재검토.
