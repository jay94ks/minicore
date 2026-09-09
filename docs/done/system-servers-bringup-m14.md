# 완료 보고: system-servers-bringup M14 — devmgr + PCIe ECAM 열거 + PS/2 + USB(xHCI) 리셋

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M14
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md)
ADR-154(스레드별 IOPB)/ADR-156(sys_map_phys)/ADR-157(devmgr/PS2/USB 구현)
**실행일**: 2026-09-09

## 완료한 것

### D1. 선행 커널 작업 — ADR-154(스레드별 IOPB) + ADR-156(sys_map_phys)

TSS IOPB를 코어당 전역 1회성에서 스레드별 활성 I/O 포트 범위로
재설계(`sys_io_activate`/`sys_io_deactivate`, 컨텍스트 스위치 시 diff
기반 재프로그래밍). 새 syscall `sys_map_phys` — ADR-007/038/039가
예고했던 "MMIO 캐패빌리티"를 실제로 도입(trusted 프로세스가 임의의
기존 물리주소 범위를 그대로 매핑, 프레임 참조 카운트 미사용).

### D2. `servers/devmgr` — 유저랜드 ACPI/PCIe ECAM 열거

`kernel/arch/x86_64/acpi.cpp`의 RSDP 검색+MCFG 파싱을 유저랜드용으로
다시 구현(devmgr는 커널 코드를 호출할 수 없다, ADR-006/039). bus 0의
32개 device를 ECAM으로 열거해 vendor/device/class를 로그로 남긴다.
`register_driver` IPC(label=1, regs[]만 사용)로 드라이버가 vendor:device
또는 class code로 매칭을 요청하면, 그 자리에서 BAR0을 배정(ADR-147의
I/O BAR 배정을 메모리 BAR로 일반화)하고 물리주소/크기를 응답으로
돌려준다.

### D3. `servers/drivers/ps2` — 8042 컨트롤러 자체 테스트

실제 키 입력이 QEMU 자동화 환경에 주입되지 않으므로, 컨트롤러 자체
테스트(커맨드 0xAA → 결정적으로 0x55)로 검증 가능한 결과를 얻는다.
`sys_io_activate`로 포트 0x60~0x64를 스스로 활성화한다.

### D4. `servers/drivers/usb` — xHCI 리셋 + 포트 상태 스캔

devmgr에 PCI 클래스 코드 `0x0C0330`(xHCI)로 등록해 BAR0을 위임받고,
`sys_map_phys`로 매핑해 캐패빌리티 레지스터를 읽은 뒤 **컨트롤러
리셋(USBCMD.HCRST)**을 실제로 수행, USBSTS.CNR이 꺼질 때까지 기다려
각 포트의 PORTSC를 로그로 남긴다.

### D5. 배선

`tools/mkbootdisk.py --trusted=<이름>`, `init/initrun/main.cpp`의
스폰 루프가 `trusted=1`/`depends=`를 읽어 처리하고 devmgr에게는
`boot_info.arch_data_addr`를 실제 argv로 넘긴다. `tools/run-qemu.sh`
`MINICORE_QEMU_XHCI=1`로 QEMU 기본 xHCI 컨트롤러를 붙인다.

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh       # 58개 전부 PASS(M13의 52개 + 신규 6개)
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌):

```
[devmgr] mcfg ecam_base=0xb0000000        ← 커널 자신의 독립 MCFG 파싱과 정확히 일치(교차검증)
[devmgr]   vendor=0x1b36 device=0xd class=0xc0330   ← QEMU qemu-xhci
[devmgr] device_count=0x6
[ps2] self-test result=0x55
[ps2] controller self-test ok=1
[usb] xhci bar_phys=0xe0000000 bar_size=0x4000
[usb] xhci hci_version=0x100 max_slots=0x40 max_ports=0x8
[usb] xhci hcrst_done=0x1
[usb] xhci controller_ready=0x1
[usb] portsc=0x202a0 (×8)
[usb] xhci reset+port scan done
```

- **확인함**: devmgr가 실제 ACPI 테이블에서 독립적으로 MCFG를 찾아
  얻은 ECAM 베이스가 커널 자신의(다른 목적의) 파싱 결과와 정확히
  일치함 — 유저랜드 구현이 올바르다는 교차 검증.
- **확인함**: ps2/usb 드라이버가 실제 하드웨어 레지스터(I/O 포트,
  MMIO)에 직접 접근해 결정적인 결과(self-test 0x55, xHCI 리셋 성공)
  를 얻음 — ADR-154/156이 실전에서 동작함을 증명.
- **확인하지 못함**(사용자 확인 하에 명시적으로 범위 밖): 실제 USB
  장치 열거(SET_ADDRESS/GET_DESCRIPTOR)와 HID 클래스 드라이버,
  PCI-PCI 브리지 재귀, 핫플러그/PME notification.

## 다음 단계

M14는 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M15
(virtio-blk 드라이버 — 첫 실제 유저 드라이버). USB 장치 열거+HID는
별도 후속 라운드로 남는다(사용자 요청 시 재개).
