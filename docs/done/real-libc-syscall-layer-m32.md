# 완료 보고: real-libc-syscall-layer M32 — 프로세스 syscall(fork/execve/wait4/getpid) 실왕복

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M32
**관련 결정**: [security-model.md](../design/security-model.md) ADR-201, ADR-206;
[kernel-memory.md](../design/kernel-memory.md) ADR-183, ADR-207
**실행일**: 2026-09-10

## 완료한 것

1. **procsrv 프로토콜 확장** — `mc/procsrv_protocol.h`에 `self_register`
   (label=13, 요청 없음, 응답=신규 pid)/`fork_register`(label=14,
   요청=caller_pid, 응답=신규 child_pid) 두 오퍼레이션을 추가했다.
   `servers/procsrv/main.cpp`에 `handle_proc_self_register`/
   `handle_proc_fork_register`를 구현했다 — M27의 "procsrv 자신이
   직접 스폰한 프로세스 사이에서만 동작"이라던 제약을 initrun이
   스폰한 일반 프로세스(musl-hello)까지 확장한다(ADR-206).
2. **libmc 클라이언트 신규**(ADR-183 §결정4가 요구하는 "새 syscall은
   먼저 libmc 얇은 래퍼로") — `libs/mc/include/mc/procsrv_client.h`/
   `src/ipc/procsrv_client.c`에 `mc_getpid`/`mc_fork`/`mc_wait`/
   `mc_process_exit_report`를 추가했다. `mc_fork()`는 실제
   `sys_fork`(`MC_SYSCALL_FORK`) **이전에** `fork_register`로 자식의
   pid를 미리 확정받는다 — 그 값을 담은 지역변수가 `sys_fork`의 COW
   복제로 부모/자식 스택에 그대로 남으므로, procsrv와 추가 IPC
   없이 부모는 그 값을 반환하고 자식은 자신의 pid로 저장하기만
   하면 된다. `mc_wait()`는 M27의 논블로킹 폴링 `OP_WAIT`를 최대
   200000회까지 재시도한다(procsrv 자기테스트의 64회 재시도 패턴과
   같은 요령, 자식의 execve() 이후 실제 작업 시간을 감안해 한도를
   늘렸다).
3. **pid를 커널 스레드 필드에 저장**(ADR-206) —
   `kern::object::thread::procsrv_pid`(신규 필드)+
   `MC_SYSCALL_PROCSRV_PID_GET`/`SET`(신규 syscall, `mc_procsrv_pid_get/set`).
   유저랜드 static 변수가 아니라 커널 스레드 객체에 캐시해야 하는
   이유는 아래 "실행 중 발견" 참고.
4. **`syscall_shim.c`에 `SYS_fork`/`SYS_getpid`/`SYS_execve`/
   `SYS_wait4` 추가** — 각각 위 libmc 함수만 호출한다(ADR-183
   §결정4). `SYS_wait4`는 `pid>0`(특정 자식)만 지원한다 — `pid<=0`
   ("임의의 자식"/프로세스 그룹)은 이 라운드 범위 밖이다(계획
   문서가 이미 비슷한 정신으로 fork/clone 변형을 좁혀 뒀다,
   2026-09-10 사용자 확인). `SYS_execve`는 `mc_vfs_open`+
   `mc_fs_read_all`(둘 다 이미 있는 libmc 함수, M13/M31)로 대상
   ELF를 통째로 읽어 새 `mc_exec()`(신규 얇은 syscall 래퍼)를
   호출한다.
5. **`sys_exec`가 `linux_abi_stack`을 받도록 확장**(ADR-207) —
   `mc_exec_request`에 필드 추가, `exec_current()`가
   `build_process()`의 기존 M28 경로(스폰과 완전히 같은 코드)로
   그대로 전달한다. M28 시점엔 "exec()은 이번 라운드에 지원하지
   않는다"로 미뤄 뒀던 부분이다.
6. **`fork_current()`가 `fs_base`(TLS)도 물려주도록 수정**(ADR-207,
   버그 수정) — 아래 "실행 중 발견" 참고.
7. **musl-hello 확장** — `getpid()`(procsrv 등록 확인)+`fork()`+
   자식이 `execve("musl-exec-target.elf", ...)`로 **다른** 실행
   이미지를 실행+부모가 `waitpid()`로 그 exit code(42)를 회수하는
   전체 왕복을 검증한다.
8. **`userland/musl-exec-target`(신규)** — musl-hello가 exec할
   대상. `write()`/`_exit()`만 쓴다(printf는 쓰지 않는다 — 아래
   "실행 중 발견" 참고). `servers/procsrv/CMakeLists.txt`가
   `tools/bin2c.py`(신규)로 이 타깃의 컴파일된 ELF를 C++ 헤더로
   구워 procsrv 자신의 소스에 심고, procsrv가 부팅 시
   `run_exec_target_seed()`(신규, `write_elf_to_vfs` 재사용 — M18의
   su-target 헬퍼를 그대로 씀)로 VFS(memfs)에 미리 써 둔다.
   musl-hello는 `--depends=musl-hello:vfs,procsrv`로 procsrv
   endpoint도 물려받는다(핸들 3, `MC_PROCSRV_HANDLE`).

## 계획 대비 범위 조정

- 계획 원문은 "다른 실행 이미지(동적 링크된 별도 바이너리)를
  실행시키고... execve()가 자식 안에서 다시 PT_INTERP 로더 경로를
  타는 것도 함께 확인한다"고 적어 뒀다. 하지만 M29가 이미 ADR-203
  으로 정적 링킹 기준선에 되돌아가 있다(동적 링킹/PT_INTERP는 이
  프로젝트가 아직 실제로 완주하지 않은 채 보류된 상태) — 이 라운드는
  ADR-203의 기준선을 그대로 따라 musl-exec-target도 **정적으로
  링크된** 별도 바이너리로만 확인했다. PT_INTERP 경로 검증은 여전히
  범위 밖이다(동적 링킹을 다시 시도하는 시점까지).
- musl의 진짜 `fork()`가 x86_64에서 `SYS_clone`이 아니라 `SYS_fork`
  를 직접 쓴다는 것을 확인했다(`third_party/musl/src/process/_Fork.c`
  의 `#ifdef SYS_fork` 분기가 이 아키텍처에서 항상 참). 계획이 대비해
  둔 "SYS_clone, flags==SIGCHLD만 지원" 케이스는 이 아키텍처에서
  애초에 밟히지 않는다.
- `SYS_wait4(pid<=0, ...)`("임의의 자식")은 지원하지 않는다 — 이
  라운드의 시나리오는 항상 특정 자식 pid를 아는 상태이므로,
  musl-hello의 테스트는 (musl의 plain `wait()`가 아니라)
  `waitpid(child_pid, ...)`를 명시적으로 쓴다.
- `execve()`의 `argv`/`envp` 인자는 무시된다 — M28이 이미 Linux ABI
  초기 스택의 argc/argv를 고정값(`argc=1, argv[0]="/bin/musl-hello"`)
  으로 못박아 뒀기 때문에(`build_process()`), 실제 인자 전달은 이
  커널에 애초에 없다. 이건 M32가 새로 만든 단순화가 아니라 M28이
  이미 받아들인 것을 그대로 물려받는 것뿐이다.

## 실행 중 발견

1. **pid를 유저랜드 static에 캐시하면 execve()로 사라진다.** 처음
   구현은 `procsrv_client.c`에 `static uint32_t g_mc_self_pid`로
   pid를 캐시했다 — musl-hello의 fork() 자식이 곧바로 execve()로
   musl-exec-target 이미지로 바뀌자, 그 새 이미지의 BSS/데이터가
   전부 새로 초기화돼(`g_mc_self_pid=0`) 자기 pid를 잃어버렸다.
   `_exit(42)`가 procsrv에 아무 보고도 못 해 부모의 `waitpid()`가
   `MC_PROC_STATUS_STILL_RUNNING`으로 영원히 남았다("musl
   fork+exec+wait ok=0"). **고침**: pid를 `kern::object::thread::
   procsrv_pid`(커널 스레드 필드)에 저장한다 — `sys_exec`가 같은
   스레드 객체를 재사용하고 `owner_space`만 바꾸므로(`exec_current()`),
   이 필드는 execve()를 거쳐도 살아남는다(ADR-206).
2. **`fork()`는 `fs_base`(TLS)도 물려줘야 한다 — 진짜 버그.**
   `fork_current()`(M23, ADR-179)는 `io_port_base`/`io_port_count`
   만 부모에게서 물려주고 있었다. musl-hello의 `fork()`를 처음
   QEMU로 돌리자, 자식이 exec() 전(여전히 부모와 같은 이미지를
   실행하는 동안) `_Fork()`의 `__post_Fork()`가 `__pthread_self()`
   (`%fs` 상대 읽기)를 호출하는 순간 페이지 폴트로 죽었다
   (`vector=14 error_code=0x5` — present+user+read, 이 커널의 저지대
   항등 매핑처럼 present이지만 supervisor 전용인 영역을 읽은 것과
   일치 — `create_forked_thread()`가 만드는 새 스레드 객체가
   `fs_base=0` 기본값으로 시작했기 때문). **고침**: `child->fs_base
   = self->fs_base;` 한 줄(ADR-207) — fork()는 주소공간 전체를
   COW로 복제하므로, 부모의 `fs_base`가 가리키는 TLS 블록도 자식
   쪽에 이미 유효한 COW 사본으로 존재해 값만 넘기면 된다. procsrv
   자신의 fork 자기테스트(M23/M27)는 TLS를 전혀 안 써서 이 간극이
   지금까지 드러나지 않았다.
3. **printf 하나가 memfs 파일 크기 상한을 두 번 건드렸다.**
   musl-exec-target을 처음엔 musl-hello처럼 `printf()`로 작성했다 —
   그러자 vfprintf.c의 전체 서식 처리 체인이 링크에 끌려와 이
   바이너리가 136608바이트가 됐고, procsrv 자신도 이 바이트를
   `tools/bin2c.py`로 통째로 품으면서 139072바이트로 커졌다. (a)
   musl-exec-target.elf 자체가 memfs의 파일 크기 상한
   (`k_max_file_bytes=131072`)을 넘어 VFS에 못 써졌고("[procsrv]
   musl-exec-target seed ok=0"), (b) procsrv 자신의 ELF도 같은
   상한을 넘어 M18의 기존 self-exec 왕복 테스트(`run_loader_test`,
   su-target 경로)까지 함께 실패했다("[procsrv] loader roundtrip
   ok=0" — 이번 라운드가 만든 새 회귀). **고침**: musl-exec-target을
   `write()`/`_exit()`만 쓰게 다시 써서(69552바이트로 줄어듦) 근본
   원인을 줄이고, `servers/fs/memfs::k_max_file_bytes`와 procsrv
   자신의 `k_max_reassembled_bytes`를 131072→262144로 함께 올려
   (M18이 su-target 때 4096→131072로 늘린 것과 같은 이유의 반복)
   양쪽에 여유를 뒀다.
4. **`syscall_shim.c`의 `SYS_wait4` 인자 위치 실수.** 처음 구현은
   `b`=pid, `c`=wstatus로 잘못 매핑했다(Linux ABI는
   `wait4(pid, wstatus, options, rusage)` — 첫 인자가 `a`, 즉 pid는
   `a`, wstatus는 `b`다). 첫 QEMU 실행에서 "musl fork+exec+wait
   ok=0"으로 드러나 고쳤다.

## 검증

x86_64 전체 재빌드 성공. bootdisk 재생성(musl-exec-target은
서비스로 패키징되지 않는다 — procsrv가 컴파일 시점에 심어 VFS에 써
두는 대상일 뿐). QEMU에서 확인(순서대로): "musl getpid ok=1" →
"[process] fork ok" → "musl fork ok=1" → "[process] exec ok" →
"hello from musl-exec-target (a different image)" → "musl
fork+exec+wait ok=1" → 이후 나머지 부팅(shell 자기테스트까지)이
평소와 동일하게 계속됨(회귀 없음, "[procsrv] loader roundtrip
ok=1"도 다시 통과). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) — 신규 확인 문자열(musl getpid/fork/fork+exec+wait, musl-exec-target seed/실행) 포함 |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M33(LAPIC 타이머 보정, OPEN-62)부터
계속 진행한다. OPEN-67(caller_pid 자기주장/비블로킹 폴링 wait)은
이번 라운드에도 해소되지 않는다(self_register/fork_register도
여전히 자기주장 모델이다) — user-service-manager.md 착수 시점에
다시 재검토 대상. `SYS_wait4(pid<=0)`("임의의 자식")·동적 링킹/
PT_INTERP exec 경로는 여전히 범위 밖이다.
