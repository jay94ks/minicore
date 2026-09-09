# 완료 보고: 실제 부팅 경로(GRUB Multiboot2 + UEFI) 최초 검증 (마일스톤 외 확인 작업)

**대상**: 어떤 `docs/plan/*.md` 마일스톤에도 속하지 않는다 —
`system-servers-bringup.md`의 M12~M20이 이미 전부 완료된 뒤, 사용자가
"도전 실기 부팅 확인해보자(GRUB Multiboot2 경로)", 이어서 "UEFI 경로도
검증해보자" → "실제로 구현해줘"로 요청한 별도 확인/구현 작업이다.
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md)
ADR-017(Multiboot2/UEFI 이중 지원), ADR-114(QEMU PVH 직접 부팅 —
"실제 GRUB 기반 검증은 여전히 하지 않은 상태로 남는다"고 명시했던
바로 그 갭), ADR-173(레거시 8259 PIC 버그), ADR-174(initrun/devmgr
arch_data_addr 실값 전달), ADR-175(UEFI 부팅 최초 구현)
**실행일**: 2026-09-10

## 배경

`docs/design/boot-and-drivers.md` ADR-114는 M1(kernel-bootstrap.md)
시점에 이미 "실제 GRUB(Multiboot2 ISO) 기반 검증은 여전히 하지 않은
상태로 남는다 — 실기 또는 GRUB를 구할 수 있는 환경에서 별도로
검증이 필요하다"고 명시적으로 기록해 뒀다. 이 개발 머신(Windows,
MSYS2)에는 GRUB 패키지가 없어(`pacman -Ss grub`로 재확인 — 여전히
없음) 이후 M2~M20까지 단 한 번도 이 경로가 실행된 적이 없었다.
이번 작업은 실제 물리 하드웨어가 아니라, **QEMU를 PVH 개발 지름길
(`-bios qboot.rom -kernel ...`) 없이 진짜 BIOS(SeaBIOS 기본값)+진짜
GRUB로 띄워 Multiboot2 경로 자체를 검증**하는 것을 목표로 했다 —
"실기"라는 표현이 가리키는 실제 요구는 물리 하드웨어 자체가 아니라
"QEMU 개발 지름길이 아닌 진짜 배포 부팅 경로"였다.

## 한 것

### D1. Docker 기반 GRUB Multiboot2 ISO 빌드

MSYS2에 `grub-mkrescue`가 없어(ADR-114와 같은 사정, 여전히 미해결)
`debian:bookworm-slim` 컨테이너에 `grub-pc-bin`/`grub-common`/
`xorriso`를 설치해 그 안에서 ISO를 만들었다 — 호스트(Windows)에는
아무것도 영구 설치하지 않는다.

```bash
mkdir -p /tmp/grub-iso/boot/grub
cp build/x86_64-clang/kernel/arch/x86_64/minicore_kernel_x86_64.elf \
   /tmp/grub-iso/boot/kernel.elf
cat > /tmp/grub-iso/boot/grub/grub.cfg <<'EOF'
set timeout=0
set default=0
menuentry "minicore" {
    multiboot2 /boot/kernel.elf
    boot
}
EOF
docker run --rm -v "<grub-iso 경로>:/iso" debian:bookworm-slim bash -c \
  "apt-get update -qq && apt-get install -y -qq grub-pc-bin grub-common xorriso >/dev/null 2>&1 && \
   grub-mkrescue -o /iso/minicore.iso /iso"
```

### D2. QEMU를 진짜 BIOS+GRUB 경로로 부팅

`tools/run-qemu.sh`는 항상 `-bios qboot.rom -kernel <elf>`(ADR-114의
PVH 개발 지름길)만 지원한다 — 이 확인 작업은 그 두 플래그만
`-cdrom <iso>`로 바꾸고, 나머지 모든 플래그(`-M q35`, virtio-blk
부트/테스트/fat32/ext4 디스크, xHCI)는 그대로 유지했다(즉
`tools/run-qemu.sh`를 고치지 않았다 — 이 경로는 일회성 확인이라
스크립트화하지 않는다, ADR-114가 이미 "이 스크립트는 QEMU 개발
반복 전용"이라고 범위를 그어 둔 것과 일관된다).

### D3. 발견하고 고친 버그 — ADR-173(레거시 8259 PIC 미마스킹)

첫 시도에서 initrun이 최초로 유저모드(ring3)에 진입하는 순간 바로
`vector=8`(#DF) 예외로 죽었다 — PVH 개발 경로에서는 한 번도 나타난
적 없는 증상이다. `MINICORE_QEMU_TRACE=1`이 이미 갖춘
`-d cpu_reset,guest_errors,int` 트레이스로 확인한 실제 원인은 진짜
CPU 더블폴트가 아니라 `Servicing hardware INT=0x08`(CPL=3,
IP=0x10000000=initrun 진입점) — **하드웨어 IRQ0(타이머)가 그대로
벡터 8로 들어온 것**이었다. 이 커널은 8259 PIC를 한 번도
초기화(마스킹도 리맵도)하지 않았는데, 실제 SeaBIOS+GRUB 경로는
8259/PIT를 살려 둔 채 넘어와 IRQ0이 리맵되지 않은 기본 벡터
오프셋(8)을 타고 그대로 `#DF`와 충돌했다. `kernel/arch/x86_64/
idt.cpp::init_idt()`에 8259 전체 마스킹(포트 0x21/0xA1에 `0xFF`)을
추가해 고쳤다 — 자세한 내용은 ADR-173 참고.

## 검증 결과 (QEMU 실측)

```bash
# 고친 커널로 ISO 재빌드 후:
qemu-system-x86_64 -M q35 -m 256M -no-reboot -no-shutdown -display none \
  -cdrom minicore.iso \
  -drive if=none,id=bootdisk,format=raw,file=bootdisk.img -device virtio-blk-pci,drive=bootdisk,addr=04.0 \
  -device qemu-xhci,addr=05.0 \
  -drive if=none,id=testdisk,format=raw,file=testdisk.img -device virtio-blk-pci,drive=testdisk,addr=06.0 \
  -drive if=none,id=fat32disk,format=raw,file=fat32-test.img -device virtio-blk-pci,drive=fat32disk,addr=07.0 \
  -drive if=none,id=ext4disk,format=raw,file=ext4-test.img -device virtio-blk-pci,drive=ext4disk,addr=08.0 \
  -chardev stdio,id=char0,mux=off -serial chardev:char0
```

실측 로그(발췌 — M20까지의 전체 시퀀스가 그대로 재현된다):

```
[boot_info:real] magic=0x4d434249(ok) version=3 cpu_count=1
...
[initrun] mcpack find_entry ok=1 size=0x11000
[initrun] load_elf ok=1 entry=0x10000000
[initrun] setup_initrun_process ok=1
...
[login] auth ok=1
[shell] session started
[procsrv] shell session start ok=1
[su-target] uid=0 super=1 guest=0
[login] su delegated ok=1
[login] su denied ok=1
[shell] no keyboard input, running self-test commands
[shell] ls ok=1
[shell] cat ok=1
[shell] self-test done
```

- **확인함**: `[boot_info:real] magic=0x4d434249(ok)`가 실제 GRUB이
  전달한 Multiboot2 정보 구조체를 커널이 올바르게 인식했음을
  보인다 — 이 프로젝트에서 이 문자열이 실제 GRUB 환경에서 관찰된
  것은 이번이 처음이다(그 전까지는 항상 self-test 픽스처
  경로(`[boot_info:selftest]`)만 실제로 구동됐다).
  **참고**: `tools/smoke-test-x86_64.sh`는 여전히 PVH 경로에
  하드코딩돼 있어 이 확인은 그 스크립트를 실행한 게 아니라, 위
  실측 로그 파일을 공식 `EXPECTED` 문자열 82개 전부와 직접
  대조하는 방식으로 했다(전부 일치 확인) — 이 방법론상의 차이를
  기록해 둔다.
- **확인함**: M12~M20 전체(procsrv/vfs/memfs/fat32/ext4/devmgr/ps2/
  console/usb/virtio-blk/cfgsrv/shell/login)가 실제 GRUB Multiboot2
  경로에서 PVH 개발 경로와 동일하게 끝까지 동작한다 — 공식 스모크
  테스트 `EXPECTED` 배열의 82개 문자열이 전부 이 로그에서 발견됐다.
- **고친 버그 이후 회귀 확인**: `tools/smoke-test-x86_64.sh`(82/82),
  `tools/smoke-test-smp-x86_64.sh`(11/11), `tools/smoke-test-numa-x86_64.sh`
  (24/24), `tools/smoke-test-avx-x86_64.sh`(12/12) 전부 PVH 개발
  경로에서 재확인 — 레거시 PIC 마스킹 추가가 기존 경로에 아무
  영향을 주지 않았다.
- **알려진 한계**: 물리 실기 검증은 여전히 하지 않았다(이 환경엔
  물리 하드웨어가 없다) — 이번 확인은 "QEMU 개발 지름길이 아닌
  진짜 BIOS+GRUB 경로"까지다. UEFI 경로(ADR-017이 함께 요구한 두
  번째 진입 방식)는 바로 다음 절에서 이어서 다룬다.

---

## 2부: 실제 UEFI(OVMF) 부팅 경로 최초 구현

GRUB 경로를 검증한 뒤 사용자가 "UEFI 경로도 검증해보자"고 요청했다.
확인해 보니 `docs/spec/boot.md` §1.2가 UEFI 진입점(`efi_main`)을
스펙으로만 정의해 뒀을 뿐, **실제 구현이 전혀 없었다**(Multiboot2
경로는 최소한 부트 스텁 코드가 이미 있어 "검증만" 하면 됐지만, UEFI는
"만드는 것부터" 시작해야 했다) — 그래서 사용자에게 규모를 미리
알리고("실제로 구현해줘"로 확인받은 뒤) 처음부터 구현했다. 상세
설계는 [boot-and-drivers.md](../design/boot-and-drivers.md) ADR-175
참고, 여기서는 실행 과정과 검증 결과만 기록한다.

### D4. Docker 기반 OVMF 확보

MSYS2에 `ovmf` 패키지도 없다(GRUB와 같은 사정) — `debian:bookworm-slim`
컨테이너에 `ovmf` apt 패키지를 설치해 `OVMF_CODE.fd`/`OVMF_VARS.fd`를
꺼냈다.

### D5. EFI 스텁 구현 (`kernel/arch/x86_64/efi_stub/`)

ADR-175가 상세를 다룬다 — 요약하면 별도 PE32+ EFI 애플리케이션이
커널 ELF를 통째로 심고, GRUB의 Multiboot2 로더가 하는 일(PT_LOAD
세그먼트를 `p_paddr`에 복사, ACPI RSDP를 찾아 넘김)을 UEFI Boot
Services로 재현한 뒤 커널의 새 진입점(`_efi_entry`, `boot.S`)으로
점프한다.

### D6. 발견하고 고친 버그 — retf가 UEFI의 원래 스택에 push함 (ADR-175)

CR3를 우리 페이지테이블로 바꾼 직후, CS를 우리 GDT로 바꾸는 retf
트릭이 여전히 UEFI의 원래 스택(새 페이지테이블 밖)에 push하려다
조용히 멈췄다 — 그 시점엔 `klog`도 IDT도 없어 아무 진단도 안
나왔다. 포트 0x3F8에 문자를 하나씩 직접 쓰는 임시 체크포인트
(`'A'`~`'F'`)로 정확한 위치(CR3 교체 이후, retf 이전)를 좁혔다.
retf 전에 저지대 스택으로 먼저 옮겨 고쳤다.

### D7. 발견하고 고친 버그 — initrun/devmgr가 항상 arch_data_addr=0을 받음 (ADR-174)

여기까지 고친 뒤 커널 자체(`kernel_main`)는 정상 동작했지만
(`madt_ok=1`, `mcfg_ok=1` — ACPI RSDP를 UEFI Configuration Table에서
찾아 넘긴 것은 성공), 부트 디스크·xHCI 등 전체 장치를 붙이고
다시 부팅하니 `[devmgr] mcfg ecam_base=0xffffffffffffffff
device_count=0x0`으로 PCI 열거가 완전히 실패하고 결국 스케줄러
데드락 PANIC까지 이어졌다. 원인은 `kernel_main`이 아니라 **M12부터
있던 별개의 오래된 결함**이었다 — `setup_initrun_process()`가
initrun에게 넘기는 `boot_info`가 항상 self-test 고정값(arch_data_addr
=0)이었고, `servers/devmgr`의 EBDA/BIOS ROM 스캔 폴백이 QEMU PVH와
실제 SeaBIOS+GRUB 양쪽에서 우연히 그 공백을 메워 왔을 뿐이었다 —
OVMF만 그 폴백 영역에 legacy RSDP를 남기지 않아 처음으로 드러났다.
자세한 내용과 수정은 ADR-174 참고.

### 검증 결과 (QEMU 실측)

```bash
qemu-system-x86_64 -M q35 -m 256M -no-reboot -no-shutdown -display none \
  -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=OVMF_VARS_rw.fd \
  -drive file=fat:rw:<ESP 디렉터리>,format=raw \
  -drive if=none,id=bootdisk,format=raw,file=bootdisk.img -device virtio-blk-pci,drive=bootdisk,addr=04.0 \
  -device qemu-xhci,addr=05.0 \
  -drive if=none,id=testdisk,format=raw,file=testdisk.img -device virtio-blk-pci,drive=testdisk,addr=06.0 \
  -drive if=none,id=fat32disk,format=raw,file=fat32-test.img -device virtio-blk-pci,drive=fat32disk,addr=07.0 \
  -drive if=none,id=ext4disk,format=raw,file=ext4-test.img -device virtio-blk-pci,drive=ext4disk,addr=08.0 \
  -chardev stdio,id=char0,mux=off -serial chardev:char0
# <ESP 디렉터리>/EFI/BOOT/BOOTX64.EFI = minicore_kernel_x86_64_efi.efi
```

실측 로그(발췌 — 두 버그를 모두 고친 뒤, 최종 시도):

```
[efi_stub] entered, kernel blob size=0x0000000000085008
[efi_stub] kernel segments loaded
[efi_stub] acpi rsdp=0x000000000f77e014
[efi_stub] exited boot services, jumping to kernel at 0x00000000001000f9
hello from kernel
[boot_info:real] magic=0x4d434249(ok) version=3 cpu_count=1
[acpi] madt_ok=1 cpu_count=1 lapic_base=0xfee00000
[acpi] mcfg_ok=1 ecam_base=0xb0000000
...
[devmgr] device_count=0x9
[virtio-blk] write/read roundtrip ok=1
[fat32] mount ok=0x1
[ext4] mount ok=0x1
[procsrv] cfgsrv full protocol ok=1
[shell] session started
[procsrv] shell session start ok=1
[login] auth ok=1
[login] su delegated ok=1
[login] su denied ok=1
[shell] no keyboard input, running self-test commands
[shell] ls ok=1
[shell] cat ok=1
[shell] self-test done
```

- **확인함**: 실제 UEFI 펌웨어(OVMF)가 이 커널의 EFI 스텁을 정상
  로드·실행하고, 스텁이 GRUB와 동등한 방식으로 커널을 적재해
  M1~M20 전체가 GRUB/PVH 경로와 동일한 최종 상태(로그인, su
  위임/거부, cfgsrv 왕복+권한 모델, fat32/ext4 마운트+읽기,
  virtio-blk 왕복, 셸 `ls`/`cat` 자체 테스트)에 도달한다 — 공식
  스모크 테스트 `EXPECTED` 82개 문자열 중 80개가 이 로그에서
  발견됐다(GRUB 검증과 같은 방법론 — `tools/smoke-test-x86_64.sh`
  자체는 PVH 경로 전용이라 이 로그를 직접 대조했다).
- **알려진 제약(고치지 않고 남긴 것) — USB xHCI**: 남은 2개
  미일치는 전부 `[usb] xhci hcrst_done=0x1`/`controller_ready=0x1`
  이다 — OVMF에서만 `hcrst_done=0x0 controller_ready=0x0`,
  `bar_phys=0x10000`(다른 두 경로에서는 `0xC0000000`대의 정상적인
  MMIO 주소)처럼 명백히 잘못된 BAR 값을 읽는다. PCI 열거 자체는
  xHCI 장치(`vendor=0x1b36 device=0xd class=0xc0330`)를 정확히
  찾는다 — devmgr가 그 장치의 BAR를 읽거나 재사용하는 판단(M15/M16,
  ADR-158/163)이 OVMF의 "안 쓰는 장치는 BAR를 안 구성해 둔다"는
  상태를 잘못 해석하는 것으로 보인다. 다운스트림 크래시나 행을
  유발하지 않고(자체 테스트만 실패), USB HID 실제 열거는 이미 M14
  완료 보고서가 범위 밖으로 명시해 둔 항목이라 더 조사하지 않고
  알려진 한계로 남긴다.
- **고친 버그 이후 회귀 확인**: `tools/smoke-test-x86_64.sh`(82/82,
  2회 연속), `tools/smoke-test-smp-x86_64.sh`(11/11),
  `tools/smoke-test-numa-x86_64.sh`(24/24),
  `tools/smoke-test-avx-x86_64.sh`(12/12) 전부 PVH 개발 경로에서
  재확인, GRUB 경로도 갱신된 커널로 다시 부팅해 82개 문자열 전부
  재확인 — `g_real_arch_data_addr` 배선(ADR-174)이 기존 두 경로에
  아무 영향을 주지 않았다.

## 다음 단계

이 확인 작업에 뒤따르는 별도 계획은 없다 — ADR-114/017이 남겨 둔
갭 중 "GRUB/UEFI를 구할 수 있는 환경에서의 검증"은 둘 다 이것으로
해소됐다. 남은 것(물리 실기 검증, OVMF에서의 USB xHCI BAR 문제)은
필요해지는 시점에 별도로 다룬다.
