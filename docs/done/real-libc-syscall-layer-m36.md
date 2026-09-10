# 완료 보고: real-libc-syscall-layer M36 — 완전한 signal 계층(syscall-리턴 시점 한정)

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M36
**관련 결정**: [kernel-scheduler.md](../design/kernel-scheduler.md) ADR-186, ADR-211
**실행일**: 2026-09-10

## 완료한 것

1. `object::thread`에 `pending_signals`/`signal_mask`(둘 다
   `uint64_t`)와 `sigactions[32]`(`{handler, restorer}`)를 추가했다.
   `k_right_can_signal`은 `k_right_can_kill`(ADR-178)의 별칭이다.
2. 새 커널 syscall 3개(`kernel/arch/x86_64/signal.hpp`/`.cpp`, 신규
   파일) — `sys_signal_action`(자기 자신의 핸들러 등록/조회),
   `sys_signal_send`(대상 thread 핸들의 `pending_signals`에 비트만
   세운다), `sys_rt_sigreturn`(핸들러 종료 후 실행 상태 복원).
3. `syscall_entry.S`가 `call syscall_dispatch` 직후
   `check_signal_delivery(saved_regs, dispatch_ret)`를 불러 return-to-user
   경계에서 대기 중인 시그널을 확인하고, 있으면 saved_regs 9워드를
   그 자리에서 다시 써 핸들러로 리다이렉트한다.
4. `fork_current()`가 `process_spawn`과 같은 `out_thread_handle`
   패턴으로 부모 handle_table에 자식을 가리키는 thread 핸들(권한
   `k_right_can_signal`)을 만들어 돌려준다 — `mc_last_fork_child_thread_handle()`
   (libmc)로 캐시해 둔다.
5. musl 자신의 진짜 `sigaction()`(`SYS_rt_sigaction`을 부른다,
   `third_party/musl/src/signal/sigaction.c`)이 처음 링크됐다 —
   `libc/sysdeps/minicore/syscall_shim.c`가 `struct mc_ksigaction`을
   커널의 `mc_signal_action_request`로 마샬링한다.
6. musl-hello에 자기테스트를 추가했다 — 자식이 `sigaction(SIGUSR1, ...)`
   으로 진짜 핸들러를 등록하고 대기하다가, 부모가
   `mc_signal_send()`로 보낸 신호를 받아 핸들러가 실행됨을 자식의
   exit code(55)로 확인한다("musl signal handler ok=1").

## 계획 대비 범위 조정 (ADR-211)

ADR-186(계획 단계)이 예정한 6개 결정 중 §결정1/2/4와 §결정3의 절반
(syscall 리턴 경로)만 실제로 구현했다. **아래는 전부 이번 라운드
범위 밖으로 남겼다**:

- §결정3의 나머지 절반 — IRETQ(인터럽트 리턴) 경로의 시그널 확인.
  이번 자기테스트가 필요로 하는 유일한 경로가 syscall 리턴이었다.
- §결정5 — `SIGCHLD`의 procsrv 발 자동 전달. M27의 `OP_WAIT`
  블로킹 폴링 관례를 그대로 둔다.
- §결정6 — `SIGKILL`을 대기열에서 즉시 `unlink`하는 메커니즘.
  `sys_process_kill`(ADR-178)의 기존 한계(대상이 대기열에 갇혀
  있으면 다시 깨우지 않는 한 폐기되지 않음)가 그대로 남는다.

**OPEN-65는 ADR-186 계획 단계에 앞당겨 "해소" 표시가 돼 있었으나,
§결정6이 실제로 구현되지 않아 다시 열었다**([open-items.md](../design/open-items.md)
참고) — 계획 시점의 의도와 실제 구현 사이의 간극을 정직하게
기록해 두기 위함이다.

`SIG_DFL`(0)/`SIG_IGN`(1) 둘 다 "무시"로 단순화했다(진짜 기본
종료 의미론은 대부분의 시그널에서 프로세스 종료다 — 범위 밖).
`SYS_kill(pid, sig)`은 구현하지 않았다(procsrv가 fork_register된
자식의 thread 핸들을 모른다, OPEN-67과 같은 뿌리) — 자기테스트는
`mc_last_fork_child_thread_handle()`로 받은 핸들을 `mc_signal_send()`
에 직접 쓴다.

## 실행 중 발견한 것

세 가지 실제 문제를 QEMU 기반 반복 검증으로 발견·수정했다.

### 1. musl include 순서 버그 (신호 자체와는 무관, 빌드 중 처음 드러남)

`libc/CMakeLists.txt`가 `src/internal`을 `arch/x86_64`보다 먼저
검색하도록 `target_include_directories`를 호출하고 있었다(musl 자신의
Makefile은 반대 순서 — `arch/$(ARCH)` → `arch/generic` → `src/include`
→ `src/internal` → 공개 `include/`). arch별로 오버라이드되는 헤더
(`ksigaction.h`)가 이 순서 때문에 제네릭 버전(`__restore`/`__restore_rt`
를 별도 심벌 두 개로 선언)으로 잡혀, x86_64 버전의
`#define __restore __restore_rt` 별칭이 전혀 적용되지 않았다 —
`sigaction.c`가 존재하지 않는 `__restore` 심벌을 참조해 링크가
깨졌다. `target_include_directories` 호출 순서를 musl 원본과
맞춰 고쳤다(arch 그룹 → PRIVATE(src/include, src/internal) →
생성된 헤더+공개 include/).

### 2. `__restore_rt` 트램폴린의 syscall 번호 채널 불일치

순서를 고친 뒤 링크는 통과했지만, 실제 QEMU 실행에서 자식이
`sigusr1_handler`를 실행한 뒤 `#GP`로 죽었다 — 죽은 위치가 정확히
`__restore_rt`의 `syscall` 명령 바로 다음 주소였다. 원인: musl의
`restore.s`(핸들러가 `ret`한 뒤 CPU가 곧바로 뛰어드는 손짜기
트램폴린 — `__syscallN`류의 C 래퍼를 전혀 거치지 않는다, 진짜
Linux도 이 트램폴린만은 raw `syscall`을 직접 쓴다)는 원본 그대로
진짜 Linux ABI(syscall 번호를 RAX에 싣는다: `mov $15,%rax`)를 썼다.
하지만 이 커널의 `syscall_entry`는 번호를 RDI에서 읽는다
(`syscall_entry.S` 상단 주석) — M28(ADR-183)의 우회 전략은
`arch/x86_64/syscall_arch.h`의 `__syscallN` 함수들을
`__minicore_syscall_dispatch`로 바꿔치기해 이 ABI 차이를
흡수했지만, `restore.s`는 그 함수들을 전혀 거치지 않는 특수
경로라 우회가 적용될 자리가 없었다. 결과: `sys_rt_sigreturn`이
전혀 실행되지 않고, 핸들러가 실행 중 남긴 임의의 RDI 값을 syscall
번호로 오인해 엉뚱한(대개 무효한) syscall이 실행되다가 SYSRET
이후 사용자 코드가 전혀 예상 못한 지점에서 죽었다.

`third_party/patches/musl/0002-restore-trampoline.patch`(ADR-022/
ADR-183이 이미 마련해 둔 `tools/apply-patches.sh` 패치 파이프라인을
재사용, 0001 다음 두 번째로 실제 사용)로 그 한 줄만
`mov $21,%rdi`(`MC_SYSCALL_RT_SIGRETURN`)로 바꿨다 — `rt_sigreturn`
은 인자가 없어 RSI/RDX/R10에 남는 값은 무해하다.

### 3. 자기테스트 자체의 fork 스케줄링 경합

위 두 버그를 고친 뒤에도 "musl signal handler ok=0"이 계속
나왔다. 커널에 임시 디버그 로그를 넣어 확인한 순서:
`[signal] send`(handler=0x0) → `[signal] consume`(handler=0x0,
"무시"로 버려짐) → `[signal] action`(handler=진짜 주소 — 등록은
이 시점에야 끝남). 부모가 자식보다 먼저 스케줄돼 자식이
`sigaction()`으로 핸들러를 등록하기 전에 `mc_signal_send()`가
도착했고, 그 시점의 §결정4 "SIG_DFL=무시" 단순화가 그 신호를
조용히 소비해 버렸다. fork()는 COW라 자식이 "등록 끝났다"는
플래그를 부모가 직접 읽을 수 있는 공유 메모리에 쓸 방법이 없다
(각자의 쓰기는 자기 사본에만 반영된다, `g_sigusr1_count`와 같은
이유) — 그래서 재전송 루프로 그 창을 덮기로 했는데, 처음 시도
(`getpid()`를 반복 호출하며 재전송)는 효과가 없었다: 이 프로세스의
pid가 이미 `procsrv_pid`에 캐시돼 있어(M32) `getpid()`가 순수
로컬 반환이라 스케줄러를 전혀 건드리지 않았기 때문이다(20ms
타임슬라이스 안에서 부모가 자식에게 양보할 일이 없었다).

새 syscall `sys_yield`(`MC_SYSCALL_YIELD` 22, `mc/syscall.h`)를
추가해 `kern::sched::yield()`를 유저랜드에 그대로 노출했다 —
musl의 `sched_yield()`(`SYS_sched_yield`=24)가 `libc/sysdeps/
minicore/syscall_shim.c`를 거쳐 이 자리로 우회한다. musl-hello의
부모는 이제 `mc_signal_send()`를 최대 200회 반복하며 매번
`sched_yield()`로 실제로 양보한다 — 자식의 `sigaction()` 자체는
매우 빨라, 그 창 안에서 반드시 한 번은 등록 이후에 도착한다.

## 검증

x86_64 전체 재빌드 성공. QEMU에서 확인: "musl signal handler
ok=1". QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 107) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0) |

## 남겨 둔 것

real-libc-syscall-layer.md M37(pthread 최소— `sys_thread_create`+
`sys_futex`)부터 계속 진행한다. IRETQ 경로 시그널 확인, `SIGCHLD`
자동 전달, `SIGKILL` 즉시 unlink(재오픈된 OPEN-65), 실시간 시그널,
`sigprocmask`의 실제 마스크 변경, job control은 모두 범위 밖으로
남는다. `sys_yield`는 M37의 futex 대기/깨우기 설계에도 재사용할 수
있는 일반 프리미티브다.
