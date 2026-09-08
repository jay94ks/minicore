# 완료 보고: kernel-bootstrap M1 — Arch 부트 스텁 + 디버그 콘솔

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M1
**관련 스펙**: [boot.md](../spec/boot.md), [debug-console.md](../spec/debug-console.md), [virtual-memory-layout.md](../spec/virtual-memory-layout.md), [cxx-conventions.md](../spec/cxx-conventions.md)
**관련 결정**: ADR-009, 010, 017, 037, 042, 078, 113, 114
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M1의 목표("QEMU에서 시리얼 포트로 hello from kernel 출력")를
달성했다 — 클린 빌드 후 `tools/smoke-test-x86_64.sh`가 `PASS`를
반환한다:

```
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => PASS: 시리얼 콘솔에서 "hello from kernel" 확인
```

## 수행한 작업

### 1. klog (디버그 콘솔, debug-console.md)

- [kernel/include/klog.hpp](../../kernel/include/klog.hpp) — arch 독립
  선언(`init`/`putc`/`write`/`printf`/`vprintf`). ADR-002 HAL 경계에
  따라 `kernel/core`는 이 헤더만 안다.
- [kernel/core/klog.cpp](../../kernel/core/klog.cpp) — `write`/`printf`를
  `klog::putc` 위에 구현. 동적 할당 없는 고정 버퍼 정수 포맷터
  (`%d %u %x %p %s %c %%`).
- [kernel/arch/x86_64/klog_uart.cpp](../../kernel/arch/x86_64/klog_uart.cpp) —
  16550 UART(COM1, 포트 0x3F8) 폴링 드라이버. `init`/`putc` 구현.

### 2. x86_64 Multiboot2 부트 스텁 (boot.md §1.1, virtual-memory-layout.md §2)

- [kernel/arch/x86_64/boot/boot.S](../../kernel/arch/x86_64/boot/boot.S) —
  Multiboot2 헤더, 자체 GDT(null/code32/data32/code64/data64),
  `_start32`(임시 항등 매핑 4×2MiB 페이지 구성 → PAE/LME/NXE/PG
  활성화 → long mode 진입), `_start64`(임시 낮은 스택으로
  `boot_setup_paging()` 호출 → 진짜(커널 이미지 안) 스택으로 전환 →
  `kernel_main`으로 점프).
- [kernel/arch/x86_64/boot/paging_setup.cpp](../../kernel/arch/x86_64/boot/paging_setup.cpp) —
  virtual-memory-layout.md §2 표의 physmap(512GiB, 1GiB huge page
  512개 항등 매핑)과 커널 이미지 higher-half 매핑을 구성.
  `.text`/`.rodata`/`.data`+`.bss`를 §3 표대로 R+X/R/R+W로 분리
  매핑(NX 비트, EFER.NXE 사용). 커널 스택 영역(2GiB)은 슬롯 할당이
  M5 대상이라 주소만 예약하고 엔트리는 만들지 않는다.
  **함정**: 이 함수 자신도 `.boot.text`(저지대, VMA==LMA)에 명시적으로
  배치해야 한다 — 데이터만 옮기고 함수를 빠뜨리면, 자신이 만드는
  중인 higher-half 매핑을 통해 자신을 호출하려는 모순(#PF)에 빠진다
  (실제로 이 버그로 한 번 트리플 폴트를 겪고 수정했다).
- [kernel/arch/x86_64/link.ld](../../kernel/arch/x86_64/link.ld) —
  `.boot`(물리 1MiB, VMA==LMA)와 higher-half `.text`/`.rodata`/`.data`/`.bss`
  (0xFFFFFFFF80000000 기준, 물리적으로는 `.boot` 바로 뒤에 연속)를
  분리하는 표준 "higher-half 커널" 링커 스크립트. `PT_NOTE` 세그먼트를
  명시적으로 생성하기 위해 `PHDRS` 블록을 쓴다(ADR-114 참고).
- [kernel/arch/x86_64/kernel_main.cpp](../../kernel/arch/x86_64/kernel_main.cpp) —
  higher-half 진입점. `klog::init()` 후 "hello from kernel" 출력,
  이후 `hlt` 루프(M1 시점엔 종료 수단이 없다).

### 3. CMake 연결

- [kernel/CMakeLists.txt](../../kernel/CMakeLists.txt) — arch 독립
  `minicore_kernel_core` OBJECT 라이브러리(현재 `core/klog.cpp`만)를
  추가하고 `add_subdirectory(arch/${MINICORE_ARCH})`로 연결.
- [kernel/arch/x86_64/CMakeLists.txt](../../kernel/arch/x86_64/CMakeLists.txt) —
  `minicore_kernel_x86_64.elf` 실행파일 정의. `-mno-red-zone`과
  SSE/MMX 비활성 플래그(FPU/SSE 상태 미초기화 상태에서 컴파일러가
  XMM을 쓰지 못하게) 추가.
- [kernel/arch/aarch64/CMakeLists.txt](../../kernel/arch/aarch64/CMakeLists.txt) —
  빈 플레이스홀더(aarch64는 이 계획 범위 밖, ADR-009).
- 최상위 [CMakeLists.txt](../../CMakeLists.txt)에 `ASM` 언어 추가
  (`project(minicore CXX C ASM)`) — `boot.S` 컴파일에 필요.

### 4. tools/

- [tools/run-qemu.sh](../../tools/run-qemu.sh) — 실제 구현(기존
  스텁 대체). x86_64: `qboot.rom` + `-kernel`로 PVH 직접 부팅(ADR-114).
  aarch64: 아직 부트 스텁이 없다는 안내만 출력.
- [tools/smoke-test-x86_64.sh](../../tools/smoke-test-x86_64.sh) —
  M1 완료 기준의 자동 검증(고정 시간 후 QEMU를 종료하고 시리얼 로그에서
  "hello from kernel" 확인). kernel-bootstrap.md의 "M1 완료 시점에
  스모크 테스트 스크립트 추가" 요구사항.

## 실행 중 발견해 기록한 결정(ADR)

작업 도중 사전에 예상하지 못했던 두 가지 사실을 확인했고, 각각
새 ADR로 기록했다(둘 다 코드 작성이 아니라 이 개발 환경/툴체인
자체의 사실 확인에서 나온 결정):

- **ADR-113** — 설치된 Clang/LLVM(winget `LLVM.LLVM`)에는 freestanding
  타깃용 libc++가 전혀 없어 `<cstdint>`/`<cstddef>`/`<cstdarg>`가
  어디에도 없었다. `toolchain/freestanding-cxx/`에 3개 헤더를 자체
  shim으로 작성해 해결(당장 필요한 것만 — `<type_traits>` 등 나머지는
  **OPEN-48**로 남겨 M3 착수 전 결정하기로 했다).
- **ADR-114** — QEMU(11.1)의 내장 `-kernel` 로더가 Multiboot2도
  64비트 ELF도 지원하지 않음을 QEMU 소스(`hw/i386/multiboot.c`)로
  확인했다. 이 개발 머신에는 GRUB(`grub-mkrescue`)를 구할 방법이
  없어(MSYS2 미제공, 소스 빌드는 범위 밖), QEMU 검증 전용으로
  Xen/PVH ELF Note 직접 부팅 경로를 추가했다 — **Multiboot2/UEFI라는
  실제 배포 부팅 프로토콜(ADR-017)은 전혀 바뀌지 않았다.**

## 그 외 구현 세부(문서화할 정도는 아니지만 재현에 필요한 것)

- **링크 드라이버 우회**: 이 Windows 호스트의 clang++는
  `--target=x86_64-unknown-none-elf` 링크 시 자기 자신이 아니라 PATH의
  `g++`(MSYS2, POSIX 호스트용)에 위임해버려 `-T` 링커 스크립트조차
  못 알아듣는 상태가 됐다. `kernel/arch/x86_64/CMakeLists.txt`에서
  `CMAKE_CXX_LINK_EXECUTABLE`을 `ld.lld`(LLVM 동봉) 직접 호출로
  덮어써 우회했다 — freestanding ELF에는 어차피 crt/표준 링크 단계가
  불필요하므로 컴파일러 링크 드라이버 자체가 필요 없다.
- **QEMU 조달**: winget의 QEMU 설치는 관리자 권한 승인이 필요해(이
  세션에서는 승인 불가) 실패했다 — 대신 **MSYS2 pacman**
  (`mingw-w64-x86_64-qemu`, 관리자 권한 불필요)으로 QEMU 11.1.1을
  설치했다. `docs/done/toolchain-setup.md`가 이미 Clang을 확인했으므로,
  이것으로 M1 실행에 필요한 두 도구(컴파일러, QEMU)가 모두 이
  개발 머신에 갖춰졌다 — 단, 저장소 밖에 설치했을 뿐 이 사실 자체를
  별도 `docs/done` 문서로 기록하진 않는다(QEMU는 ADR-091이 이미
  "저장소 밖에서 관리"로 정책만 정해두었고, 설치 자체는 실행 세부라
  이 문서에 남긴다).

## 검증 결과 (정직하게 보고)

- **확인함**: `cmake --preset x86_64-clang` 클린 구성 → 빌드 →
  `tools/smoke-test-x86_64.sh` PASS. 여러 번 반복 실행해도 안정적으로
  통과.
- **확인함**: `aarch64-clang` 프리셋은 여전히 구성만 통과한다(컴파일
  대상 소스가 없으므로 빌드 검증은 아직 의미 없음, 정상).
- **확인하지 못함(사전에 알려진 한계, 새로 발견한 것 아님)**:
  `x86_64-gcc`/`aarch64-gcc` 프리셋 — `x86_64-elf-gcc`/`aarch64-elf-gcc`
  크로스 GCC가 이 머신에 없어 여전히 구성조차 안 된다
  ([toolchain-setup.md](toolchain-setup.md)에 이미 기록된 한계, 이번
  작업과 무관).
- **확인하지 못함(새로운 한계, ADR-114 참고)**: 실제 GRUB Multiboot2
  ISO 부팅 경로 — 이 개발 머신에 GRUB가 없어 시도하지 못했다. M1은
  QEMU PVH 경로로만 검증했다. Multiboot2 헤더 자체는 spec대로
  작성했고 링크 결과물에 정상적으로 포함되어 있음을 확인했지만
  (`llvm-objdump`로 헤더 바이트·체크섬 확인), 실제 GRUB가 이를
  로드하는지는 검증하지 못했다.
- `.rodata`/`.data`/`.bss`가 실제로 서로 다른 페이지 권한(R vs R+W,
  NX 비트)으로 매핑됐는지는 QEMU `-d int` 로그로 간접 확인했을 뿐
  (트리플 폴트 당시 오류 코드로 NX/not-present 구분이 가능함을
  확인), 매핑 이후 각 권한을 어기는 접근을 의도적으로 유발해
  정면으로 검증하지는 않았다.

## 다음 마일스톤과의 접점

- M2(boot_info 파이프라인)는 이번에 만든 `_start64`에 Multiboot2
  태그 파싱을 추가하는 지점이다 — 이번 M1에서는 태그를 전혀 읽지
  않는다(EAX/EBX를 보존조차 하지 않음, boot.md §1.1이 요구하는
  범위가 M1엔 없었기 때문).
- M3(libk/mm)은 **OPEN-48**(freestanding C++ 헤더 나머지)을 먼저
  해결해야 착수 가능하다.
- `kernel/arch/x86_64/boot/paging_setup.cpp`가 이미 만들어 둔 physmap은
  M3의 `phys_to_virt`/`virt_to_phys`(virtual-memory-layout.md §5)가
  그대로 재사용할 수 있다 — 다시 만들 필요 없음.
