# M52 완료 — 자체 작성 최소 셸(msh) + coreutils(echo/ls/cat)

[musl-userland-porting.md](../plan/musl-userland-porting.md) §M52.
관련 ADR: [ADR-221](../design/foundations.md)(BusyBox 도입 철회,
방향 전환), [ADR-222](../design/kernel-memory.md)(execve() 실제
argv 전달), [ADR-223](../design/kernel-ipc-objects.md)(memfs
동시성 버그).

## 무엇을 했는가

계획 원안은 BusyBox(서드파티 sh+coreutils)를 정적으로 musl에
링크하는 것이었다. 실제로 vendoring까지는 됐지만 빌드 연결
단계에서 이 저장소의 툴체인(clang 컴파일+`ld.lld` 직접 링크)과
BusyBox의 빌드 가정(hosted gcc/clang이 컴파일+링크를 한 번에 처리)
이 근본적으로 안 맞아, 사용자 결정으로 BusyBox를 완전히 철회하고
이 저장소 안에서 직접 작성하기로 방향을 바꿨다(ADR-221 — 이미
`5b4f60d` 커밋으로 반영돼 있었다). 이번 라운드는 그 결정을 실제로
완주했다:

- **`userland/msh`** — 진짜 minicore 프로그램(musl 링크,
  `linux_abi_stack=1`). 명령줄을 공백으로 토큰화해 `argv[0]`을
  `/bin/<이름>` VFS 경로로 매핑하고, **빌트인이 아니라** 매번
  `fork()`+`execve()`로 별도 실행파일을 띄운다(userland/pipe-test가
  이미 증명한 M51의 pipe/dup2와 M32의 fork/execve/waitpid를 그대로
  쓴다). 키보드 입력이 없으면(servers/login과 같은 관례) 고정된
  자기테스트 명령줄 셋(`echo hello msh`/`ls`/`cat /bin/echo`)을
  순서대로 실행한다.
- **`userland/echo`** — argv를 공백으로 이어 붙여 출력.
- **`userland/ls`** — Linux `getdents`류 syscall이 없어(이 커널엔
  없고 이번 라운드도 만들지 않는다), libmc의 `mc_vfs_open`/
  `mc_fs_list`를 직접 호출한다(musl 링크 프로그램도 `minicore_libmc`
  를 항상 같이 링크한다, M50). `mc_fs_list`가 fs_handle 하나로 그
  FS 서버의 flat-namespace 전체를 나열하는 구조라, 이미 존재를
  보장할 수 있는 파일(`/bin/echo`)을 열어 그 fs_handle을 얻는
  "부트스트랩" 관례를 썼다 — `userland/shell`의 기존 ADR-170 빌트인
  `ls`와 정확히 같은 패턴이다.
- **`userland/cat`** — musl의 진짜 `open()`(이번이 첫 실제 소비자,
  `libc/CMakeLists.txt`에 `src/fcntl/open.c` 추가)+`read()`로 파일을
  읽어 `write(1, ...)`로 그대로 흘려보낸다.
- **`servers/procsrv`** — `run_coreutils_seed()`를 추가해 부팅 시
  `tools/bin2c.py`로 심어 둔 세 실행파일의 원본 바이트를 VFS(memfs)
  의 `/bin/echo`/`/bin/ls`/`/bin/cat`에 미리 써 둔다(M32의
  `musl-exec-target` 시딩과 같은 패턴).
- **`servers/CMakeLists.txt`** — `msh`를 initrun의 `--service=`
  목록에 추가하고(`--depends=msh:vfs,procsrv`, `syscall_shim.c`의
  고정 핸들 순서 관례에 맞춤), `--linux-abi-stack=msh`를 추가했다.
  `echo`/`ls`/`cat`은 initrun이 직접 스폰하지 않는다 — `msh`의
  자식으로 `fork()`+`execve()`될 뿐이고, `execve()`는 핸들 테이블을
  초기화하지 않으므로(ADR-179) `msh`가 물려받은 vfs/procsrv 핸들을
  그대로 상속한다.

## 실행 중 발견한 진짜 버그 2건

### 1. `execve()`가 M28부터 진짜 인자를 전달한 적이 없었다 (ADR-222)

M28이 musl의 진짜 시작 경로(`_start`)를 처음 지원했을 때는 "실행
이미지가 바뀌는 것"만 증명하면 충분해, `build_process()`의
`linux_abi_stack` 스택 레이어웃이 `argc=1`, `argv[0]="/bin/
musl-hello"` 고정 문자열 하나만 채웠다. `mc_exec_request`가 M27부터
이미 갖고 있던 `argv_blob`/`argv_size` 필드는 완전히 무시됐고,
`libc/sysdeps/minicore/syscall_shim.c::exec_common()`도 `execve()`의
`argv` 인자를 그대로 버렸다. M28~M51의 모든 소비자(musl-hello,
musl-exec-target, pipe-test)가 인자가 필요 없는 프로그램이라 이
간극이 한 번도 드러나지 않았다 — `msh`가 자식에게 실제 명령줄
인자를 넘기는 첫 소비자다.

고쳤다: `exec_common()`이 `argv[]`를 NUL로 구분된 하나의 blob으로
엮어 `mc_exec()`에 실제로 실어 보내고, `build_process()`의 스택
레이아웃을 고정 1개 문자열 구조에서 진짜 `argc`/`argv[]`(최대 8개,
문자열 총합 224바이트 예산)로 바꿨다. `argv_blob`이 없으면(대다수
initrun 스폰 서비스는 여전히 안 준다) 기존 M28 기본값으로 그대로
되돌아간다 — 기존 호출자는 회귀하지 않는다.

### 2. memfs의 응답 스크래치 버퍼가 열린 파일 전체에서 하나뿐이었다 — 진짜 데이터 손상 (ADR-223)

**가장 심각한 발견.** `msh`가 `fork()`+`execve("/bin/echo", ...)`로
echo.elf(82KB, 21페이지)를 읽는 동안 진짜 페이지 폴트(vector=14,
error_code=0x7)로 죽었다. 처음엔 폴트의 `RDI`/`RDX`가 M52가 새로
건드린 argv 전달 경로의 고정 매핑 주소(`k_argv_user_vaddr`)와
값이 같다는 우연 때문에 그쪽을 의심했지만, `query_page(cr2)`가
`present=1 write=0 exec=1`(정상적인 R+E 코드 페이지)을 보여 줘
권한 문제가 아님을 확인했다. 커널의 ELF 로더 안에서 원본
`elf_data` 버퍼(아직 msh 자식 자신의 주소공간, 커널이 손대기 전)
를 직접 찍어 보니 **소스 바이트 자체가 이미 오염돼 있었다** — 진입
지점 근처의 진짜 내용이 자기 파일의 다른(더 앞선) 오프셋 내용으로
바뀌어 있었다.

근본 원인: `kernel/core/ipc/endpoint.cpp::deliver_message()`의 §2
경로(유저 프로세스 수신자)는 `page_descriptor::mode`가
`transfer_mode::copy`("COPY")라도 실제로 바이트를 복사하지 않는다
(ADR-159/161이 이미 이렇게 설계해 뒀다) — 발신자 프레임을 그대로
수신자에게 매핑하는 zero-copy다. `servers/fs/memfs`의
`handle_read()`는 이 매핑 대상 데이터를 **열린 인스턴스 전체가
공유하는 정적 버퍼 하나**(`g_read_scratch`)에 채워 넘긴 뒤 곧바로
다음 요청을 받으러 돌아간다 — M13~M51 내내 memfs와 동시에 대화하는
클라이언트가 항상 하나뿐이라(매 다음 요청이 그 클라이언트 자기
자신의 다음 호출로만 이어졌다) 이 공유가 한 번도 문제가 되지
않았다. `msh`가 echo.elf를 읽는 동안, 마침 같은 시각에 실행 중이던
**기존** M32 musl fork/exec 자기테스트(비슷한 크기의
musl-exec-target.elf를 읽는 중)가 같은 memfs에 별도로 접속해 자기
파일을 읽으면서 이 하나뿐인 버퍼를 동시에 덮어썼다 — 한쪽이 아직
못 읽은 응답 내용이 다른 쪽 요청으로 그 자리에서 바뀌는 실제
경합이었다.

고쳤다: `g_read_scratch`를 `open_id`로 인덱싱하는 배열
(`g_read_scratch[k_max_open_files][k_page_size]`)로 바꿔 서로 다른
열린 인스턴스가 절대 같은 물리 프레임을 공유하지 않게 했고,
`open_id`가 없는 `handle_list()`에는 전용 슬롯을 하나 더 얹었다.
커널의 IPC §2 매핑 자체(zero-copy)는 바꾸지 않았다 — 그 계약을
지켜야 하는 쪽은 스크래치 버퍼를 공유하는 서버 자신이라는 책임
소재를 명확히 하고 memfs를 거기에 맞췄다.

### 부수 발견: procsrv 자신의 M18 loader roundtrip 자기테스트가 조용히 실패하고 있었다

echo/ls/cat 블롭 셋을 procsrv 자신의 컴파일 시점 데이터로 더
심으면서 procsrv 자신의 ELF가 418640바이트까지 커져, M32가 이미
한 번 늘렸던 `servers/fs/memfs::k_max_file_bytes`(262144)와 procsrv
자신의 재조립 버퍼 `g_reassembled`(같은 262144)를 다시 넘었다 —
`[procsrv] loader roundtrip ok=0`이 매 부팅 조용히 남고 있었다.
둘 다 1048576(1MiB)으로 함께 올려 해결했다.

## 검증 (QEMU, x86_64)

핵심 확인 로그:

```
[msh] no keyboard input, running self-test commands
[msh] running: echo hello msh
[procsrv] coreutils seed ok=1
[procsrv] loader roundtrip ok=1
[msh] running: ls
[msh] running: cat /bin/echo
[msh] self-test done ok=1
```

`tools/smoke-test-x86_64.sh`에 M52 어서션 7개 추가, 전체 스모크
스위트(146개) PASS(exit 0, FAIL 0). 추가로 SMP(11)/NUMA(24)/
AVX(12)/net(6) 4개 회귀 스위트 전부 PASS(exit 0, FAIL 없음) —
`build_process()`의 argv 전달 변경(모든 `linux_abi_stack=1` 소비자
에 영향)과 memfs의 버퍼 배열화가 다른 IPC/musl 소비자에 회귀를
만들지 않았음을 확인했다.

## 범위 밖으로 남긴 것 (계획 문서가 이미 명시)

- 파이프/리다이렉션(`|`, `>`, `<`) — M54로 미룬다.
- job control(백그라운드 `&`, `Ctrl+C`) — M55(스트레치)로 미룬다.
- 셸의 명령줄 파싱은 공백 분리만(따옴표/이스케이프 없음) — M54가
  파이프/리다이렉션을 다룰 때 다시 검토한다.
- `$PATH` 탐색 없음(`/bin/<이름>` 고정 매핑) — YAGNI.
- `ls`는 여전히 flat-namespace 전체를 나열할 뿐 서브디렉터리
  개념이 없다(memfs 자체의 기존 제약, ADR-170 빌트인 `ls`와 같은
  한계).

## 다음

M53 — `msh`를 로그인 후 실제 셸로 배선한다(계획 문서 참고).
