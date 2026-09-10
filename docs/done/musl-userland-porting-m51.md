# M51 완료 — 익명 파이프(pipe()/pipe2()) + dup2()

[musl-userland-porting.md](../plan/musl-userland-porting.md) §M51.
관련 ADR: [ADR-220](../design/foundations.md)(새 서버+재시도 폴링
전략).

## 무엇을 했는가

`pipe()`/`pipe2()`/`dup2()`를 실제로 동작하게 만들었다 — 셸의
`|`(파이프라인)와 리다이렉션이 요구하는 최소 IPC 프리미티브다
(M53~M54의 전제 조건).

- **새 서버 `servers/pipesrv`**(`mc/pipesrv_protocol.h`, ADR-195
  마크업+`tools/gen-wire-docs.py`로 [docs/spec/generated/pipesrv-wire.md](../spec/generated/pipesrv-wire.md)
  추출) — procsrv/cfgsrv/svcmgr와 같은 순수 minicore 네이티브 서버,
  단일 요청-응답 루프. 파이프당 고정 4096바이트 원형 버퍼, 최대
  8개(YAGNI). id는 프로토콜-레벨 정수(진짜 커널 핸들 아님)이고
  읽기/쓰기 쪽 각각 참조 카운트를 둔다.
- **절대 회신을 미루지 않는다**(ADR-220 §결정2) — 파이프가 비었거나
  가득 찼으면 즉시 `WOULD_BLOCK`을 돌려주고, 호출자
  (`libc/sysdeps/minicore/syscall_shim.c`)가 `mc_yield()`+재시도로
  블로킹을 흉내낸다(`mc_wait()`가 이미 쓰는 것과 같은 요령,
  OPEN-67과 같은 이유 — pipesrv도 단일 스레드라 회신을 붙들면 다른
  클라이언트를 전혀 처리할 수 없다).
- **참조 카운트+명시적 `op_dup`**(ADR-220 §결정3) — `fork()`/`dup2()`
  로 같은 파이프 id를 여러 프로세스(또는 한 프로세스의 fd 슬롯
  여러 개)가 들고 있을 수 있는데, pipesrv는 이런 복제를 스스로
  관찰할 수 없다 — `syscall_shim.c`가 그 시점마다 명시적으로
  `op_dup`을 불러 참조 카운트를 알린다. 참조 카운트가 0이 되는
  쪽이 진짜로 닫힌 것이다 — 쓰기 쪽이 0이면 read는 진짜 EOF
  (status=OK, len=0), 읽기 쪽이 0이면 write는 `BROKEN_PIPE`.
- **새 유저랜드 fd 표**(`syscall_shim.c::g_pipe_fds[]`) — 기존 VFS
  파일 표(`g_open_files[]`, "fd-3" 오프셋 관례)와 달리 fd 번호로
  직접 인덱싱한다(fd 0/1/2도 `dup2()`로 덮어씌워야 하므로). 새 fd를
  할당할 때 두 표 모두와 충돌하지 않는지 확인하는 `fd_is_free()`를
  추가했다.
- **새 실제 musl 프로그램 `userland/pipe-test`** — 세 시나리오를
  확인: (1) 같은 프로세스 안에서 write→close(쓰기 쪽)→read→EOF
  (2) `fork()`로 파이프 양끝을 부모/자식이 나눠 가진 뒤(각자 안
  쓰는 쪽을 닫는 표준 관례) 부모가 쓰고 자식이 읽는 왕복 (3)
  `dup2()`로 파이프 읽기 쪽을 fd 0(stdin)에 덮어씌운 뒤 fd 0을
  직접 읽어도 파이프 데이터를 받음.

## 실행 중 발견

**x86_64가 실제로는 레거시 `SYS_pipe`(22)도 갖고 있었다** — i386
전용이라고 잘못 가정했다. musl의 `third_party/musl/src/unistd/pipe.c`
가 `#ifdef SYS_pipe`를 참으로 평가해, `pipe()`가 계획 문서가 미리
준비해 둔 `SYS_pipe2`(293)가 아니라 `SYS_pipe`(22)로 왔다 — 처음엔
그대로 `-ENOSYS`로 떨어졌다. 둘 다(`SYS_pipe`/`SYS_pipe2`) 같은
핸들러로 처리하도록 케이스를 합쳐 해결했다(`b`=flags 인자는
`SYS_pipe`로 왔을 때 무의미하지만 무시해도 안전하다).

그 외 실행 중 발견한 작은 것들:
- `read()`/`close()`의 공개 POSIX 래퍼(`third_party/musl/src/unistd/
  {read,close}.c`) 자체가 지금까지 링크된 적이 없었다 — 이전 라운드
  (M31 등)는 항상 `readv`/`fread` 내부 경로만 탔다. pipe-test가 이
  래퍼들을 직접 부르는 첫 소비자라 `libc/CMakeLists.txt`에 추가했다.
- `open_common()`의 빈 fd 슬롯 탐색이 원래 `g_open_files[]` 안에서만
  빈 슬롯을 찾아, 그 fd 번호를 파이프가 이미 쓰고 있어도 몰랐다 —
  `fd_is_free()`로 두 표를 모두 확인하도록 고쳤다(파이프를 먼저
  만든 뒤 VFS 파일을 여는 순서에서 실제로 충돌할 수 있는 경로였다).

## 검증 (QEMU, x86_64)

핵심 확인 로그:

```
[pipe-test] single-process write/read/eof ok=1
[pipe-test] fork pipe roundtrip ok=1
[pipe-test] dup2 stdin ok=1
[pipe-test] all ok=1
```

`tools/smoke-test-x86_64.sh`에 M51 어서션 4개 추가, 전체 스모크
스위트(139개) PASS(exit 0). 추가로 SMP/NUMA/AVX/net 4개 회귀
스위트 전부 PASS(exit 0, FAIL 없음) — 새 서버+`syscall_shim.c`
확장이 다른 IPC/musl 소비자에 회귀를 만들지 않았음을 확인했다.

## 범위 밖으로 남긴 것 (계획 문서가 이미 명시)

- named pipe(FIFO), 소켓.
- `O_CLOEXEC`/`O_NONBLOCK` 플래그(YAGNI — exec()가 아직 fd를 전혀
  물려주지 않으므로 CLOEXEC는 의미가 없다).
- `dup2()`는 파이프 fd만 대상으로 지원한다 — VFS 파일 fd의 dup2는
  이번 범위 밖(OPEN-64와 맞물린 별도 작업).
- 여러 pipesrv 클라이언트가 **동시에** 서로 다른 파이프에서 블로킹
  대기하는 경우, pipesrv 자신은 단일 스레드라 한 클라이언트의
  요청을 처리하는 동안(비블로킹이라 매 요청이 빠르지만) 다른
  클라이언트를 동시에 처리하지 못한다 — M51의 검증 시나리오(단일
  생산자/단일 소비자)에는 문제가 없지만, M54의 실제 셸 파이프라인
  사용(여러 단계가 동시에 서로 다른 파이프를 오가는 경우)에서
  재검토가 필요할 수 있다.

## 다음

M52 — BusyBox를 `third_party/`에 submodule로 추가하고 정적으로
musl에 링크한다.
