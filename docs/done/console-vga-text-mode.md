# 완료 보고: 콘솔 드라이버 실제 VGA 텍스트 모드 하드웨어 초기화 (마일스톤 외 확인/구현 작업)

**대상**: 어떤 `docs/plan/*.md` 마일스톤에도 속하지 않는다 —
musl-userland-porting.md(M51~M56)가 전부 완료된 뒤, 사용자가 "부팅된
결과물을 보여줘" → "QEMU 창을 직접 보여줘" → "VGA 카드를 실제로
표준 텍스트 모드로 세팅하는 걸 개발해야지"로 요청을 구체화한 별도
작업이다.
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md)
ADR-230, [open-items.md](../design/open-items.md) OPEN-77(신규)
**실행일**: 2026-09-11

## 배경

이 커널은 실제 BIOS(INT 10h)나 GRUB을 거치지 않고
`tools/run-qemu.sh`가 `-bios qboot.rom -kernel ...`(최소 PVH 스텁)
으로 곧바로 부팅한다. M1부터 지금까지 모든 검증이 디버그 시리얼
콘솔로만 이뤄져, VGA 카드 자신을 실제로 "80x25 텍스트 모드"로
세팅해 주는 존재가 원래부터 없었다는 사실이 한 번도 드러난 적이
없었다 — `servers/drivers/console`(M17, ADR-164)은 물리주소
0xB8000에 문자+속성 바이트를 쓰기만 하면 화면에 보인다고 가정했다.
사용자가 실제 QEMU 창으로 로그인 프롬프트를 보길 원하면서 이 가정이
깨졌다 — 화면엔 "Guest has not initialized the display (yet)."만
떴다.

## 한 것

### D1. VGA 레지스터 직접 프로그래밍으로 텍스트 모드 3 활성화

`servers/drivers/console/main.cpp`에 `set_text_mode_3()`을 신설해
실제 VGA BIOS의 INT 10h AH=00h AL=03h와 같은 레지스터 시퀀스
(Miscellaneous Output→Sequencer→CRTC→Graphics Controller→Attribute
Controller, FreeVGA 표준값)를 포트 I/O로 재현했다. `sys_io_activate`
(ps2 드라이버가 이미 쓰는 패턴)로 0x3B0~0x3DF 포트 권한을 얻는다.
검증: QEMU 해상도가 720x400(80x25, 문자당 9x16)으로 정확히 바뀜을
확인 — 모드 세팅 자체는 성공.

### D2. DAC 팔레트 로드

Attribute Controller는 속성 니블을 DAC 인덱스로만 매핑할 뿐 실제
RGB 값은 정의하지 않는다는 것을 발견 — DAC(0x3C8/0x3C9)에 EGA/VGA
표준 16색 팔레트를 직접 로드했다. 검증: 속성 0x4F(빨간 배경) 임시
테스트 바를 화면 첫 줄에 그려 실제로 빨간색이 보임을 확인.

### D3. 글꼴(글리프 비트맵) 로드

문자 셀의 실제 모양은 VRAM "플레인 2"에서 읽히는데, 이 커널에는 그
데이터를 넣어 줄 BIOS가 없어 전부 0(빈 칸)이었다. `load_font()`를
신설해 표준 "plane-2 직접 접근" 트릭으로 물리주소 0xA0000(신규
`sys_map_phys` 매핑, 8KiB)에 폰트 데이터를 썼다. `servers/login`이
실제로 쓰는 ~20글자(공백, `:`, `a,c,d,e,f,g,i,l,m,n,o,r,s,t,u,w,P,L`)
만 손으로 그린 8x8 비트맵으로 담고(`k_glyphs[]`), CRTC Maximum Scan
Line(16줄/문자)에 맞춰 각 행을 스캔라인 두 줄씩 복제했다.

## 실행 중 발견

1. **VGA 레지스터 값만 바꿔서는 문자 생성기에 반영되지 않는다** —
   처음 SEQ0 Synchronous Reset 없이 SEQ4=0x06으로 폰트를 썼을 때,
   "같은 상태에서 즉시 읽어보면" 정확히 일치했지만(자기 자신과의
   자기 일치일 뿐 실제 렌더링과는 무관) 화면에는 전혀 반영되지
   않았다. FreeVGA류 표준 절차가 요구하는
   `SEQ0=0x01(리셋)→레지스터 변경→SEQ0=0x03(재시작)` 감싸기 없이는
   Memory Mode 변경이 문자 생성기의 실제 읽기 경로에 반영되지
   않는다는 것과, SEQ4 값 자체도 0x06이 아니라 0x07이어야 한다는
   것을 뒤늦게 확인했다.
2. **가장 심각한 발견 — `sys_map_phys`의 고정 가상주소 슬롯 재사용**:
   `kernel/arch/x86_64/process_ops.cpp::map_phys`(M14, ADR-007/
   038/039)는 프로세스당 고정 가상주소 슬롯 하나(`k_mmio_user_vaddr`)
   만 재사용한다 — 새 `sys_map_phys` 호출마다 이전 매핑을 조용히
   다른 물리주소로 덮어쓴다. 0xB8000(텍스트 버퍼)을 먼저 매핑해
   `g_vga`에 저장해 두고 그 다음 0xA0000(폰트 로드 창)을 매핑했는데,
   이 두 번째 호출이 `g_vga`가 가리키던 가상주소를 조용히 0xA0000
   물리 페이지로 바꿔 버려, 그 뒤 화면 지우기/로그인 텍스트 출력
   등 `g_vga`를 통한 모든 쓰기가 실제 VGA 텍스트 버퍼(0xB8000)에는
   전혀 도달하지 못했다. 모드 세팅·DAC 팔레트·폰트 쓰기 각각은
   전부 정상이었는데도(레지스터 읽기/화면 해상도/자기 읽기-쓰기로
   각각 확인됨) 최종 화면은 완전히 검은 채로 남아 원인 파악이
   오래 걸렸다 — 커널 소스에서 `k_mmio_user_vaddr`가 상수 하나뿐
   이라는 것을 직접 확인하고서야 알아냈다. 해결: 폰트 로드(임시
   매핑)를 먼저 끝내고 **그 다음** 0xB8000을 매핑하도록 순서를
   바꿔, 마지막(=계속 살아있는 `g_vga`) 매핑이 항상 올바른
   물리주소를 가리키게 했다.
3. 디버깅 도구 부재 — 이 Windows/MSYS2 환경엔 ImageMagick/PIL이
   없어, QEMU 모니터 `screendump`가 실제로 내놓는 raw PPM(P6, 이
   QEMU 빌드는 libpng 없이 `.png` 확장자를 줘도 PPM을 쓴다)을 PNG로
   바꿔 보기 위해 stdlib(`zlib`+`struct`)만으로 PPM→PNG 변환 스크립트
   (`ppm2png.py`)와 영역 크롭+확대 스크립트(`crop.py`)를 직접
   작성했다. QEMU 모니터 명령은 `-monitor tcp:127.0.0.1:<port>,
   server,nowait`로 띄운 뒤 bash `/dev/tcp/...` 가상 디바이스로
   보냈다 — 경로에 백슬래시(`C:\...`)를 쓰면 모니터의 readline이
   `\`를 다음 글자와 함께 삼켜 버그가 나 슬래시(`C:/...`) 경로를
   써야 했다.
4. 최초 QEMU 재현 시도(별도 임시 `-vnc`/`-monitor`만 붙인 명령)에서
   커널 패닉(`kern::sched::block: no runnable thread (deadlock)`)이
   났으나, `tools/smoke-test-x86_64.sh`가 기본으로 붙이는
   `-device qemu-xhci`/testdisk/fat32disk/ext4disk 장치들을 빠뜨린
   것이 원인이었다(장치 구성이 달라지면 부팅 경로/타이밍이 달라져
   기존에 알려진 job-control 자기테스트의 타이밍 취약성이 드러난
   것으로 보인다) — 이 스크립트가 붙이는 전체 장치 구성을 그대로
   재현하니 재발하지 않았고, 실제 회귀 스위트(`smoke-test-x86_64.sh`
   자체)는 매 라운드 전부 정상 통과했다. 이 커널 자체의 새로운
   버그는 아니라고 판단했다.

## 검증

QEMU를 `-display none -vga std`+모니터 TCP 소켓으로 띄워 부팅
직후(그리고 로그인 완료 후) `screendump`로 캡처한 PPM을 PNG로
변환해 확인했다 — "minicore login: Login successful" 글자가 실제로
육안으로 읽히는 모양(균일한 색 블록이 아니라 실제 글리프 형태)으로
렌더링됨을 확인했다. 이후 표준 회귀 절차(`MINICORE_QEMU_BIN` 설정,
5개 스위트 전부):

- `tools/smoke-test-x86_64.sh`: PASS 147, FAIL 0
- `tools/smoke-test-smp-x86_64.sh`: 전부 PASS
- `tools/smoke-test-numa-x86_64.sh`: 전부 PASS
- `tools/smoke-test-avx-x86_64.sh`: 전부 PASS
- `tools/smoke-test-net-x86_64.sh`: 전부 PASS

## 범위 밖으로 남긴 것

- 폰트는 로그인 프롬프트가 실제로 쓰는 ~20글자만 담는다 — 더 넓은
  문자 집합이 필요해지면 OPEN-77로 추적한다.
- 그래픽 프레임버퍼 백엔드, 하드웨어 텍스트 커서 깜빡임 제어,
  다중 폰트 뱅크(밑줄/블링크 속성)는 ADR-164가 이미 범위 밖으로
  남긴 그대로 손대지 않았다.
- 실제 GRUB/UEFI 부팅 경로(BIOS가 존재하는 경로)에서는 이 드라이버가
  이미 세팅된 모드를 다시 세팅하려 들 수 있다는 것은 고려하지
  않았다 — 이 프로젝트의 실제 배포 경로(Multiboot2/UEFI)에 대한
  영향은 범위 밖(현재 개발 반복 경로인 qboot.rom 한정으로 검증).

## 다음

이 확인 작업은 그 자체로 완결됐다 — 더 이어지는 마일스톤은 없다.
사용자가 다음 방향(aarch64 이식, 또는 다른 확장)을 정하면 그때
새 계획을 `docs/plan/`에 남긴다.
