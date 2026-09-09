# 완료 보고: system-servers-bringup M15 — virtio-blk 드라이버 (첫 실제 유저 드라이버)

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M15
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md) ADR-158
**실행일**: 2026-09-09

## 완료한 것

### D1. `servers/drivers/virtio-blk` — legacy virtio-blk 클라이언트

`init/initrun/virtio_blk.cpp`(부트 디바이스 전용)와 레지스터 레이아웃은
같지만 독립적으로 구현한 유저 드라이버. devmgr에 vendor:device
`0x1af4:0x1001`로 등록해 BAR를 위임받고, 3-디스크립터 체인(header→
data→status) 하나를 재사용해 `VIRTIO_BLK_T_IN`(읽기)/`VIRTIO_BLK_T_OUT`
(쓰기)을 모두 수행한다.

### D2. devmgr BAR 재사용 일반화 — `assign_or_get_bar0`

`assign_memory_bar0`을 `assign_or_get_bar0`으로 확장 — I/O BAR와 메모리
BAR를 모두 다루고, 요청받은 BAR가 이미 배정돼 있으면(0이 아니면)
재배정하지 않고 그대로 반환한다. virtio-blk 드라이버가 매칭하는 장치의
BAR는 initrun이 부팅 중 이미 배정해 둔 상태(ADR-147)라 필요했다.

### D3. 부트 디바이스 배제 — `devmgr_argv.boot_bdf`

devmgr의 `_start` argv를 `{arch_data_addr, boot_bdf}`로 확장해 initrun이
부트 디바이스의 BDF를 함께 넘긴다. `handle_register_driver`가 후보
장치의 BDF가 `boot_bdf`와 같으면 매칭에서 제외한다.

### D4. 배선

`servers/drivers/CMakeLists.txt`/`servers/CMakeLists.txt`에 virtio-blk
드라이버 빌드/부트디스크 배선 추가(`--depends=virtio-blk:devmgr`,
`--trusted=virtio-blk`). `tools/run-qemu.sh`에 `MINICORE_QEMU_TESTDISK`
(bus0/device6, 부트 디바이스와 분리된 스크래치 디스크) 추가.

## 실행 중 발견하고 고친 버그 (ADR-158 참고)

1. **디스크 손상 버그**: 최초 구현은 virtio-blk 드라이버가 부트
   디바이스에 그대로 등록해 쓰기 테스트를 했다 — `-drive format=raw`가
   기본 read-write라 그 쓰기가 호스트 `bootdisk.img`의 cpio 아카이브를
   실제로 덮어썼고, CMake가 ELF 입력 불변 시 재빌드하지 않아 손상된
   이미지가 재사용돼 이후 모든 실행에서 procsrv가 `#UD`로 100% 재현
   크래시했다. 별도 테스트 디스크(D4) + devmgr BDF 배제(D3)로 이중
   방어해 고쳤다.
2. **`avail_idx` 리셋 버그**: 매 요청마다 avail_idx를 1로 되돌리는
   구현은 두 번째 요청(쓰기 다음 읽기)에서 디바이스가 "새 작업 없음"
   으로 보고 무시해 항상 타임아웃했다 — 큐가 살아있는 동안 계속
   증가하는 `g_avail_idx`/`g_queue_size` 전역으로 고쳤다.

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh       # 59개 전부 PASS(M14의 58개 + 신규 1개) — 분리된 테스트 디스크로 2회 연속 재확인
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌):

```
[pci] assign_virtio_blk_bar ok=1 vendor=0x1af4 device=0x1001   ← 부트 디바이스(initrun 직접 배정)
[virtio-blk] write_ok=0x1
[virtio-blk] read_ok=0x1
[virtio-blk] write/read roundtrip ok=1                         ← 별도 테스트 디바이스(devmgr 매칭+재배정)
```

- **확인함**: 별도 테스트 디스크를 2회 연속 재사용해도(수정 전에는
  재실행마다 100% 크래시) 부트 디스크와 procsrv 경로가 손상 없이
  그대로 유지됨.
- **확인함**: devmgr의 BAR 재사용 경로(이미 배정된 BAR를 그대로 반환)
  와 boot_bdf 배제 경로가 실제로 동작함 — virtio-blk 드라이버가 부트
  디바이스와 물리적으로 분리된 장치에만 쓰기 테스트를 수행함.
- **알려진 단순화(M15 범위 밖으로 명시적으로 남긴 것)**: 큐 협상
  (feature negotiation)은 여전히 최소(feature 0개/큐 1개/순수 폴링,
  ADR-150), 멀티큐·인터럽트 기반 완료 통지는 다루지 않는다.

## 다음 단계

M15는 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M16
(FAT32 + ext4 FS 서버) — 착수 전에 ADR-155 §2(IPC `pages[]`의 유저
프로세스 대상 공유 매핑 경로, OPEN-61)를 먼저 구현해야 한다.
