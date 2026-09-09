# 완료 보고: general-purpose-completion M22 — 프로세스 생명주기(wait/kill 최소 구현)

**대상 계획**: [general-purpose-completion.md](../plan/general-purpose-completion.md) §M22
**관련 결정**: [kernel-scheduler.md](../design/kernel-scheduler.md) ADR-178
(sys_process_kill 스케줄러 픽 타임 폐기 + procsrv wait 자기 전용 endpoint 패턴)
**실행일**: 2026-09-10

## 완료한 것

### D0. 선행 스코핑

[procsrv.md](../spec/procsrv.md) §2/§6이 정의한 완전한 프로세스
테이블(`process_entry`, pid/parent/children, `proc_op::wait`/`signal`의
실제 와이어 프로토콜)은 이번 라운드에도 구현하지 않는다 — M17~M20이
반복해 온 "이번 라운드는 좁힌다" 패턴 그대로, "자식을 강제 종료할 수
있는가"와 "자식의 종료 코드를 회수할 수 있는가" 두 가지 메커니즘만
증명한다. 자세한 근거는 ADR-178(kernel-scheduler.md) 참고.

### D1. 커널 — `sys_process_kill`(강제 종료, 스케줄러 픽 타임 폐기)

- [kernel_objects.hpp](../../kernel/core/object/kernel_objects.hpp)에
  `object::thread::kill_requested`(bool)와 `object::k_right_can_kill`
  (`object_kind::thread` 핸들 전용 권한)을 추가했다.
- [scheduler.cpp](../../kernel/core/sched/scheduler.cpp)에
  `sched::request_kill(thread&)`(플래그만 세운다)와
  `pick_next_alive()`(`pick_next_with_stealing()`을 감싸, 고른
  스레드가 `kill_requested`면 스케줄하지 않고 그 자리에서
  폐기 — `arch_fpu_thread_exiting()`으로 FPU 소유권 기록만 정리하고
  klog로 `"[sched] thread killed (discarded before scheduling)"`를
  남긴다)를 추가하고, `start()`/`yield()`/`block()`/`exit()`가 기존
  `pick_next_with_stealing()` 호출을 전부 이걸로 바꿨다. `yield()`가
  새로 `pick_next_alive()`에서 `nullptr`을 받을 수 있는 경우(자기
  자신이 kill 대상이자 유일한 runnable 스레드)를 `arch_idle_halt()`
  로 처리하도록 함께 고쳤다.
- [process_ops.hpp/.cpp](../../kernel/arch/x86_64/process_ops.hpp)의
  `process_spawn()`이 성공하면 `process_spawn_request::out_thread_handle`
  에 새 스레드를 가리키는 `k_right_can_kill`만 가진 소유 핸들을
  **호출자 자신의** handle_table에 채운다(`out_endpoint_proxy_handle`과
  같은 자리). 새 `process_kill()` 함수가
  `ipc/endpoint.cpp::resolve_endpoint()`와 정확히 같은 `debug_entry()`
  기반 핸들 검사 패턴으로 핸들을 검증하고 `sched::request_kill()`을
  부른다.
- 새 syscall `sys_process_kill`(번호 12,
  [uapi.hpp](../../kernel/include/uapi.hpp)) —
  [syscall.cpp](../../kernel/arch/x86_64/syscall.cpp)에 라우팅을
  추가했다.

### D2. procsrv 자기테스트 — wait/kill 왕복 검증

[servers/procsrv/main.cpp](../../servers/procsrv/main.cpp)에 su-target과
같은 관례(procsrv 자신의 재조립 ELF를 다른 argv 마커로 다시 스폰)로
`wait_target_argv`/`kill_target_argv`를 추가했다:

- **wait 타깃**: procsrv가 `create_endpoint=true`로 스폰해 **자기
  전용** endpoint를 받은 뒤(`out_endpoint_proxy_handle`), procsrv가
  그 endpoint로 Call하면 타깃이 Reply의 `regs[0]`에 자기 exit
  code(42)를 담아 돌려준다. procsrv는 그 값이 42와 일치하는지
  확인한다(`"[procsrv] wait exit_code ok=1"`).
- **kill 타깃**: 아무 handle도 물려받지 않고 유한 반복(2억 회 nop,
  M21의 "무한 루프가 나머지 부팅을 굶긴다" 실수를 반복하지 않기
  위한 방어) 후 스스로도 끝나지만, procsrv가 `out_thread_handle`로
  `sys_process_kill`을 호출해 강제 종료를 요청하고 그 결과를 확인한다
  (`"[procsrv] kill requested ok=1"`).

**실제로 겪은 버그(설계 방향 전환)**: 처음에는 wait 타깃이 procsrv의
**기존 로그인용 공유 endpoint**(handle 1)에 Call로 exit code를
"먼저 통지"하는 설계였다. QEMU 실측 결과 servers/login이 procsrv의
자기테스트 진행과 무관하게 이미 그 endpoint에 `OP_LOGIN` Call을
걸어 두고 있어서, procsrv의 `sys_recv`가 login의 메시지를 가로채
버렸다(login은 텅 빈 응답을 상태 코드 0="성공"으로 오인해 겉으로는
문제없이 지나갔지만, wait 자기테스트는 login의 페이로드를 exit
code로 오인해 실패로 관찰됐다). 방향을 뒤집어(procsrv가 자식 전용
새 endpoint로 먼저 묻고 자식이 Reply하는 구조) 완전히 해결했다 —
자세한 원인 분석은 ADR-178 참고.

### D3. 검증

`tools/smoke-test-x86_64.sh`에 세 문자열을 추가했다:
`"[procsrv] wait exit_code ok=1"`, `"[procsrv] kill requested ok=1"`,
`"[sched] thread killed (discarded before scheduling)"`. 4개 QEMU
스위트 전부 재확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0개) |

**작업 중 겪은 별도 함정(빌드 시스템)**: `servers/bootdisk.img`는
`add_custom_target`으로만 정의돼 있어 기본 `cmake --build`(타깃 미지정)
에 포함되지 않는다 — procsrv를 고치고 일반 빌드만 돌린 뒤 QEMU로
확인하면 **이전 버전의 procsrv가 담긴 stale한 bootdisk.img**를 계속
부팅하게 된다(실제로 처음 겪음 — 새 self-test 로그가 전혀 안 보여서
원인을 추적하다 발견). `servers/*`(또는 `userland/*`) 아래 코드를
바꿀 때는 반드시
`cmake --build <빌드 디렉토리> --target minicore_bootdisk_image`를
추가로 돌려야 한다.

## 남겨 둔 것 (OPEN)

- **OPEN-64**: 실제 프로세스 테이블(pid 발급, parent/children)과
  외부에서 부를 수 있는 `OP_WAIT`/`OP_KILL` IPC 프로토콜은 여전히
  없다 — procsrv 자기 자신을 대상으로 하는 자기테스트로만 메커니즘을
  증명했다.
- **OPEN-65**: kill은 대상이 IPC 대기열에 갇혀 있으면 다시 깨어나야만
  폐기된다(즉시 unlink하지 않는다).

둘 다 [open-items.md](../design/open-items.md)에 기록했다.

## 다음

[general-purpose-completion.md](../plan/general-purpose-completion.md)
§M23(진짜 fork/exec — procsrv가 fd 진실 공급원 역할을 실제로 수행)로
이어간다.
