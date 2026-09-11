# M55 완료 (스트레치) — job control 최소: msh가 자기 자식에게 SIGINT를 전달

[musl-userland-porting.md](../plan/musl-userland-porting.md) §M55.
관련 ADR: [ADR-226](../design/kernel-scheduler.md),
[ADR-227](../design/security-model.md).

## 설계가 원안을 좁혔다

계획 원문("`setpgid()`/`getpgid()`(`SYS_setpgid`/`SYS_getpgid`)"으로
**procsrv**가 프로세스 그룹을 관리하고 포그라운드 그룹에 시그널을
라우팅한다)을 실행 전 설계 검토(사용자 지시 — "OPEN 항목을 검토해
설계 계획부터 작성") 중에 깨졌다. `servers/procsrv/main.cpp`의
`process_entry.thread_handle`은 procsrv가 **직접** 스폰한 프로세스
(msh 자신)에만 유효하고, `mc_fork()`로 등록된 자식(msh의 파이프라인
단계 전부)은 항상 `thread_handle=0`("모름")으로 등록된다 — procsrv는
msh의 자식에게 시그널을 보낼 방법이 원천적으로 없다. 그래서
procsrv에 `pgid`/`setpgid`/`getpgid`를 전혀 추가하지 않고, "그룹"의
실제 주체를 msh 자신으로 옮겼다(자세한 근거는 ADR-227).

## 무엇을 했는가

- **[kernel/arch/x86_64/signal.cpp](../../kernel/arch/x86_64/signal.cpp)**
  (ADR-226): `check_signal_delivery()`의 `SIG_DFL`(핸들러 없음)
  분기를 쪼갰다 — 시그널 번호가 `SIGINT`(2)면 `kern::sched::
  request_kill(*self)`(`sys_process_kill`이 이미 쓰는 것과 같은
  비동기 종료 요청)를 그 자리에서 직접 부른다. 나머지 31개 시그널은
  ADR-211 그대로(무시) — OPEN-75로 그 gap을 등록했다.
- **새 procsrv 오퍼레이션 `MC_PROC_OP_REPORT_SIGNALED`**(label=18,
  `mc/procsrv_protocol.h`) — 호출자(msh)가 자기 자식에게 이미
  `mc_signal_send()`로 직접 시그널을 보낸 뒤 그 사실만 알린다.
  `handle_proc_kill`과 달리 `MC_SYSCALL_PROCESS_KILL`을 다시 시도
  하지 않는다(msh의 자식은 항상 `thread_handle=0`이라 어차피
  무효 핸들로 no-op된다) — 소유권 검사(`target->parent_pid==
  caller_pid`) 후 곧바로 `zombie`+`exit_code=-시그널번호`로 낙관적
  마킹한다(`MC_PROC_OP_KILL`이 이미 `exit_code=-9`로 SIGKILL을
  인코딩하는 것과 같은 관례). 이게 없으면 `mc_wait()`가 이 자식이
  죽었다는 걸 전혀 못 배워 200,000회 폴링 예산을 다 태운다.
- **`libs/mc/include/mc/procsrv_client.h`/`.c`**: `mc_report_signaled()`
  클라이언트 래퍼. **`syscall_shim.c`**: msh 전용 얇은 래퍼
  `mc_shell_report_signaled()`(msh가 procsrv의 핸들 번호를 몰라도
  되게, `mc_shell_bind_pipe_fd` 등과 같은 원칙).
- **새 최소 유저 프로그램 `userland/loop-test`** — 시작 즉시
  `"[loop-test] starting\n"`을 찍고 `sched_yield()`를 500만 회
  반복하다가 전부 마치면 `"[loop-test] finished without
  interruption\n"`을 찍는다(이 줄이 로그에 나타나면 이 마일스톤은
  실패다). `servers/procsrv`가 echo/ls/cat과 같은 방식으로 부팅
  시점에 `/bin/loop-test`로 VFS에 심는다.
- **`userland/msh`** — 전용 함수 `run_job_control_test()`를 추가해
  `spawn_stage()`를 재사용해 `loop-test`를 fork+exec한 뒤,
  `mc_last_fork_child_thread_handle()`로 얻은 자식의 시그널 가능
  handle에 `mc_signal_send(SIGINT)`를 직접 부르고
  `mc_shell_report_signaled()`로 procsrv에 알린 뒤 `waitpid()`한다.
  self-test 마지막 단계로 실행되고, 그 뒤에도 `"[msh] self-test
  done ok=1"`이 정상적으로 찍힌다는 사실 자체가 "셸 자신은
  살아남는다"는 목표의 증명이다.

## 실행 중 발견

**`execve()`도 syscall 리턴 시점 시그널 확인을 거친다는 것**(ADR-211
§결정3 — 이 결정이 새로 바꾼 동작은 아니다, 원래부터 그랬다는 것을
이번에 처음 실제로 부딪혔다). msh가 자식을 `fork()`한 직후 곧바로
`SIGINT`를 보내니, 그 자식이 `"/bin/loop-test"`의 `execve()` 자체를
마치고 돌아오는 순간 바로 ADR-226의 새 분기가 그 신호를 소비해
버려, `loop-test`의 `main()`이 단 한 줄도 실행되기 전에 죽었다 —
1차 QEMU 실행에서 `"[msh] job control: child interrupted ok=1"`은
찍혔지만 `"[loop-test] starting"`이 로그에 전혀 없어 발견했다(자기
테스트 자체는 "죽었다"는 것만 확인해서 겉보기엔 통과했지만, "실행
중인 자식을 끊는다"는 진짜 목표는 증명하지 못한 셈이었다). msh가
신호를 보내기 전에 `sched_yield()`를 50회 돌려 자식이 실제로 몇
차례 스케줄돼 자기 루프에 들어갈 시간을 준 것으로 고쳤다 — 이후
`"[loop-test] starting"`은 찍히고 `"finished without interruption"`
은 끝까지 안 찍히는 것으로 진짜 목표를 확인했다.

부수적으로, 부팅 시점에 VFS에 새 파일(`/bin/loop-test`)을 하나 더
심게 되면서 M54의 출력 리다이렉션 자기테스트가 여는 파일의
`open_file_id`가 36→37로 밀렸다 — `tools/smoke-test-x86_64.sh`의
그 어서션(`"[msh] running: echo @filefd 1 10 36 ..."`)이 잠깐
FAIL했다가 37로 갱신해 고쳤다(VFS에 파일을 더 심는 어떤 변경도
이런 하드코딩을 깨뜨릴 수 있다는 구조적 취약점 — 근본 수정은
범위 밖으로 남긴다).

## 검증 (QEMU, x86_64)

핵심 확인 로그:

```
[msh] running: loop-test
[loop-test] starting
[sched] thread killed (discarded before scheduling)
[msh] job control: child interrupted ok=1
[msh] self-test done ok=1
```

`"[sched] thread killed (discarded before scheduling)"`(M22/ADR-178
부터 있던 기존 로그, `pick_next_alive()`)이 실제로 찍힌 것은 커널이
그 스레드를 진짜로 폐기했다는 독립적인 증거다(procsrv의 낙관적
북키핑과 무관하게) — 그리고 `"finished without interruption"`이
로그 전체에 한 번도 나타나지 않는 것을 직접 확인했다(자동화된
어서션은 부재를 못 잡으므로 이 확인은 로그 직접 검사로 했다).
`tools/smoke-test-x86_64.sh`에 M55 어서션 5개 추가(`"loop-test"`
실행/시작 로그, job control 성공, 새 `loop-test seed` 확인), 기존
M54 리다이렉션 어서션 하나 값 갱신. 5개 회귀 스위트 전부 PASS
(exit 0, FAIL 없음): 스모크 147, SMP 11, NUMA 24, AVX 12, net 6.

## 범위 밖으로 남긴 것 (계획 문서가 이미 명시, ADR-227이 재확인)

- 실제 PS/2 키보드에서 Ctrl-C 스캔코드를 감지해 이 경로를
  트리거하는 것 — 콘솔 드라이버는 여전히 출력 전용이고, 키 입력을
  읽는 유일한 소비자는 `login`뿐이다(musl 프로그램의
  `read(0, ...)`을 통하지 않는다). 이번 라운드는 msh 자신이
  "Ctrl-C가 눌렸다"를 시뮬레이션했다. **OPEN-76**으로 등록.
- `SIGTERM`/`SIGQUIT`/`SIGHUP` 등 다른 Term류 시그널의 기본 동작.
  **OPEN-75**로 등록.
- `SIGTSTP`/`SIGCONT`(bg/fg 전환), `tcsetpgrp`/`tcgetpgrp`(제어
  터미널) — 완전한 job control.

## 다음

`musl-userland-porting.md`(M51~M55)가 이제 전부 완료됐다 — 이
계획에는 더 이상 다음 마일스톤이 없다.
