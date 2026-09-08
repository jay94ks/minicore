# 완료 보고: kernel-bootstrap M2 — boot_info 파이프라인

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M2
**관련 스펙**: [boot.md](../spec/boot.md), [debug-console.md](../spec/debug-console.md)
**관련 결정**: ADR-002, 009, 017, 035, 036, 042, 114, 116, 117
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M2의 목표("메모리맵 항목 수·initrd 위치를 시리얼 콘솔에 덤프")를
달성했다 — 클린 빌드 후 `tools/smoke-test-x86_64.sh`가 `PASS`를
반환한다(M1의 검사에 M2 검사 2개를 추가):

```
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => PASS: "hello from kernel" 확인
# => PASS: "[boot_info:selftest] memory_map_count=4" 확인
# => PASS: "[boot_info:selftest] initrd_addr=0x1000000 initrd_size=0x100000" 확인
```

## 수행한 작업

### 1. boot_info ABI (boot.md §3)

- [kernel/include/boot_info.hpp](../../kernel/include/boot_info.hpp) —
  `boot::boot_info`/`boot::memory_region`을 spec 그대로 옮겼다. 커널
  코어와 initrun이 공유하는 유일한 부팅 ABI(ADR-002 HAL 경계) — 이
  헤더는 Multiboot2/UEFI/FDT의 존재를 전혀 모른다.
- `boot::dump(tag, info, regions)` — klog으로 `boot_info` 내용을
  출력하는 arch 독립 함수. `regions`는 이미 역참조 가능한 포인터를
  요구한다(물리주소인 `info.memory_map_addr` 자체를 이 함수가 풀지
  않음 — phys_to_virt는 arch 몫이라 HAL 경계를 지킨다).
  [kernel/core/boot_info_dump.cpp](../../kernel/core/boot_info_dump.cpp)에서 구현.

### 2. x86_64 Multiboot2 태그 파서 (boot.md §1.1/§2)

- [kernel/arch/x86_64/boot_info_x86_64.hpp](../../kernel/arch/x86_64/boot_info_x86_64.hpp)/
  [.cpp](../../kernel/arch/x86_64/boot_info_x86_64.cpp) — `build_boot_info()`가
  Multiboot2 태그 스트림(메모리맵/모듈/커맨드라인/ACPI RSDP)을
  physmap(`phys_to_virt`, M1이 이미 구성해 둔 512GiB 항등 매핑) 경유로
  읽어 `boot::boot_info`로 변환한다. 커널 자신과 initrd가 차지한
  물리 범위를 `k_region_kernel_image`/`k_region_initrd_image`로 명시적
  추가한다(안 하면 M3의 물리 할당자가 실행 중인 커널 위에 할당할
  위험이 있다 — boot.md §3이 이 두 타입 값을 이미 예정해 둔 이유).
- [kernel/arch/x86_64/memory_layout.hpp](../../kernel/arch/x86_64/memory_layout.hpp) —
  `phys_to_virt`/`virt_to_phys_physmap`(physmap 경유)와
  `image_virt_to_phys`(커널 이미지 자신의 가상주소 변환, physmap과는
  다른 주소 공간이므로 절대 혼용하지 않는다)를 M1의 `paging_setup.cpp`와
  공유하는 단일 소스로 뽑아냈다. **정식 소유자는 M3의
  `kernel/core/mm`**(virtual-memory-layout.md §5)이며, 이 파일은 그
  전까지 쓰는 임시 위치임을 주석으로 명시했다.
- `boot.S`(`_start32`) — 부트로더가 EAX/EBX로 넘기는 Multiboot2
  매직/info 물리주소를 다른 코드가 건드리기 전에 `.boot.bss`의
  `mb2_magic`/`mb2_info_addr`에 저장하도록 수정(가장 먼저 실행되는
  두 줄로 추가).
- [kernel/arch/x86_64/kernel_main.cpp](../../kernel/arch/x86_64/kernel_main.cpp) —
  `mb2_magic`/`mb2_info_addr`로 `build_boot_info()`를 호출해
  `"real"` 태그로 덤프.

### 3. klog `%l` 길이 수식어

- [kernel/core/klog.cpp](../../kernel/core/klog.cpp) — `%ld`/`%lu`/`%lx`
  추가(SysV x86-64의 `long`은 64비트). 물리주소·페이지테이블 엔트리처럼
  64비트 값을 다루는 M2부터 필요해졌다 — `%u`/`%x`에 `uint64_t`를 넘기면
  가변인자 프로모션 규칙상 잘못된 값을 읽는다. [debug-console.md](../spec/debug-console.md) §3
  갱신.

### 4. freestanding memset/memcpy/memmove/memcmp (ADR-116)

- [kernel/core/freestanding_mem.cpp](../../kernel/core/freestanding_mem.cpp) —
  `boot::boot_info info{};` 같은 구조체 zero-init에서 Clang이
  `-ffreestanding`과 무관하게 `memset` 호출을 낼 수 있어 링크 실패가
  났다(`undefined symbol: memset`). 최소 루프 구현을 추가하고,
  자기 자신을 다시 memcpy/memset 호출로 "최적화"하는 함정을 피하려
  이 파일만 `-fno-builtin`으로 컴파일한다. M3(libk) 착수 시 정식
  위치로 옮기는 것을 검토한다.

## QEMU 검증 방법 (ADR-117 — GRUB 부재로 인한 self-test)

이 개발 머신에는 여전히 GRUB가 없어(ADR-114) 실제 Multiboot2 데이터로
파서를 검증할 수 없다. QEMU PVH 개발 경로는 애초에 Multiboot2 정보
자체를 주지 않으므로, `kernel_main`이 찍는 `"real"` 태그 결과는 항상
비어 있다(정상 — 아래 로그의 `[boot_info:real]` 참고).

대신 `boot_info_x86_64.cpp`가 손으로 만든 최소 Multiboot2 info 블록
(메모리맵 항목 2개 + 모듈 1개 + 커맨드라인 1개)을 커널 이미지 안에
정적으로 두고, **파서 전용 별도 코드 경로가 아니라 실제
`build_boot_info()`를 그대로** 그 블록에 호출해 `"selftest"` 태그로
덤프한다. 아래는 실제 QEMU 실행 결과다:

```
hello from kernel
[boot_info:real] magic=0x4d434249(ok) version=2 cpu_count=1
[boot_info:real] memory_map_count=0 numa_node_count=1
[boot_info:real] initrd_addr=0x0 initrd_size=0x0
[boot_info:real] cmdline_addr=0x0 arch_data_addr=0x0
[boot_info:selftest] magic=0x4d434249(ok) version=2 cpu_count=1
[boot_info:selftest] memory_map_count=4 numa_node_count=1
[boot_info:selftest]   region[0] base=0x0 length=0x9fc00 type=0 node=0
[boot_info:selftest]   region[1] base=0x100000 length=0x7f00000 type=0 node=0
[boot_info:selftest]   region[2] base=0x100000 length=0x11800 type=3 node=0
[boot_info:selftest]   region[3] base=0x1000000 length=0x100000 type=4 node=0
[boot_info:selftest] initrd_addr=0x1000000 initrd_size=0x100000
[boot_info:selftest] cmdline_addr=0x111668 arch_data_addr=0x0
```

- `region[0]`/`region[1]`은 self-test가 심어둔 가짜 usable 영역
  (자체 테스트 fixture의 값과 정확히 일치 — 파싱 정확성 확인).
- `region[2]`(type=3, kernel_image)의 `base=0x100000`은
  `link.ld`의 `KERNEL_PHYS_BASE`와, `length=0x11800`은
  `image_virt_to_phys(_image_end) - 0x100000`(실제 빌드된 이미지
  크기)과 정확히 일치 — 커널 자신을 마킹하는 로직도 검증됐다.
- `region[3]`(type=4, initrd_image)은 self-test 모듈 태그의
  `mod_start`/`mod_end`(0x1000000~0x1100000)와 일치.
- `cmdline_addr=0x111668`은 `region[2]`의 범위(0x100000~0x111800)
  안에 정확히 들어온다 — physmap 경유로 읽은 태그 데이터를
  `virt_to_phys_physmap`으로 되돌린 값이 커널 이미지 자신의
  물리 범위와 일관됨을 (다른 두 변환 경로를 교차시켜) 확인한 것이다.

## 검증 결과 (정직하게 보고)

- **확인함**: self-test 경로로 메모리맵 4개 항목(fixture 2개 + 커널
  이미지 + initrd)과 initrd 위치가 정확히 기대값과 일치함을 QEMU
  실행으로 반복 확인(스모크 테스트 3회 이상 재실행, 매번 동일).
- **확인하지 못함**: 실제 GRUB Multiboot2 ISO가 준 데이터로의 검증 —
  ADR-114/ADR-117에 이미 기록된 한계가 그대로 이어진다. 이 self-test는
  "파서 로직이 스펙대로 태그를 해석하는가"만 확인하며, "실제 GRUB
  구현이 우리 가정과 다른 바이트를 줄 가능성"까지는 배제하지 못한다.
- ACPI RSDP 태그(type 14/15) 파싱 코드는 작성했지만 self-test fixture에
  포함하지 않아 이번에 실행 경로를 타지 않았다 — 코드 리뷰 수준으로만
  확인했다(단순한 오프셋 계산이라 위험은 낮다고 판단했지만, 실제
  실행 검증은 아니다).

## 다음 마일스톤과의 접점

- M3(코어 메모리 관리)이 `kernel/arch/x86_64/memory_layout.hpp`의
  `phys_to_virt`/`virt_to_phys_physmap`을 `kernel/core/mm`의 정식
  `phys_to_virt`/`virt_to_phys`로 승격해야 한다
  (virtual-memory-layout.md §5) — `boot_info_x86_64.cpp`는 그때
  `#include`만 바꾸면 되도록 이미 같은 함수 이름/의미로 맞춰뒀다.
  `boot_info.memory_map_addr`(물리주소, `k_region_kernel_image`/
  `k_region_initrd_image` 포함)가 M3의 물리 페이지 할당자가 "이
  범위는 이미 쓰고 있다"고 판단할 첫 입력이 된다.
  `kernel/core/freestanding_mem.cpp`도 이 시점에 정식 위치로 옮기는
  것을 검토한다(ADR-116).
- M8(initrun 로딩)이 `boot_info.initrd_addr`/`initrd_size`를 그대로
  써서 MCPACK을 파싱한다 — 이번에 만든 모듈 태그 파싱이 그 입력이다.
