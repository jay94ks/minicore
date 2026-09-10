# M53 완료 — 로그인 후 셸을 minicore 네이티브(ADR-170) 대신 포팅된 msh로 교체

[musl-userland-porting.md](../plan/musl-userland-porting.md) §M53.
관련 ADR: [ADR-224](../design/security-model.md)(ADR-171 대체).

## 무엇을 했는가

계획 원안은 "OP_START가 깨우는 대상 ELF만 BusyBox 정적 바이너리로
바꾼다"였다 — BusyBox가 M52에서 이미 철회됐으므로 실제 대상은
`userland/msh`(M52)다. 착수하며 확인한 것: `msh`는 BusyBox와 달리
"부팅 시 이미 떠서 자기 handle 1에서 블록 중인 프로세스를 OP_START
로 깨운다"는 기존 ADR-171의 모델과 근본적으로 안 맞는다 — `msh`는
실제 musl 프로그램이라 매번 새로 `fork()`+`execve()`돼야 한다.
그래서 OP_START 핑을 그대로 유지한 채 대상만 바꾸는 게 아니라,
**procsrv가 로그인 성공 시 `msh`를 실제로 `sys_process_spawn`한다**
로 메커니즘 자체를 바꿨다:

- **`servers/procsrv/main.cpp::start_session_once()`** — 고정
  handle(4, 예전 `userland/shell`의 handle)에 IPC를 보내는 대신,
  `mc_process_spawn_request`를 채워 `sys_process_spawn`으로 `msh`를
  스폰한다. `elf_data`/`elf_size`는 procsrv 자신의 컴파일 시점
  데이터(`tools/bin2c.py`로 심은 `g_msh_elf`, echo/ls/cat과 같은
  배선)이고 `linux_abi_stack=1`(musl의 `crt_arch.h` 요구, M28).
  핸들 상속 순서(`inherited_handles[0]=procsrv 자신의 vfs 핸들`,
  `[1]=procsrv 자신의 수신 endpoint`)를 msh가 예전에
  `--depends=msh:vfs,procsrv`로 initrun에게서 받던 것과 정확히
  맞춰, `syscall_shim.c`의 고정 핸들 관례(`MC_VFS_HANDLE=2`,
  `MC_PROCSRV_HANDLE=3`)가 그대로 성립한다.
- **`userland/shell` 완전 제거** — 더 이상 아무도 부르지 않는다
  (디렉터리 자체를 지웠다: `CMakeLists.txt`/`link.ld`/`main.c`).
- **`msh`도 더 이상 부팅 시점 서비스가 아니다** — `servers/
  CMakeLists.txt`에서 `--service=msh=`/`--depends=msh:...`/
  `--linux-abi-stack=msh`를 모두 제거했다. procsrv의 스폰이 유일한
  진입점이다.

## 실행 중 발견

**ADR-171의 전제가 이미 M32에서 무효화돼 있었다.** ADR-171(M20)은
"임의 경로의 ELF를 다른 프로세스에 전달하는 일반 메커니즘이 없다"
는 근거로 OP_START 핑 단순화를 정당화했다 — 그런데 M32
(real-libc-syscall-layer.md §M32)가 만든 "procsrv가 컴파일 시점
데이터로 심은 ELF를 `sys_process_spawn`으로 스폰"이 정확히 그
메커니즘이다. M32~M52 사이 세 마일스톤이 지나는 동안 아무도 이
연결을 만들지 않았다 — M53 착수 전 계획을 다시 읽으며 뒤늦게
확인했다. 실제 구현은 `spawn_su_target()`(M18)/M27 wait-target
자기테스트가 이미 쓰던 "procsrv 자신의 handle을
`inherited_handles`로 넘긴다" 패턴을 그대로 재사용했을 뿐, 새
커널/IPC 기능은 전혀 필요 없었다 — 설계가 아니라 "이미 있는 것을
연결하는 것"이 이번 라운드의 전부였다.

## 검증 (QEMU, x86_64)

핵심 확인 로그(순서대로):

```
[login] no keyboard input, using self-test account
[process] spawn ok entry=0x10004d50 trusted=0
[procsrv] shell session start ok=1
[msh] no keyboard input, running self-test commands
[msh] running: echo hello msh
[msh] running: ls
[msh] running: cat /bin/echo
[msh] self-test done ok=1
```

`entry=0x10004d50`이 msh의 실제 ELF 엔트리 포인트와 일치함을
확인했다 — 로그인 성공이 실제로 msh를 새 프로세스로 스폰했다는
직접 증거다. `tools/smoke-test-x86_64.sh`에서 `[shell] ...`
어서션 6개를 제거(`"[procsrv] shell session start ok=1"`만 남는다
— 이제 msh 스폰을 가리킨다)하고 스위트(139개) PASS. msh 자신의
M52 어서션은 트리거만 바뀌었을 뿐(부팅 시점 서비스 → 로그인 시점
스폰) 그대로 재확인했다. `tools/smoke-test-net-x86_64.sh`도 같은
이유로 `"[shell] self-test done"`을 `"[msh] self-test done ok=1"`
로 교체 — 이 스위트가 처음엔 옛 마커를 그대로 찾다 FAIL했다가
고친 뒤 PASS로 확인했다(SMP/NUMA/AVX 3개 스위트는 shell 마커를
쓰지 않아 애초에 영향 없었다). 5개 회귀 스위트 전부 PASS(exit 0,
FAIL 없음): 스모크 139, SMP 11, NUMA 24, AVX 12, net 6.

## 범위 밖으로 남긴 것 (계획 문서가 이미 명시)

- `user_account.session_program` 필드, 계정별 셸 재정의(ADR-089
  §결정1/4) — 여전히 모든 로그인이 같은 단일 `msh` 스폰으로
  이어진다. 세션 하나만 다루는 기존 정적 플래그 가드도 그대로다.
- `isatty`/`tcgetattr`/`getenv` 같은 대화형 셸 완성도(계획이 사전
  진단으로 이미 경고해 둔 것) — msh는 여전히 키보드 입력 없이
  고정 자기테스트 명령줄만 실행한다.

## 다음

M54 — 파이프라인/리다이렉션 실제 사용(M51+M53 통합 검증, 새 구현
없음).
