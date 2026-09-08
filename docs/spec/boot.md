# 부팅 스펙 (Boot Specification)

**관련 결정**: ADR-009, ADR-017, ADR-026, ADR-030, ADR-074
**관련 설계**: [repo-layout.md](../design/repo-layout.md), [security-model.md](../design/security-model.md) (§4의 trusted 부여)

이 문서는 커널이 어떻게 진입되고, 무엇을 초기화하며, initrun에게 무엇을
어떤 형태로 넘기는지를 구현 가능한 수준으로 정의한다.

## 1. 커널 진입점

### 1.1 x86_64 — Multiboot2 (1순위, ADR-009)

- `kernel/arch/x86_64/boot/`에 Multiboot2 헤더 배치
  (magic `0xE85250D6`, architecture=0, header_length, checksum).
- GRUB 등 Multiboot2 로더가 32비트 보호모드로 진입 → `_start32`가 최소
  GDT 설정, 페이지테이블(항등 매핑 + higher-half) 구성 후 long mode로
  전환 → `_start64`(멀티부트 정보 포인터를 EBX/RDI로 전달받음)로 점프.
  higher-half 페이지테이블(physmap/커널 스택/커널 이미지 영역)의
  정확한 주소·구성 순서는 [virtual-memory-layout.md](virtual-memory-layout.md)
  §2.1(ADR-078)에서 정의한다.
- `_start64`가 Multiboot2 태그(memory map, module)를 파싱해 공통
  `boot_info`(§3)로 변환한다.

### 1.2 x86_64 — UEFI (ADR-017)

- 커널을 PE32+ UEFI 애플리케이션으로도 빌드하며, 별도 진입점
  `efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE*)`을 둔다.
- 흐름: `GetMemoryMap` → `ExitBootServices` → `boot_info`(§3) 구성 →
  공통 커널 초기화(§4)로 점프. 커널 본체 코드는 Multiboot2 경로와
  100% 공유하며, **진입 스텁만 다르다**.

### 1.3 aarch64 — QEMU virt (2순위, ADR-009/ADR-026)

- QEMU virt 관례: EL1 진입, `x0` = FDT(Devicetree Blob) 물리주소,
  나머지 레지스터 0.
- `_start`(아키텍처 진입 스텁)이 스택 설정 등 최소 초기화 후, 자체
  구현한 FDT 파서(ADR-006 — 서드파티 FDT 라이브러리 금지)가
  `/memory`, `/chosen`(initrd, bootargs) 노드를 읽어 `boot_info`(§3)로
  변환한다. TTBR1 higher-half 페이지테이블(physmap/커널 스택/커널
  이미지 영역) 구성 순서는 [virtual-memory-layout.md](virtual-memory-layout.md)
  §4.1(ADR-078)에서 정의한다.

### 1.4 x86_64 — QEMU 개발용 PVH 직접 부팅 (ADR-114, 부트로더 아님)

- `kernel/arch/x86_64/boot/`에 Xen/PVH ELF Note
  (`XEN_ELFNOTE_PHYS32_ENTRY=18`, desc=`_start32`의 물리주소)를 함께
  둔다. QEMU의 내장 `-kernel` 로더는 Multiboot2도 64비트
  ELF(`EM_X86_64`)도 지원하지 않아(§1.1의 실제 부팅 이미지를 그
  경로로 못 띄운다) `tools/run-qemu.sh`가 QEMU의 `qboot.rom` 펌웨어로
  PVH 직접 부팅 경로를 대신 쓴다 — PVH 진입 상태(32비트 보호모드,
  페이징 꺼짐, 플랫 세그먼트, GDT/IDT/스택은 커널이 직접 구성)가
  Multiboot2 진입 상태와 사실상 같아 `_start32`를 그대로 재사용한다.
- **이것은 실제 부팅 경로가 아니다** — §1.1(Multiboot2/GRUB)과
  §1.2(UEFI)만이 ADR-017이 정한 배포·실기 부팅 프로토콜이다. 이 노트는
  개발 중 QEMU 반복 검증 속도를 위한 것으로, `boot_info` 파이프라인
  (§3~4)과 무관하며 PVH의 `hvm_start_info` 포인터(전달 시 EBX)를
  읽지 않는다.

## 2. 소스별 → boot_info 변환 규칙

| boot_info 필드 | Multiboot2 출처 | UEFI 출처 | FDT 출처 |
|---|---|---|---|
| `memory_map` | 태그 6 (memory map) | `GetMemoryMap` 결과 | `/memory` 노드 |
| `initrd` | 태그 3 (module) | 커맨드라인 인자로 전달된 경로 로드 | `/chosen/linux,initrd-{start,end}` |
| `cmdline` | 태그 1 (boot command line) | UEFI 로드 옵션 | `/chosen/bootargs` |
| `arch_data` | ACPI RSDP (태그 14 구버전/15 신버전) | ACPI RSDP 포인터(UEFI 설정 테이블) | 미사용(0) |
| `numa_topology` | `arch_data`의 ACPI SRAT/SLIT을 커널이 직접 파싱(ADR-036) | 좌동 | `/distance-map`, `/cpus`의 `numa-node-id`를 커널이 직접 파싱 |

토폴로지 정보가 아예 없는 환경(대부분의 QEMU 기본 구성)에서는
`numa_node_count = 1`, 모든 CPU가 노드 0에 매핑된 것으로 취급한다
(ADR-034/036의 "노드 1개 = 사실상 전역" 단순화와 일치).

## 3. boot_info 구조체

`kernel/include/boot_info.hpp`에 위치하며, 커널 코어와 initrun이
공유하는 유일한 부팅 관련 ABI다. **커널 코어는 이 구조체만 알고,
Multiboot2/UEFI/FDT의 존재 자체를 모른다** (ADR-002 HAL 경계).

```cpp
struct memory_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;      // 0=Usable 1=Reserved 2=AcpiReclaimable 3=KernelImage 4=InitrdImage
    uint32_t node_id;   // NUMA 노드 번호 (ADR-036). 토폴로지 정보 없으면 0.
};

// cpu_id(0..cpu_count-1) → NUMA 노드 번호. boot_info.cpu_node_map_addr가
// 가리키는 uint32_t 배열의 인덱스가 cpu_id다 (ADR-034/036).
struct boot_info {
    uint64_t magic;              // 0x4D434249 ("MCBI")
    uint32_t version;            // = 2 (NUMA 토폴로지 필드 추가, ADR-035/036)
    uint32_t cpu_count;          // 논리 CPU(코어) 총 개수. M1~M8 실행 환경에서는 1.

    uint64_t memory_map_addr;    // memory_region 배열의 물리주소
    uint32_t memory_map_count;
    uint32_t numa_node_count;    // NUMA 노드 개수. 토폴로지 정보 없으면 1.

    uint64_t cpu_node_map_addr;  // uint32_t[cpu_count] 배열의 물리주소.
                                  // 토폴로지 정보 없으면 0이며, 이 경우
                                  // 커널은 모든 cpu_id를 노드 0으로 취급한다.

    uint64_t initrd_addr;        // 물리주소, 없으면 0
    uint64_t initrd_size;

    uint64_t cmdline_addr;       // NUL 종료 문자열 물리주소, 없으면 0

    uint64_t arch_data_addr;     // arch별 부가정보(ACPI RSDP 등), 없으면 0
};
```

- `memory_region.node_id`와 `cpu_node_map`은 각각 ADR-036의 "메모리
  할당자 NUMA 인지"와 "스케줄러 NUMA 인지"가 참조하는 원천 데이터다.
- M1~M8(단일 코어·단일 노드 실행, ADR-035)에서는 `cpu_count=1`,
  `numa_node_count=1`, `cpu_node_map_addr=0`으로 관찰되며, 커널
  코드 경로는 실제 멀티 노드 환경과 동일한 파싱·조회 로직을 타되
  결과가 자명하게 "전부 노드 0"이 된다 — 별도의 단일코어 전용
  분기를 두지 않는다.

## 4. initrun 초기화 흐름

1. 커널 초기화 완료 — GDT/IDT/APIC(x86_64) 또는 예외벡터/GIC(aarch64),
   페이지테이블, 물리 메모리 할당자·힙(ADR-012), 스케줄러(ADR-014).
2. `boot_info.initrd_*`가 가리키는 영역을 initrd(§5)로 파싱.
3. initrd에서 `"initrun"` 항목을 찾아 ELF로 로드 — 새 주소공간과
   스레드를 생성한다(ADR-011 객체 모델). 이때 커널은 이 주소공간을
   **무조건 `trusted = true`**로 생성한다(ADR-074) — initrun은 시스템의
   유일한 최초 신뢰 루트이며, 다른 어떤 프로세스도 이렇게 자동으로
   `trusted`를 받지 않는다.
4. initrun 첫 스레드에게 `boot_info`를 전달(§6)하고, "trusted 부여
   권한" 캐패빌리티([security-model.md](../design/security-model.md)
   ADR-074)를 함께 쥐어준 뒤 유저모드로 진입시킨다 — initrun은 이후
   자신이 기동하는 서버 중 신뢰 보호가 필요한 것(예: cfgsrv)에 한해
   이 권한을 사용해 `trusted`를 재부여할 수 있다.

## 5. Initrd 포맷: MCPACK v1 (자체 구현, ADR-006)

서드파티 tar/cpio 파서를 쓰지 않고 최소한의 자체 포맷을 정의한다.

```cpp
struct mcpack_entry {
    char     name[60];    // NUL 종료. 예: "initrun", "procsrv"
    uint64_t offset;       // 헤더 시작 기준, 파일 데이터의 바이트 오프셋
    uint64_t size;
};

struct mcpack_header {
    uint32_t magic;         // 0x4D43504B ("MCPK")
    uint32_t version;       // = 1
    uint32_t entry_count;
    uint32_t _pad;
    // 이어서 mcpack_entry[entry_count], 그 뒤에 각 파일의 원본 바이트가 이어짐
};
```

`tools/mkinitrd.*`(현재 스텁 — [scaffold-repo-skeleton.md](../done/scaffold-repo-skeleton.md))가
이 포맷으로 패키징하는 역할을 맡는다.

## 6. boot_info 전달 메커니즘 (해결: OPEN-22 → ADR-030)

- 커널은 `boot_info` 전체가 담긴 페이지(들)를 initrun 주소공간에
  **읽기전용으로 매핑**하고, 그 가상주소를 initrun 최초 스레드
  진입 시 **아키텍처 관례상 "첫 인자" 레지스터**로 전달한다:
  x86_64는 `RDI`, aarch64는 `X0`.
- 근거: 각 아키텍처의 함수 호출 규약에서 이미 "첫 인자"로 쓰이는
  레지스터를 재사용하면, initrun 진입 스텁을 평범한 함수 호출처럼
  `_start(const boot_info* info)` 형태로 작성할 수 있어 가장 단순하다.
- initrun이 procsrv 등 다른 서버를 기동할 때는 이 관례를 그대로
  강제하지 않는다 — 각 서버에게 어떤 시작 인자를 줄지는 initrun
  자신의 설계(기동 매니페스트, 아직 미정)에 맡긴다.

## 아직 정하지 않은 것

- 코어 서버들의 기동 순서·의존성을 기술하는 매니페스트 형식은 이
  스펙의 범위 밖이다 — initrun 자체의 설계 문제로 넘긴다. 이
  스펙은 "커널이 initrun 하나를 유저모드로 띄운다"까지만 다룬다.
- ACPI(x86_64) 파싱은 SMP·전원관리가 필요해지는 시점에 `arch_data_addr`을
  통해 확장한다.
