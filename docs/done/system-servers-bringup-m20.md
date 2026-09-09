# 완료 보고: system-servers-bringup M20 — libc/POSIX 최소 포팅 + 로그인 후 셸 (계획 최종 마일스톤)

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M20
**관련 결정**: [foundations.md](../design/foundations.md) ADR-132(libmc
설계), ADR-170(M20 범위 좁힘), [security-model.md](../design/security-model.md)
ADR-089(session_program 설계), ADR-171(M20 구현 단순화),
[filesystem.md](../design/filesystem.md) ADR-172(fs-protocol v4)
**실행일**: 2026-09-09

## 완료한 것

### D0. 선행 스코핑 — ADR-170/171/172

이 마일스톤은 계획이 "third_party/에 실제 libc+셸을 서브모듈로
들여 포팅한다"고 적어 둔 것과, 실제로 검증 가능한 결과물을 이번
라운드에 만들어내는 것 사이에 큰 간극이 있었다 — 이 커널은
syscall이 12개뿐이고 open/read/write/mmap/brk/fork/exec가 전부
procsrv/VFS/FS IPC로 구현돼 있어, 실제 musl 같은 서드파티 libc를
포팅하는 것은 M12~M19 어느 마일스톤보다 훨씬 큰 작업이다. 사용자에게
`AskUserQuestion`으로 세 가지를 확인했다: (1) 실제 libc 포팅은
보류하고 `libmc`(ADR-132가 이미 설계해 뒀지만 디렉터리 자체가 없던
1단 계층)를 먼저 만든다, (2) 셸에서 `ls`/`cat`은 별도 프로세스가
아니라 셸 프로세스 안의 빌트인이다(ADR-008이 "가장 어려운 문제"로
지적한 fork의 다중 서버 fd 상속 문제를 이번 라운드에서 피한다),
(3) `libmc` 자체도 이번 검증에 필요한 최소 클라이언트만 만든다.
세 결정 모두 ADR-170(foundations.md)에 기록했다. 구현 중 "로그인
후 셸이 실제로 어떻게 시작되는가"(ADR-171)와 "ls가 필요로 하는
목록 조회가 fs-protocol에 없다"(ADR-172)는 두 개의 추가 설계
결정을 발견해 각각 별도 ADR로 기록했다.

### D1. `libmc/` — 신설, 최소 부분집합

`docs/design/repo-layout.md`가 이미 예약해 둔 자리에 처음으로
실제 코드를 채웠다. syscall 1:1 C 래퍼(`mc_ipc_call`/`mc_ipc_recv`/
`mc_ipc_reply`/`mc_debug_log`/`mc_thread_exit`, 헤더 전용), VFS의
`OP_OPEN` 클라이언트(`mc_vfs_open`), FS 공통 프로토콜의 `OP_READ`/
`OP_LIST` 클라이언트(`mc_fs_read_all`/`mc_fs_list`), 콘솔의
`OP_PRINT`·ps2의 `OP_READ_KEY` 클라이언트, `_start` 진입 글루를
갖췄다. procsrv/vfs/devmgr/cfgsrv 클라이언트는 이번 라운드의 유일한
소비자(셸)가 쓰지 않아 비워 뒀다(ADR-170 §영향).

### D2. `userland/shell` — minicore 전용 최소 셸(대체 구현)

libc 없이 `libmc`만 링크하는 순수 C 네이티브 앱이다(ADR-049 근거,
ADR-170 §결정2). 부팅 시 initrun이 다른 서비스와 똑같이 직접
스폰하고(`depends=vfs,console,ps2`), 자기 endpoint(handle 1)에서
`sys_ipc_recv`로 블록하며 procsrv의 `OP_START`를 기다린다. 받으면
프롬프트를 내고 `ls`(memfs `OP_LIST`로 파일 목록 조회)·`cat <경로>`
(VFS로 열고 EOF까지 읽어 그대로 출력)를 빌트인으로 처리한다. 실제
키 입력이 QEMU 자동화 환경에 주입되지 않는 문제(ps2/login이 이미
겪은 것과 같은 제약)는 같은 해법을 재사용한다 — 첫 줄 읽기에서 키가
하나도 안 오면 `ls`와 `cat test.txt`를 결정적으로 한 번씩 실행해
검증한다.

### D3. `servers/procsrv` — `OP_START` 신호(ADR-171)

`depends=vfs,cfgsrv,shell`(handle 2/3/4)로 셸에 대한 캐패빌리티를
스폰 시점에 받는다. `OP_LOGIN`이 성공하면(정적 플래그로 딱 한 번만)
셸의 handle에 `OP_START`(label=1)를 보낸다. ADR-089가 정한 "procsrv가
`session_program`을 `exec()`한다"를 문자 그대로 구현하는 대신 이
방식을 쓴 이유는, 그러려면 procsrv가 **셸 바이너리의 원본 ELF
바이트**를 자기 주소공간에 가져와야 하는데 그럴 방법이 없어서다 —
M18의 su-target 로더는 procsrv 자신의 self_info 브릿지(자기 자신의
ELF)를 재사용한 것이라 다른 바이너리로는 일반화되지 않는다. 반면
"이미 스폰된 프로세스에 캐패빌리티 주입 시점에 확보한 handle로 IPC
신호를 보내는 것"은 새 커널/IPC 기능 없이 M13부터 검증된 기존
메커니즘 그대로다.

### D4. `servers/fs/memfs` — `OP_LIST`(fs-protocol v4, ADR-172)

`OP_WRITE`/`OP_READ`와 같은 정신으로 VFS를 거치지 않고 클라이언트가
memfs 핸들에 직접 건다. 채워진 파일 슬롯 이름을 NUL로 구분해 한
페이지에 담아 반환한다. FAT32/ext4로의 확장, 중첩 디렉터리 지원은
범위 밖(memfs는 원래부터 평평한 이름공간).

### D5. 부트 순서/빌드 배선

`servers/CMakeLists.txt`가 `userland/shell`을 13번째 부트 서비스로
담는다(전체 순서: memfs→devmgr→ps2→console→usb→virtio-blk→fat32→
ext4→vfs→cfgsrv→shell→procsrv→login — shell이 procsrv보다 먼저
떠 있어야 procsrv가 그 handle을 받을 수 있다). 최상위
`CMakeLists.txt`는 `libmc`→`userland`→`servers`→`libc` 순서로
바꿨다(servers의 부트 디스크 조립이 `userland/shell`의 빌드 산출물을
참조하므로).

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh       # 82개 전부 PASS(M19의 76개 + 신규 6개) — 2회 연속 재확인, fat32/ext4 이미지 해시 불변
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌):

```
[login] no keyboard input, using self-test account
[shell] session started
[procsrv] shell session start ok=1
[login] auth ok=1
[su-target] uid=0 super=1 guest=0
[login] su delegated ok=1
[login] su denied ok=1
[shell] no keyboard input, running self-test commands
[shell] ls ok=1
[shell] cat ok=1
[shell] self-test done
```

- **확인함**: 셸이 로그인 전에는 아무 것도 하지 않고 블록만 하다가,
  로그인이 성공한 뒤에야 procsrv의 `OP_START`로 실제 세션이
  시작된다 — 계획이 요구한 "로그인 프롬프트를 통과하면 셸이
  뜬다"는 순서 관계를 그대로 만족한다.
- **확인함**: `ls`(memfs `OP_LIST`)와 `cat`(VFS 열기 + `OP_READ`
  EOF까지)이 둘 다 성공하고, `cat test.txt`의 내용이 procsrv의
  M13 자체 테스트가 써 둔 "hello vfs"와 정확히 일치한다 — 계획의
  "ls/cat 같은 기본 명령으로 파일을 조회할 수 있다"를 memfs(M13)
  위에서 실제로 증명했다(FAT32/ext4는 이번 셸의 `ls`/`cat` 대상은
  아니다 — 아래 "알려진 단순화" 참고).
- **이번 라운드도 실행 중 새로 발견한 버그가 없었다** — 첫 전체
  QEMU 부팅에서 바로 82/82 통과했다(libmc/셸 작성 중 코드 리뷰
  단계에서 handle 번호·wire 포맷을 procsrv/vfs/memfs의 기존 구현과
  꼼꼼히 대조해 미리 맞췄다).
- **알려진 단순화(계획이 원래 요구했지만 이번 라운드에 하지 않은
  것, ADR-170 §결정1/2가 명시적으로 남긴 것)**:
  - **실제 서드파티 libc(musl 등) 포팅** — `third_party/`에
    git submodule로 들여 패치를 적용하는 것은 하지 않았다.
    `libc/`, `third_party/`는 이 라운드 이전과 마찬가지로 여전히
    비어 있다(스캐폴딩만 존재).
  - **실제 서드파티 셸/coreutils 포팅** — `userland/shell`은
    minicore 전용으로 새로 쓴 대체 구현이다(ADR-049). 포팅된
    셸이 요구하는 진짜 POSIX 프로세스/fd 모델은 여전히 없다.
  - **외부 프로그램 실행(진짜 fork/exec)** — `ls`/`cat`은 빌트인
    이라 procsrv의 fd-여러-서버-상속 문제(ADR-008)를 실제로
    풀지 않았다.
  - **계정별 `session_program`**(ADR-089) — 모든 로그인이 같은
    단일 셸 인스턴스로 이어진다. `user_account.session_program`
    필드도 아직 없다.
  - **`ls`/`cat`의 FAT32/ext4 지원** — `OP_LIST`는 memfs 전용이라
    셸의 `ls`도 memfs만 본다. `cat`은 경로만 맞으면 VFS의 마운트
    테이블을 그대로 타므로 이론상 `/mnt/fat32/hello.txt` 등도
    되지만, 이번 라운드의 자체 테스트·스모크 테스트는 검증하지
    않았다.

## 다음 단계

**system-servers-bringup.md 계획 전체(M12~M20)가 이것으로 완료됐다**
— 계획서에 다음 마일스톤은 없다. 다만 위 "알려진 단순화"가 보여주듯
이 계획의 최종 완료 기준을 "포팅"이 아니라 "기능적 대체 구현"으로
충족했을 뿐, ADR-005가 원래 지향한 실제 서드파티 libc/셸 생태계
포팅은 여전히 미착수 상태다 — 프로젝트 자체가 끝난 것은 아니고,
그 포팅은 별도의 향후 계획(다음 `docs/plan/*.md`) 대상으로 남는다.
