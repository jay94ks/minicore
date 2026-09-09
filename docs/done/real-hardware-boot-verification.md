# 완료 보고: 실제 GRUB Multiboot2 부팅 경로 최초 검증 (마일스톤 외 확인 작업)

**대상**: 어떤 `docs/plan/*.md` 마일스톤에도 속하지 않는다 —
`system-servers-bringup.md`의 M12~M20이 이미 전부 완료된 뒤, 사용자가
"도전 실기 부팅 확인해보자(GRUB Multiboot2 경로)"로 요청한 별도
확인 작업이다.
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md)
ADR-017(Multiboot2/UEFI 이중 지원), ADR-114(QEMU PVH 직접 부팅 —
"실제 GRUB 기반 검증은 여전히 하지 않은 상태로 남는다"고 명시했던
바로 그 갭), ADR-173(이번에 실제로 검증하며 발견·수정한 레거시
8259 PIC 버그)
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
  번째 진입 방식)도 이번 확인 대상이 아니다.

## 다음 단계

이 확인 작업에 뒤따르는 별도 계획은 없다 — ADR-114가 남겨 둔 갭
중 "GRUB를 구할 수 있는 환경에서의 검증"은 이것으로 해소됐다.
남은 것(물리 실기, UEFI 경로)은 필요해지는 시점에 별도로 다룬다.
