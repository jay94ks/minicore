# 실행 계획: 실제 서드파티 셸/coreutils 재포팅 (M51~M55)

**관련 결정**: [real-libc-syscall-layer.md](real-libc-syscall-layer.md)
(M27~M38, musl syscall 계층·malloc·파일 I/O·stdio·fork/exec/wait·
signal·pthread·locale를 이미 실전 검증), [kernel-memory.md](../design/kernel-memory.md)
ADR-203(동적 링킹 포기, 정적 링킹 확정 — 이 계획도 그대로 따른다),
[build-system.md](../design/build-system.md) ADR-022(서드파티 소스
submodule+패치 디렉토리 관례), [security-model.md](../design/security-model.md)
ADR-090/092(su/sudo, 셸이 만드는 자식 프로세스와 무관), [foundations.md](../design/foundations.md)
ADR-170(M20이 셸을 minicore 네이티브 대체 구현으로 좁힌 결정 — 이
계획이 마침내 되돌린다), [open-items.md](../design/open-items.md)
OPEN-64(fd 진실 공급원 완전한 프로토콜, 이 계획과 무관하게 남음)·
OPEN-66(musl 포팅의 남은 세부)

**선행 완료 전제**: [real-libc-syscall-layer.md](real-libc-syscall-layer.md)
M27~M38 전부 완료돼 있어야 한다(완료됨, `docs/done/` 참고) — 실제
musl 시작 경로+malloc+파일 I/O+stdio+fork/exec/waitpid+signal+pthread
+locale가 이미 실전 검증돼 있어야 이 계획이 "셸 하나"가 아니라 "그
위에서 도는 진짜 프로그램"을 다룰 수 있다. M39(같은 계획의 스트레치
목표)는 2026-09-10 사용자 결정으로 스킵됐다 — 이 계획은 M39를
그대로 재시도하는 것이 아니라, M39가 전제했던 것 중 실제로 무효가
된 부분(동적 링킹)을 걷어내고 다시 설계한 것이다.

## 배경

M20(ADR-170)과 M26(ADR-182)이 두 번 미룬 원래 목표 — "실제로 포팅된
서드파티 셸/coreutils로 로그인 후 셸을 대체한다" — 를 real-libc-syscall-layer.md
가 M27~M38로 그 전제 조건(musl이 진짜로 syscall을 할 수 있는 상태)을
만들어 뒀다. M39가 이 목표를 다시 시도하려 했지만, M39 자신의 문안이
"이 마일스톤에서 처음으로 여러 바이너리가 같은 `libc.so`를 공유하는
M29의 원래 목표가 실현된다"고 **동적 링킹을 전제**하고 있었다 —
그런데 M29(ADR-203)는 이미 musl 자신의 `libc.so`/ld.so 자기재배치
부트스트랩을 "질적으로 다른 위험도"로 판단해 시도하지 않고 정적
링킹으로 되돌아가 있었다. M39는 그 전제가 무효화된 상태로 계획
문서에 남아 있었을 뿐이라 사용자가 스킵을 택했다.

이 계획은 M39를 **정적 링킹 전제로 다시 설계**한다 — 동적 링킹(여러
바이너리가 `.so` 하나를 공유하는 일반화)은 여전히 범위 밖이고,
그 대신 서드파티 셸/coreutils를 **각자 정적으로 musl에 링크**한
독립 ELF로 포팅한다. M39가 "사전 진단"으로 이미 경고해 둔 대로,
실제 셸이 필요로 하는 것 중 이 프로젝트가 아직 갖추지 않은 진짜
기능(파이프, `dup2` 리다이렉션)이 있다 — 이번 계획은 그것부터
먼저 만든다(M51).

## M51. 파이프(`pipe()`) + `dup2()` — 셸의 `|`/리다이렉션이 요구하는 최소 IPC 프리미티브

- **구현**: musl의 `pipe()`/`pipe2()`(`SYS_pipe2`)와 `dup2()`
  (`SYS_dup2`)가 실제로 동작해야 한다. 정확한 구현 방식(새 커널
  오브젝트 종류를 하나 추가할지, 기존 IPC endpoint를 단방향으로
  재사용할지, VFS/memfs를 파이프 백엔드로 삼을지)은 착수 시점에
  기존 아키텍처(ADR-004/011/013 IPC 원시, ADR-155/159/161 pages[]
  전달)와의 정합성을 보며 결정하고 새 ADR로 기록한다 — 이 계획
  문서는 방식을 미리 정하지 않는다(ADR-196/197이 svcmgr 설계를
  실행 직전에 확정한 것과 같은 절제).
- **범위**: 익명 파이프(`pipe()`)만 다룬다 — named pipe(FIFO,
  `mkfifo`)는 범위 밖. 블로킹 읽기/쓰기(버퍼 가득/빔)는 지원해야
  하지만, 파이프 용량 정책은 고정된 작은 버퍼(예: 4KiB) 하나로
  단순화한다(YAGNI).
- **목표**: 새 최소 테스트 프로그램(기존 `musl-hello`류와 무관,
  착수 시점에 확정)이 `pipe()`로 fd 두 개를 얻고, `fork()` 후
  부모가 쓰고 자식이 읽는(또는 반대) 왕복이 정확한 바이트 수로
  성립함을 QEMU로 확인한다. `dup2()`로 그 파이프의 한쪽 끝을
  자식의 stdin/stdout(fd 0/1)에 겹쳐 씌우는 것까지 확인한다(셸의
  리다이렉션이 실제로 쓰는 패턴).

## M52. 서드파티 셸+coreutils 선정 + submodule 추가 + 정적 링크 빌드

- **선정(사전 검토, 착수 시점 확정)**: **BusyBox를 권장한다** —
  `sh`(ash 기반)+`ls`/`cat`/`echo`/`echo` 등 흔히 쓰는 coreutils
  전부를 **단일 정적 바이너리**(multicall)로 제공해, 셸과
  coreutils를 별도 프로젝트 두 개로 각각 포팅·빌드·패치해야 하는
  부담을 없앤다 — musl 기반 배포판(Alpine 등)이 이미 이 조합으로
  널리 검증돼 있어 포팅 중 마주칠 문제의 선례도 많다. 대안(`dash`+
  `toybox`/개별 coreutils 등)은 바이너리 두 개 이상을 각각 다뤄야
  해 이 라운드에는 과함(YAGNI) — 필요해지면 재검토.
- **구현**: `third_party/busybox`를 새 git submodule로 추가하고
  (ADR-022 관례 그대로, `.gitmodules`에 이미 자리가 비어 있다),
  `CONFIG_STATIC=y`(정적 링크, ADR-203 그대로 재확인) 설정으로
  이 커널의 musl(`libs/mc`/`libc`가 아니라 `third_party/musl`
  그 자체 — M38의 SDK 툴체인을 재사용)에 링크한다. 필요한 패치는
  `third_party/patches/busybox/*.patch`(같은 관례)에 둔다 — BusyBox
  의 자체 `libc` 가정(예: 특정 syscall 존재 확인 매크로) 중
  `syscall_shim.c`가 아직 -ENOSYS인 것들은 이 라운드에 드러날
  실제 목록이다(사전 예측하지 않는다).
- **목표**: 저장소 안에서 BusyBox를 크로스 컴파일해 정적 ELF 하나를
  만들고, 부팅과 무관한 별도 확인(QEMU로 직접 실행, 아직 initrun
  서비스 목록에 넣지 않음)으로 `busybox sh -c 'echo hello'`가
  정확한 출력을 낸다는 것만 먼저 증명한다 — M53에서 실제 로그인
  경로에 편입한다.

## M53. 로그인 후 셸을 minicore 네이티브(ADR-170) 대신 이 포팅된 바이너리로 교체

- **구현**: `user_account.session_program`(ADR-089, 다만 M20 실행
  범위는 이걸 "procsrv→셸 OP_START IPC 신호"로 좁혀 뒀다 — 이
  계획은 그 단순화를 그대로 유지한 채, OP_START가 깨우는 대상
  ELF만 `userland/shell`(minicore 네이티브)에서 BusyBox 정적
  바이너리로 바꾼다) — VFS를 통해 `/bin/sh`(BusyBox의 멀티콜
  진입점)로 spawn한다. 기존 fork+exec 경로(M32)를 그대로 재사용.
- **주의(사전 진단)**: M17~M39가 반복해 온 패턴대로, 착수 시점에
  실제로 필요한 기능이 드러날 가능성이 높다 — 특히 BusyBox의 ash가
  시작 시점에 확인하는 `isatty`/`tcgetattr`류(TTY 제어, ADR-098의
  fd 상속 규칙과 맞물림)나 `getenv`(환경변수 — 이 커널은 아직
  envp를 셸에 전달하는 관례가 없다). 이런 것들이 드러나면 그 자리
  에서 범위를 좁히고 새 ADR로 기록한다(ADR-170/182와 같은 정직한
  패턴) — 대화형 셸의 프롬프트/줄 편집 완성도까지는 이 계획의
  목표가 아니다, "네이티브 구현이 아니라 포팅된 바이너리가 명령을
  실행한다"는 것만 증명하면 충분하다.
- **목표**: 로그인 후 뜨는 셸이 minicore 네이티브 구현이 아니라
  포팅된 BusyBox이고, `ls`/`cat`/`echo` 같은 명령이 셸의 빌트인이
  아니라(BusyBox 멀티콜 심볼릭링크를 통한) **별도 실행파일 인자로
  fork+exec**됨을 QEMU 로그로 확인한다(M39가 원래 세운 목표 그대로).

## M54. 파이프라인/리다이렉션 실제 사용 (M51의 프리미티브를 셸이 실제로 행사)

- **구현**: 없음 — M51+M53이 이미 만든 것의 통합 검증 라운드다.
- **목표**: `ls | cat`(파이프)와 `echo hello > /tmp/out.txt`(출력
  리다이렉션) 같은 실제 셸 명령이 BusyBox 자신의 파서/실행기를
  거쳐 정확히 동작함을 self-test 명령 목록(로그인 자동화 경로,
  ADR-165 §결정4와 같은 관례)으로 QEMU에서 확인한다.

## M55. Job control 최소 — 프로세스 그룹 + 포그라운드 시그널 라우팅 (스트레치)

- **구현**: `setpgid()`/`getpgid()`(`SYS_setpgid`/`SYS_getpgid`)와,
  터미널의 Ctrl-C(`SIGINT`)가 셸 자신이 아니라 **현재 포그라운드
  프로세스 그룹**에게 전달되는 최소한의 라우팅만 다룬다.
- **주의(사전 진단)**: `tcsetpgrp`/`tcgetpgrp`(제어 터미널 개념
  자체, OPEN-39가 이미 미결로 남겨 둔 "로그인 프롬프트/콘솔
  드라이버 상세 설계"와 맞물림)와 `SIGTSTP`/`SIGCONT`(bg/fg
  전환, Ctrl-Z)는 이 마일스톤 범위 밖이다 — **이 마일스톤이
  착수되지 않거나 실패해도 M51~M54의 성과는 독립적으로 유효하다**
  (BusyBox 자신은 job control 없이도 스크립트/파이프라인 용도로
  충분히 쓸 수 있다) — 그래서 스트레치로 맨 뒤에 뒀다.
- **목표**: 셸이 자식(예: 무한 루프 프로그램)을 포그라운드로 실행
  중일 때 콘솔에서 Ctrl-C를 누르면 셸 자신은 살아남고 그 자식만
  종료됨을 확인한다.

## 포함하지 않는 것 (이 계획 이후로 명시적으로 미룸)

- **동적 링킹(여러 바이너리가 musl의 공유 `libc.so` 하나를
  공유하는 일반화)** — ADR-203이 이미 "질적으로 다른 위험도"로
  판단해 시도하지 않기로 확정했다. 이 계획의 모든 바이너리는
  정적 링크다.
- **실시간 시그널(`SIGRTMIN`~`SIGRTMAX`)·`SCHED_FIFO`/`SCHED_RR`**
  — real-libc-syscall-layer.md가 이미 범위 밖으로 명시해 둔 것과
  같다.
- **완전한 job control**(`SIGTSTP`/`SIGCONT`를 통한 bg/fg 전환,
  제어 터미널 소유권 이양) — M55가 다루는 "포그라운드 시그널
  라우팅"보다 훨씬 넓다. OPEN-39(로그인 프롬프트/콘솔 드라이버
  상세 설계 미결)가 먼저 해소돼야 제대로 다룰 수 있다.
- **named pipe(FIFO)**, **소켓** — M51은 익명 파이프만.
- **`dup_for_new_client` 완전한 fd 진실 공급원 프로토콜**(OPEN-64)
  — 여전히 su/sudo 경유 실행이 실제로 필요해지는 시점까지 미룬다.
  이 계획의 파이프 fd는 OPEN-64와 무관하게 동작해야 한다(파이프는
  VFS를 거치지 않는 별도 경로일 가능성이 높다 — M51 착수 시점에
  확정).
- BusyBox의 방대한 애플릿 전부를 켜는 것 — 셸(`sh`)+M54 검증에
  실제로 쓰는 애플릿(`ls`/`cat`/`echo` 등 극소수)만 `CONFIG_*`로
  켠다. 나머지는 빌드 크기·검증 범위를 불필요하게 늘린다(YAGNI).
- aarch64 이식 — 여전히 이 계획 이후로 미뤄 둔 별도 방향.

## 검증 방법

기존 계획들과 같은 방식 — QEMU 부팅 로그로 확인 가능한 마일스톤별
완료 기준을 두고, `tools/smoke-test-x86_64.sh`에 확인 문자열을
마일스톤마다 추가한다. M51의 파이프 프리미티브는 로그인 경로와
무관한 별도 테스트 프로그램으로 먼저 검증한 뒤(기존 `musl-hello`
류 패턴), M53부터는 실제 로그인 후 셸 자체가 검증 대상이 된다 —
`servers/login`의 기존 자동 로그인 자기테스트(ADR-165 §결정4)
뒤에 self-test 명령 목록을 실행시키는 방식을 그대로 재사용한다.

## 완료 후

각 마일스톤(또는 몇 개씩 묶어) 완료 시 `docs/done/`에 결과를
기록한다. 이 계획 문서 자체는 실행 후에도 수정하지 않고 "실행 전
계획" 그대로 보존한다(기존 계획들과 동일한 문서 체계 원칙).
