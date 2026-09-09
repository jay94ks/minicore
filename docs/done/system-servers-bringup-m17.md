# 완료 보고: system-servers-bringup M17 — 콘솔/TTY 드라이버 + 로그인 흐름

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M17
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md)
ADR-164(콘솔/TTY 범위 좁힘)/ADR-166(배선 중 발견한 버그),
[security-model.md](../design/security-model.md) ADR-165(로그인
인증 범위 좁힘)
**실행일**: 2026-09-09

## 완료한 것

### D0. 선행 스코핑 — ADR-164/165

cfgsrv(M19)가 아직 없어 ADR-097의 "TTY 개수를 레지스트리에서 읽는다"
설계를 지금 구현할 수 없었다 — TTY 1개만 하드코딩하고, 렌더링
백엔드는 VGA 텍스트 모드(0xB8000) 하나만, procsrv의 계정 저장소는
최소 실제 저장소(평문 비교)로, 로그인 성공 후 session_program 스폰은
M20으로 미루기로 사용자가 확인했다.

### D1. `servers/drivers/console`(신설) — VGA 텍스트 콘솔

`sys_map_phys`로 물리주소 `0xB8000`(80x25, 셀당 2바이트)을 매핑한다
(PCI 장치가 아니라 QEMU q35의 레거시 VGA 호환 영역이라 devmgr 등록이
필요 없다). `OP_PRINT` 오퍼레이션 하나 — 개행/스크롤/backspace(커서를
물리고 그 자리를 지움)까지만 처리한다. ANSI/VT100 이스케이프 시퀀스는
범위 밖.

### D2. `servers/drivers/ps2` 확장 — 진짜 IPC 서버로 전환

M14의 self-test(컨트롤러 진단) 이후 종료하던 것을, `OP_READ_KEY`를
받아 계속 서비스하는 루프로 바꿨다. scancode set 1(US QWERTY, 소문자
전용 — Shift/Caps 미처리)을 ASCII로 번역하는 고정 테이블을 추가했다.

### D3. `servers/procsrv` 확장 — 처음으로 서버가 됨

고정 계정 배열(이름+평문 비밀번호, 2개) + `OP_LOGIN` 핸들러를
추가하고, 기존 self-test들이 끝난 뒤 `sys_recv` 루프에 진입하도록
바꿨다(M13~M16까지는 vfs/fat32/ext4의 클라이언트 역할만 했다).

### D4. `servers/login`(신설) — 로그인 프롬프트

console/ps2/procsrv 셋 모두에 의존한다(`depends=console,ps2,procsrv`).
사용자명 프롬프트에서 `OP_READ_KEY`를 정해진 횟수만큼 폴링해도 키가
하나도 없으면(자동화 환경, 결정적) 내장 자체 테스트 계정으로 같은
`OP_LOGIN` 경로를 그대로 호출한다 — 실제 사람이 타이핑하면 그 입력이
먼저 도착해 실제 경로(사용자명 읽기→비밀번호 읽기, 에코 없음→
`OP_LOGIN`)를 그대로 탄다.

### D5. 배선

`tools/mkbootdisk.py`의 `--depends=` 콤마 다중 의존 지원은 M16에서
이미 갖춰 뒀다 — `--depends=login:console,ps2,procsrv` 그대로 재사용.
전체 스폰 순서를 `memfs→devmgr→ps2→console→usb→virtio-blk→fat32→
ext4→vfs→procsrv→login`으로 확정(login이 셋 다 뒤여야 한다).
`tools/run-qemu.sh`에 `MINICORE_QEMU_DISPLAY=1`(opt-in, 기본은 여전히
`-display none`) 추가 — 사람이 실제 QEMU 창으로 로그인 프롬프트를
보려면 켠다.

## 실행 중 발견하고 고친 버그 (ADR-166 참고)

1. **initrun 서비스 레지스트리 크기 초과**: `k_max_registered_services`
   가 `8`로 고정돼 있었는데 M17까지 서비스가 11개로 늘어, 9번째(vfs)
   부터 레지스트리에 등록되지 않았다 — `procsrv`의 `depends=vfs`가
   조용히 핸들 0으로 실패해 `[procsrv] vfs open failed`(및 fat32/
   ext4도 동반 실패)로 재현됐다. `16`으로 늘려 고쳤다.
2. **ps2 `OP_READ_KEY` 폴링 예산 과다**: 포트 I/O 트랩 한 번이 QEMU
   에뮬레이션에서 실제로 비싼데, `2,000,000`회 폴링 × login의
   재시도 `50`회 = 최대 1억 회 트랩이 스모크 테스트 타임아웃(90초)을
   실제로 넘겼다(66개 중 콘솔/로그인 관련 어써션만 못 미침). ps2를
   `50,000`, login 재시도를 `10`으로 줄여(약 200배 절감) 고쳤다.

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh       # 66개 전부 PASS(M16의 63개 + 신규 3개) — 2회 연속 재확인, fat32/ext4 이미지 해시 불변
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌):

```
[console] vga init ok=1
[ps2] controller self-test ok=1
[procsrv] vfs write/read roundtrip ok=1
[procsrv] fat32 read ok=1
[procsrv] ext4 read ok=1
[login] no keyboard input, using self-test account
[login] auth ok=1
```

- **확인함**: 실제 키 입력이 없는 자동화 환경에서도 `login`이
  결정적으로 자체 테스트 계정 경로를 타고, 그 경로가 `procsrv`의
  실제 `OP_LOGIN` 인증 로직을 그대로 검증함.
- **확인함**: 두 회귀 버그(레지스트리 크기, 폴링 예산) 모두 수정
  후 2회 연속 안정적으로 통과 — 처음엔 "가끔 나는 문제"처럼 보였던
  타이밍 버그가 실제로는 결정적 원인이었음을 확인.
- **알려진 단순화(M17 범위 밖으로 명시적으로 남긴 것)**: 멀티 TTY/TTY
  전환(cfgsrv 등장 이후), 그래픽 프레임버퍼 백엔드, ANSI/VT100
  이스케이프 시퀀스, 실제 비밀번호 해싱, 로그인 성공 후
  `session_program` 스폰(M20이 "로그인 후 셸"을 맡는다).

## 다음 단계

M17은 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M18
(보안 모델: 신뢰 위임 체인 + su/sudo + jail/guest).
