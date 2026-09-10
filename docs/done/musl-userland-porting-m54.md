# M54 완료 — msh 파이프라인(`|`)/출력 리다이렉션(`>`) 실제 구현

[musl-userland-porting.md](../plan/musl-userland-porting.md) §M54.
관련 ADR: [ADR-225](../design/foundations.md).

## 무엇을 했는가

계획이 예상한 "M51(pipe/dup2)+M53(msh 배선) 통합 검증, 새 구현
없음"은 실행 중 틀린 것으로 드러났다 — M51의 파이프 참조 카운트
계약(fork()가 fd 테이블을 복제하는 경우를 전제)과 msh가 실제로
필요로 하는 사용 패턴(파이프 양끝을 들고 있다가 각기 다른 자식에게
**넘겨준다**, execve()가 fd 테이블 자체를 지운다)이 근본적으로
안 맞아 새 메커니즘이 필요했다:

- **`libs/mc/include/mc/shell_fd_binding.h`(신규)** — msh가
  파이프/리다이렉션 대상 fd를 argv로 직접 실어 보내는 관례.
  `"@pipefd" <fd> <pipe_id>`/`"@filefd" <fd> <fs_handle>
  <open_file_id>` 토큰을 argv[1..] 맨 앞에 붙이고, 대상 프로그램은
  `main()` 맨 앞에서 `mc_shell_strip_bindings()` 하나만 불러 벗겨낸다.
- **`echo`/`ls`/`cat`** — 전부 `mc_shell_strip_bindings()` 호출
  추가. 파이프라인 중간 단계로 쓰일 수 있어 `close(0)`/`close(1)`을
  종료 직전에 명시적으로 불러 파이프 참조를 낮춘다(이 프로젝트엔
  "프로세스 종료 시 열린 자원 자동 회수"가 없다, OPEN-64와 같은
  이유).
- **`SYS_write`(syscall_shim.c)에 표준출력 리다이렉션 오버라이드
  추가** — 새 `g_std_redirect[3]`(fd 0/1/2 전용 표)+새 `mc_fs_write`
  클라이언트(`libs/mc/src/ipc/fs_client.c`, 기존 `mc_fs_read`의
  쓰기 쌍).
- **`userland/msh`** — `split_pipeline()`(`|`로 분리)+
  `extract_redirect()`(`>` 파싱)+`spawn_stage()`(각 단계를 argv
  바인딩과 함께 fork+exec)+`run_pipeline()`(파이프 생성→id 조회→
  msh 자신의 fd 정리→모든 단계 spawn→전부 waitpid). 자기테스트에
  `ls | cat`, `echo ... > /tmp/msh-redirect.txt`, 그 파일을
  `cat`으로 읽는 것까지 3개 명령을 추가(총 6개).
- **`servers/procsrv`** — msh에게 pipesrv 핸들도 상속시켜야 해서
  (`syscall_shim.c`의 고정 핸들 관례상 `MC_PIPESRV_HANDLE=4`)
  `inherited_handles`에 세 번째 항목 추가, `servers/CMakeLists.txt`
  의 `--depends=procsrv:...`에 `pipesrv` 추가.

## 실행 중 발견 — 진짜 버그 5건

1. **memfs `k_max_open_files`(64) 고갈** — msh 자기테스트 6개 명령
   각각의 VFS open(부트스트랩 `ls`+실제 파일 열기 등)이 부팅 전체의
   누적 open 인스턴스(닫는 기능이 아직 없다, OPEN-70)와 합쳐 64
   슬롯을 다 썼다. 256으로 상향.
2. **memfs `k_max_files`(8) 고갈** — 별도로 파일 개수 표(고유
   파일명 8개 한도)도 정확히 8개(`sys/etc/registry.dat`,
   `test.txt`, `musl-exec-target.elf`, `bin/echo`, `bin/ls`,
   `bin/cat`, `bin/su-target`, `home/guest1/allowed.txt`)로 이미
   가득 차 있어, 리다이렉션 대상 신규 파일(`/tmp/msh-redirect.txt`)
   생성이 실패했다. 16으로 상향.
3. **musl `open()`의 `O_CREAT` 세 번째 가변 인자(mode) 누락** —
   `open(path, O_WRONLY | O_CREAT)`처럼 두 인자만 넘기면 musl의
   `open.c`가 `O_CREAT`를 보고 무조건 세 번째 `va_arg`(mode_t)를
   읽는 게 UB다. `open(path, O_WRONLY | O_CREAT, 0644)`로 명시.
4. **`extract_redirect()`의 NUL 종료 누락** — `>` 토큰을 찾아
   `*pn`(토큰 수)만 줄이고 `argv[i]`를 실제로 `0`으로 안 끝맺어,
   `spawn_stage()`의 NUL-종료 순회가 `>`와 파일명까지 스푸리어스
   인자로 대상 프로그램에 넘겼다. `argv[i] = 0;` 추가로 고침.
5. **가장 심각한 발견 — pipesrv 참조 카운트 모델과 msh의 실제 사용
   패턴의 근본적 불일치, 두 겹의 연쇄 버그.** 자세한 인과관계와
   최종 해법(참조를 늘리지도 줄이지도 않고 그대로 "이동"시키는
   모델 — `mc_shell_bind_pipe_fd`가 더 이상 `mc_pipe_dup`을 안
   부르고, 새 `mc_shell_forget_pipe_fd`가 서버에 알리지 않고 msh의
   로컬 표만 지운다)는 [ADR-225](../design/foundations.md)에 적어
   뒀다. 여기서는 실제로 겪은 증상만 기록한다:
   - **1차(무한 대기)**: `SYS_fork`의 자동 dup 루프(M51)가 msh가
     파이프 양끝을 들고 있는 채로 하는 매 `fork()`마다 두 끝 다
     불필요하게 참조를 늘려, 다음 단계(`cat`)가 EOF를 영원히 못
     받는 채로 `pipe_read_retry`의 무한 재시도에 갇혔다. QEMU 부트
     테스트가 원래 120초 타임아웃으로 통과하던 것이 180초/240초/
     300초로도 완료되지 않아 처음엔 "그냥 더 느려졌나" 오인했다가,
     `servers/pipesrv/main.cpp`에 refcount 추적 디버그 로그를 임시로
     추가하고 `ls`/`cat`의 진입 여부를 확인해 진짜 원인(재시도가
     아니라 refcount가 결코 0에 도달 못 함)을 확인했다.
   - **2차(조용한 실패)**: 1차를 "fork() 전에 msh 자신의 파이프
     fd를 `close()`한다"로 고쳤더니, 그 `close()`가 msh가 만든
     유일한 참조까지 지워버려 파이프가 자식에게 넘겨지기도 전에
     사라졌다 — hang이 아니라 "ls | cat" 자기테스트가 조용히
     exit status 1을 반환하는 것으로 드러나, `[msh] self-test done
     ok=1`을 목표로 추가한 임시 stage별 진단 로그
     (`[msh] dbg stage=... waited=... wifexited=... status=...`)로
     정확히 `cat` 단계 자신이 비정상 종료(`read()`가 진짜 에러를
     반환)한다는 것을 좁혀냈다. 최종 수정 후 두 임시 진단 로그
     (`[msh] dbg i=...`, `[msh] dbg stage=...`)는 모두 제거했다.

## 검증 (QEMU, x86_64)

핵심 확인 로그(`ls | cat`/리다이렉션 구간):

```
[msh] running: ls @pipefd 1 2
[msh] running: cat @pipefd 0 1
sys/etc/registry.dat
test.txt
bin/echo
bin/ls
bin/cat
bin/su-target
home/guest1/allowed.txt
[msh] running: echo @filefd 1 10 36 msh-redirect-test
[msh] running: cat /tmp/msh-redirect.txt
msh-redirect-test
[msh] self-test done ok=1
```

`ls`의 파일 목록이 `cat @pipefd 0 1` 다음에 정확히 한 번 나타나고
(직접 콘솔 폴백이 아니라 파이프를 통해 `cat`이 읽어 출력한 것),
리다이렉션 파일도 `cat /tmp/msh-redirect.txt`로 되읽어 내용이
일치함을 확인했다. `tools/smoke-test-x86_64.sh`에 M54 어서션
4개(`ls @pipefd 1 2`/`cat @pipefd 0 1`/`echo @filefd ...`/
`cat /tmp/msh-redirect.txt`) 추가, 스모크 스위트 PASS(FAIL 없음).
5개 회귀 스위트 전부 PASS(exit 0): 스모크, SMP, NUMA, AVX, net.

## 범위 밖으로 남긴 것 (계획 문서가 이미 명시)

- 파이프라인 최대 4단계(`MC_MAX_STAGES`), argv fd 바인딩은 표준
  fd 0/1/2만 지원.
- 따옴표/이스케이프, `>>`(append), `<`(입력 리다이렉션) 없음.
- job control(M55, 스트레치) — 별도.

## 다음

M55(job control 최소, 스트레치) 진행 여부는 계획 문서 자체가
스트레치로 표시해 뒀다 — 사용자 판단 대상.
