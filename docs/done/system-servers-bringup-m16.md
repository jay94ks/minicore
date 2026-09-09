# 완료 보고: system-servers-bringup M16 — FAT32 + ext4 읽기전용 FS 서버

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M16
**관련 결정**: [kernel-ipc-objects.md](../design/kernel-ipc-objects.md)
ADR-159/161(IPC pages[]의 유저 프로세스 매핑, 구현 완료),
[kernel-memory.md](../design/kernel-memory.md) ADR-160(캐패빌리티
슬롯 경계 검증, 구현 완료), [filesystem.md](../design/filesystem.md)
ADR-162(FAT32/ext4/VFS 마운트 테이블/fs-protocol v2),
[boot-and-drivers.md](../design/boot-and-drivers.md) ADR-163
(devmgr I/O BAR 배정 버그 수정)
**실행일**: 2026-09-09

## 완료한 것

### D0. 선행 구현 — ADR-159/161(IPC pages[] 유저 매핑) + ADR-160(캐패빌리티 슬롯 경계 검증)

M16 착수 전 설계만 확정돼 있던 두 ADR을 실제로 구현했다.
`kernel/core/ipc/endpoint.cpp::deliver_message()`가 수신자가 실제
유저 프로세스면(`owner_space != nullptr`) 발신자 프레임의 참조
카운트를 올리고 고정 슬롯(`k_ipc_mapped_pages_user_vaddr`)에
읽기전용으로 매핑한다 — 수신자는 사전 준비 없이 `sys_call`/
`sys_recv`가 돌아온 시점에 `msg.pages[0].vaddr`을 그냥 읽으면 된다.
해제는 그 스레드가 다시 `deliver_message`의 목적지로 선택되는 시점
직전에 자동으로 일어난다(ADR-161이 ADR-159의 "다음 sys_recv 직전"을
이렇게 정정했다 — 구현 중 procsrv처럼 응답으로 매핑을 받는 순수
호출자가 다음 recv를 영원히 안 부를 수 있다는 걸 발견해서다).
`build_process()`의 self_elf 복사에 슬롯 예산 초과 검사도 추가했다
(`process_spawn_error::capability_slot_overflow`).

### D1. `docs/spec/fs-protocol.md` v2 — `OP_READ`를 `pages[]` 기반으로

응답이 `regs[2..3]`(16바이트 상한) 대신 위 D0의 매핑 경로로 온다 —
한 번에 최대 1페이지(4096바이트), 항상 오프셋 0부터. `servers/fs/memfs`
도 같은 라운드에서 갱신했다(페이지 정렬된 전용 읽기 스크래치 버퍼
신설 — 기존 `file_slot::data`를 그대로 노출하면 이웃 파일 내용까지
같이 매핑되므로).

### D2. `servers/vfs` — 정적 마운트 테이블

`/mnt/fat32/`, `/mnt/ext4/` 접두사를 각각 FAT32/ext4 서버에게, 그 외는
memfs에게 라우팅한다(M13 기본 경로 그대로 보존). `depends=`가 이제
콤마로 여러 이름(`memfs,fat32,ext4`)을 받아 여러 개의 `inherited_handles`
로 넘긴다(`init/initrun/main.cpp`, `tools/mkbootdisk.py` 갱신).

### D3. `servers/fs/fat32`(읽기전용) / `servers/fs/ext4`(읽기전용)

둘 다 devmgr에 vendor:device `0x1af4:0x1001`로 등록해 자신만의
virtio-blk-pci 장치를 마운트한다. FAT32는 BPB를 읽어 루트 디렉터리
클러스터 체인을 평평하게 스캔(LFN/서브디렉터리 건너뜀)한다. ext4는
매직+feature 화이트리스트+저널 유무를 검사하고(저널 있으면 항상
거부), 단일 블록 그룹의 루트 아이노드 extent(depth==0)를 순회해
디렉터리 블록을 선형 스캔한다(inode==0 항목 건너뜀). 둘 다 찾은
파일의 데이터를 최대 1페이지까지 읽어 응답한다.

### D4. 테스트 이미지 생성 — `tools/make-fs-test-images.sh` + `tools/mkfs-ext4-testfile.py`

FAT32는 `mkfs.vfat`+`mcopy`(dosfstools/mtools)로 충분했다. ext4는
`mke2fs -d <디렉터리>`가 이 환경에서 깨져 있어(직접 확인), 빈 이미지의
온디스크 구조를 직접 읽고 고쳐 파일 하나를 심는 전용 스크립트를 새로
만들었다.

## 실행 중 발견하고 고친 버그 (ADR-163 참고)

**devmgr I/O BAR 동시 배정 충돌**: `assign_or_get_bar0()`이 새로
배정할 때마다 같은 고정 포트(`0xC100`)를 반환하던 것이, M14/M15는
"새로 배정해야 하는 장치가 최대 하나"라 문제가 드러나지 않았지만
M16(fat32+ext4가 동시에 새 I/O BAR 요구)에서 실제로 두 장치가 같은
포트를 점유하는 충돌로 이어졌다(`[fat32] mount ok=0x0`으로 재현,
둘 다 `io_base=0xc100`으로 동일했음을 로그로 확인). 전진하는 전역
커서(`g_next_io_base`/`g_next_mmio_base`)로 고쳤다 — 이후
`io_base=0xc180`/`0xc200`으로 서로 다른 값이 나오고 둘 다 마운트
성공했다.

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/make-fs-test-images.sh build/x86_64-clang/fat32-test.img build/x86_64-clang/ext4-test.img
tools/smoke-test-x86_64.sh       # 63개 전부 PASS(M15의 59개 + 신규 4개) — 2회 연속 재확인, 이미지 해시 불변
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌):

```
[fat32] io_base=0xc180
[fat32] mount ok=0x1
[fat32] root_cluster=0x2
[ext4] io_base=0xc200
[ext4] mount ok=0x1
[ext4] block_size=0x400
[procsrv] vfs write/read roundtrip ok=1    ← M13 회귀 확인(memfs도 pages[] 경로로 갱신됐다)
[procsrv] fat32 read ok=1
[procsrv] ext4 read ok=1
```

- **확인함**: IPC `pages[]`의 유저 프로세스 매핑 경로(ADR-159/161)가
  memfs(M13 회귀)·fat32·ext4 세 조합 전부에서 정확한 내용을 전달함.
- **확인함**: 두 이미지 모두 읽기전용으로만 열려 2회 연속 부팅에도
  `sha256sum`이 그대로 유지됨(M15의 디스크 손상 버그와 달리 이번엔
  처음부터 검증했다).
- **확인함**: devmgr가 fat32/ext4에게 서로 다른 물리 virtio-blk
  장치(BDF)와 서로 다른 I/O 포트를 배정함(§실행 중 발견한 버그 수정
  후).
- **알려진 단순화(M16 범위 밖으로 명시적으로 남긴 것)**: FAT32/ext4
  쓰기, 하위 디렉터리, 4096바이트 초과 파일의 나머지(offset/seek),
  ext4 저널 재생·다단계 extent·다중 블록 그룹, FAT32 LFN, VFS 동적
  마운트.

## 다음 단계

M16은 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M17
(콘솔/TTY 드라이버 + 로그인 흐름).
