# 완료 보고: system-servers-bringup M12 전체 완료 — virtio-blk 클라이언트 + 실제 부트 디스크 마운트 + procsrv 골격

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M12
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md) ADR-150,
[kernel-memory.md](../design/kernel-memory.md) ADR-148/149
**선행 문서**: [system-servers-bringup-m12-kernel-cow.md](system-servers-bringup-m12-kernel-cow.md)
(커널 측 fork/exec/spawn/COW 프리미티브 검증 — 이 문서가 그 이후
남았던 procsrv/virtio-blk/cpio 부분을 마무리해 **M12 전체를 완료
처리한다**)
**실행일**: 2026-09-09

## 완료한 것

### B1. `sys_alloc_dma_buffer` syscall (ADR-148)

trusted 프로세스가 물리적으로 연속인 버퍼를 확보해 그 물리주소를
직접 받아가는 새 syscall. `process_ops.cpp::alloc_dma_buffer()` +
`syscall.cpp` case 5 + `uapi::dma_buffer_result`. initrun의
virtio-blk 클라이언트가 디바이스에 DMA로 넘길 vring/요청 버퍼를
만드는 데 쓴다.

### B2. `init/initrun/virtio_blk.{hpp,cpp}` — legacy virtio-blk 클라이언트

리셋→ACKNOWLEDGE|DRIVER→feature 협상(0)→큐 0 선택/크기 확인→vring을
DMA 버퍼 앞 16KiB에 구성→QueueAddress=PFN→DRIVER_OK로 초기화하고,
3-디스크립터(요청 헤더→데이터→상태 바이트) 블로킹 읽기 요청 1회를
순수 폴링(유한 반복 상한)으로 완료 확인한다. 디바이스 config space
(오프셋 0x14)의 용량과 호출자 상한 중 작은 쪽까지만 읽는다.

### B3. `uapi.hpp::m12_self_info` 브릿지를 `build_process()`로 일반화 (ADR-149)

`kernel_main.cpp::setup_initrun_process`가 initrun 전용으로 하던
"원본 ELF를 새 주소공간에 복사해 self_info로 알려주기"를
`process_ops.cpp::build_process()`로 옮겨, `sys_process_spawn`으로
만들어지는 모든 프로세스(procsrv 포함)가 자기 자신을 fork/exec할 수
있게 했다. 같은 작업 중 `virtio_blk.cpp` 추가로 initrun.elf가
0xcc50→0xf270바이트로 커지면서 `k_m12_self_elf_user_vaddr`/
`k_m12_self_info_user_vaddr` 사이 고정 간격(0xD000)을 실제로 넘어
매핑이 깨지는 버그(`setup_initrun_process ok=0`)를 QEMU에서 발견해
간격을 약 1MiB로 넓혀 수정했다.

### B4. `init/initrun/main.cpp` 재작성 — 실제 부트 디바이스 마운트 + 서비스 스폰

RDI로 받는 실제 `boot::boot_info`를 이제 읽는다. `boot_device.valid
&& io_port_ok`면 DMA 버퍼를 받아 virtio-blk를 초기화하고 부트 디스크
전체(≤3MiB)를 읽어, 그 cpio 아카이브를 순회해 `lib/*.ini`(기록
순서=파일명순=실행순)가 가리키는 `bin/*` ELF를 `sys_process_spawn`
한다. M8~M11이 검증에 쓰던 "initrun 자신을 fork/exec/spawn하는
self-test 데모"는 이 자리에서 완전히 제거했다(procsrv가 그 자리를
실제 디스크 I/O로 대체).

### B5. `servers/procsrv/main.cpp` — M12 골격

`sys_fork`+`sys_exec`로 자기 자신을 다시 실행하는 것이 유일한
책무다. initrun의 boot_info-vs-null 구분과 같은 비대칭을 얻기 위해
"initrun이 처음 스폰할 때만 비어있지 않은 1바이트 argv, procsrv
자신의 self-exec 호출은 항상 argv 없음"이라는 규약으로 "원본이냐
사본이냐"를 구분한다. procsrv.md §5/7/8(계정/로그인/su-sudo)은
핸들러 자리조차 만들지 않았다(OPEN-56 해소, ADR-150).

### B6. `tools/mkbootdisk.py` — 부트 디스크 cpio(newc) 빌더

`bin/<이름>` + `lib/NNN-<이름>.ini`를 cpio(newc)로 담는 최소 자체
구현(서드파티 cpio/tar 없음). `servers/procsrv/CMakeLists.txt`가
`minicore_bootdisk_image` 타겟으로 호출해 `bootdisk.img`를 만든다.

### B7. 스모크 테스트 기본 경로를 실제 부트 디스크로 전환

`tools/smoke-test-x86_64.sh`가 `MINICORE_QEMU_BOOTDISK`를 자동으로
`<빌드 디렉토리>/servers/procsrv/bootdisk.img`로 채워, **기본 스모크
테스트가 이제 항상 실제 부트 디스크 경로를 검증**한다. 새 어써션
`"[pci] assign_virtio_blk_bar ok=1 vendor=0x1af4 device=0x1001"` 추가
— 총 51개.

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh   # 51개 전부 PASS (부트 디스크 자동 첨부)
```

실측 로그(발췌, 순서 그대로):

```
[pci] assign_virtio_blk_bar ok=1 vendor=0x1af4 device=0x1001 io_base=0xc000
[initrun] setup_initrun_process ok=1
...
[initrun] kernel received boot call ok=1 label=0xb007 ... - 부팅 성공
[initrun] cpio/ini self-test ok=1
[process] spawn ok entry=0x10000000 trusted=0      ← procsrv, 실제 디스크에서 읽은 ELF
[process] fork ok child_pml4=0x6d000               ← procsrv가 자기 자신을 fork
[pf] cow copy virt=0x6ffffffff000 ...
[process] exec ok entry=0x10000000                 ← procsrv가 자기 자신을 exec
```

- **확인함**: initrun이 ACPI MCFG+PCI BAR 배정으로 찾은 실제
  virtio-blk 장치를 자신의 legacy 클라이언트로 마운트하고, 부트
  디스크의 cpio 아카이브에서 procsrv ELF를 실제로 읽어
  `sys_process_spawn`한다 — **실제 디스크 I/O를 거친 프로세스
  생성**(M12의 최종 QEMU 검증 목표) 확인.
- **확인함**: procsrv가 (초기화 로직 없이) 자기 자신을 `sys_fork`+
  `sys_exec`해 두 번째 완전한 유저 프로세스를 만든다.
- **확인함**: 부트 디스크를 붙이지 않은 경로(장치 없음)에서도
  `assign_virtio_blk_bar ok=0`으로 안전하게 실패하고 그 이상 진행하지
  않는다(회귀 없음 — ADR-131 §결정3의 PCIe 폴백 스캔은 여전히 미구현
  이므로 이 경우 그대로 부팅 실패 상태로 남는다, 의도된 동작).
- **확인하지 못함**: procsrv.md의 실제 프로세스 테이블·fd 진실
  공급원·계정 모델(§2~9) — 전부 M12 범위 밖(골격만).
- **확인하지 못함**: 여러 서비스 간 순서 의존성이 있는 경우의 동작
  (M12 부트 디스크는 procsrv 하나뿐 — OPEN-55 해소 근거 참고).

## 다음 단계

M12는 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M13
(VFS + memfs). 남은 미결정 항목은 [open-items.md](../design/open-items.md)
OPEN-51(systemd류 초기 프로세스), OPEN-52(서비스 준비완료 신호
정식 프로토콜), OPEN-54(procsrv IPC 와이어 프로토콜), OPEN-58(TSS
IOPB 격리) — 전부 M13 이후 실제로 필요해지는 시점에 다시 연다.
