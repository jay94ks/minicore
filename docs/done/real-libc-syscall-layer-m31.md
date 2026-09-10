# 완료 보고: real-libc-syscall-layer M31 — 파일 I/O syscall + 실제 musl stdio

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M31
**관련 결정**: [kernel-memory.md](../design/kernel-memory.md) ADR-183, ADR-205
**실행일**: 2026-09-10

## 완료한 것

1. `libs/mc/include/mc/fs_client.h`/`.c`에 `mc_fs_read()`(신규) 추가
   — 기존 `mc_fs_read_all`(EOF까지 루프)과 달리 **단 한 번의**
   OP_READ만 보낸다(POSIX `read()`의 부분 읽기 의미론). 요청 크기를
   페이지 한도(4096) 안으로 캡해 서버가 실제로 읽은 바이트 수만큼만
   반환한다 — `mc_fs_read_all`처럼 "서버가 더 읽었는데 일부만
   복사"해서 데이터를 잃어버릴 위험이 없다.
2. `libc/sysdeps/minicore/syscall_shim.c` 확장 — `SYS_open`/
   `SYS_openat`/`SYS_read`/`SYS_readv`/`SYS_close`(신규 fd 테이블,
   fd(int)↔{fs_handle, open_file_id} 대응, 이 프로세스 로컬 상태 —
   fork 상속 없음, procsrv.md §3.6/OPEN-64가 이미 범위 밖으로 남긴
   "진짜 fd 진실 공급원"과 같은 정신)+`SYS_lseek`/`SYS_fstat`(스텁,
   항상 실패 — 이 순차 읽기 테스트는 요구하지 않음)+`SYS_ioctl`
   (스텁, TIOCGWINSZ 항상 실패 — musl stdio가 stdout을 완전
   버퍼링으로 판정하는 데 필요)+`SYS_writev`(기존 SYS_write를
   iovec으로 일반화, stdout/stderr만 지원).
3. `libc/CMakeLists.txt` — musl 자신의 진짜 stdio(읽기 경로: fopen/
   __fdopen/__fmodeflags, fread/__stdio_read/__toread/__uflow,
   fclose/__stdio_close/__stdio_seek. 출력 경로: stdout/stderr/
   stdin, __stdout_write/__stdio_write/__towrite/__overflow,
   printf/vfprintf. 공통: ofl/ofl_add, __stdio_exit)+그 전체
   링크 의존성(fflush/fwrite/isdigit/strerror/strnlen/wctomb/
   wcrtomb/__lctrans/__ctype_get_mb_cur_max+부동소수점 서식용
   frexpl/scalbn/__fpclassifyl/__signbitl)을 추가했다.
   `libc/sysdeps/minicore/lock_shim.c`에 `__lockfile`/`__unlockfile`
   no-op을 추가했다(M30의 `__lock`/`__unlock`과 같은 이유 — 모든
   FILE*가 `.lock=-1`이라 실제로는 절대 호출되지 않지만 링크에는
   필요).
4. `userland/musl-hello`를 커널 직접 스폰(M28~M30, VFS가 없는 부트
   극초반)에서 `servers/CMakeLists.txt`의 15번째 **정식 서비스**로
   재배치했다 — `--depends=musl-hello:vfs`로 다른 서비스와 같은
   방식으로 VFS 핸들을 상속받는다. `tools/mkbootdisk.py`에 새
   `--linux-abi-stack=<이름>`(기존 `--trusted=`와 대칭)을 추가해
   `lib/*.ini`에 `linux_abi_stack=1`을 쓰고, `init/initrun/main.cpp::
   spawn_visit()`가 이 키를 읽어 `mc_process_spawn_request::
   linux_abi_stack`을 켠다(M28이 도입한 Linux ABI 초기 스택 —
   musl-hello만 필요, 다른 14개 서비스는 이 키가 없어 회귀 없음).
5. `userland/musl-hello/main.c` 확장 — `fopen("test.txt", "r")`+
   `fread`+`fclose`+`printf`로 procsrv의 기존 자기테스트
   (`run_vfs_roundtrip_test`)가 이미 써 둔 파일("hello vfs", 9바이트)
   을 진짜 musl stdio로 열어 읽고 그 내용을 printf로 확인한다.

## 실행 중 발견 (ADR-205)

[ADR-205](../design/kernel-memory.md)에 전체 판단 근거를 기록했다.
요약: (1) musl-hello가 VFS 핸들을 받으려면 부트 극초반 커널 직접
스폰 자리를 떠나 initrun이 관리하는 정식 서비스가 돼야 했다 — 이
과정에서 "이 서비스만 Linux ABI 초기 스택이 필요하다"를 표현할
데이터 기반 메커니즘(`--linux-abi-stack=`, ADR-183 §결정4와 같은
정신)이 필요해 새로 만들었다. (2) `vfprintf.c`의 부동소수점 서식이
SysV 관례상 SSE(XMM0)를 요구해 이 파일만 `-msse`로 예외를 뒀다 —
M9~M11b가 이미 완성한 유저 스레드별 FPU 컨텍스트 스위칭 덕분에
안전하다는 것을 확인했다. (3) `include/unistd.h`의 `syscall()`
선언이 `src/internal/syscall.h`의 매크로와 충돌하는 것을
`_D_XOPEN_SOURCE=700`(musl 자신의 Makefile이 항상 쓰는 값)으로
해결했다 — musl을 무수정으로 쓰려면 musl 자신의 빌드 플래그를
그대로 맞춰야 한다는 것을 보여준 사례다.

## 검증

x86_64 전체 재빌드 성공(musl-hello가 이제 15번째 정식 서비스로
initrd가 아니라 bootdisk에 담긴다). bootdisk 재생성. QEMU에서
확인: "musl fopen ok=1" → "[procsrv] vfs write/read roundtrip ok=1"
(procsrv 자신의 M13 자기테스트, 순서상 먼저 완료) → "musl fread
content ok=1" → "musl printf read: hello vfs (9 bytes)" — 이후
나머지 부팅(shell 자기테스트까지)이 평소와 동일하게 계속됨(회귀
없음). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) — 신규 확인 문자열(musl fopen/fread/printf) 포함 |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M32(프로세스 syscall — fork/execve/wait4
실왕복)부터 계속 진행한다. VFS 쓰기(`SYS_write`를 통한 파일 쓰기)는
이번 라운드에서 다루지 않았다(읽기만 검증) — 필요해지면 별도로
`SYS_writev`/`SYS_write`의 VFS-fd 분기를 추가한다. `SYS_lseek`은
여전히 항상 실패한다(fs-protocol.md에 임의 위치 이동 오퍼레이션이
없다 — 필요해지는 시점에 재검토).
