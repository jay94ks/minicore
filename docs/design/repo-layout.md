# 저장소 레이아웃 및 빌드 구조

이전 결정([build-system.md](build-system.md)의 ADR-019~022)을 구체적인 디렉토리
트리와 CMake 서브프로젝트 구성으로 옮긴 문서. `docs/plan/scaffold-repo-skeleton.md`
계획의 실행 대상이 되는 상세 설계다.

## 디렉토리 트리

**주의(2026-09-10)**: 아래 트리는 [build-system.md](build-system.md)
ADR-199/[foundations.md](foundations.md) ADR-200이 정한 **목표
상태**를 반영한다(`libs/k/`, `libs/mc/`, `uapi.hpp` 없음) — 실제
저장소는 아직 `libk/`, `libmc/`(최상위), `kernel/include/uapi.hpp`
그대로다. 실행은 [libs-restructure.md](../plan/libs-restructure.md)
M49~M50이 다룬다(아직 착수 전).

```
minicore/
├── CMakeLists.txt              # 최상위: arch/toolchain 조합 오케스트레이션
├── CMakePresets.json           # x86_64-clang / x86_64-gcc / aarch64-clang / aarch64-gcc
├── .gitmodules                 # third_party/ 하위 서드파티 소스 등록
│
├── toolchain/                  # CMake 툴체인 파일 (ADR-019, ADR-020)
│   ├── common.cmake            # 공통 플래그: -fno-exceptions -fno-rtti 등 (ADR-010)
│   ├── x86_64-clang.cmake
│   ├── x86_64-gcc.cmake
│   ├── aarch64-clang.cmake
│   └── aarch64-gcc.cmake
│
├── kernel/                     # 커널 (직접 구현, ADR-006)
│   ├── CMakeLists.txt
│   ├── include/                # (ADR-200으로 폐지 예정) 유저랜드와 손으로 맞춰 공유하던
│   │                             # 커널 ABI 헤더(uapi.hpp) — libs/mc/로 흡수되고 이 자리는
│   │                             # 없어진다
│   ├── core/                   # arch-독립 코어: IPC, 스케줄러, 객체, 핸들 테이블, 메모리 할당자
│   │   ├── ipc/                # ADR-004, ADR-013, ADR-015
│   │   ├── sched/               # ADR-014
│   │   ├── mm/                  # ADR-012 (슬랩/페이지 할당자, 쿼터), COW (ADR-016)
│   │   └── object/              # 핸들 테이블 (ADR-011), 스레드/주소공간/엔드포인트 객체
│   └── arch/                   # arch 의존 코드는 반드시 이 아래에만 존재 (ADR-002)
│       ├── x86_64/              # GDT/IDT/APIC/페이징/Multiboot2+UEFI 엔트리 (ADR-009, ADR-017)
│       └── aarch64/              # 예외벡터/GIC/MMU/디바이스트리 엔트리 (ADR-009, ADR-017, OPEN-29)
│
├── libs/                        # 이 저장소가 직접 만들고 유지·관리하는 라이브러리
│   │                             # (ADR-199 — lib 접두사 없는 이름)
│   ├── k/                        # 커널·시스템 서버 공용 프리스탠딩 코어 라이브러리 (ADR-010,
│   │   │                           # 구 libk) — ADR-198 범위 밖(kern::/kernsrv:: 계층 대상 아님)
│   │   └── include/k/             # result, optional, span, intrusive_list, atomic 래퍼 등
│   └── mc/                       # minicore 네이티브 API, 순수 C+어셈블러 (ADR-132, 구 libmc) —
│       │                           # 이제 커널·유저 공용(ADR-200, MC_LAND_KERNEL 매크로로
│       │                           # 구분) — syscall 1:1 래퍼+서버별 프로토콜 클라이언트+
│       │                           # 커널 ABI(uapi.hpp 흡수) 전부의 정본
│       ├── include/mc/            # 정본 헤더 — 커널-랜드는 구조체/enum/상수만, 유저-랜드는
│       │   │                       # 그 위에 syscall 트램폴린+프로토콜 클라이언트 함수까지
│       └── src/
│           ├── <arch>/             # syscall 트램폴린 asm + _start(진입점, ADR-131 인자 관례 파싱)
│           └── ipc/                # 서버별 프로토콜 클라이언트 구현 (servers/ 구조·이름과 대응)
│
├── init/
│   └── initrun/                # 커널이 직접 기동하는 최초 유저 프로세스 (ADR-017)
│                                 # boot_info.boot_device_descriptor(initrd의 disk.cfg에서
│                                 # 옴, 없으면 임베디드 PCIe 폴백 스캔)로 부트 디바이스를
│                                 # 직접 마운트하는 임베디드 최소 virtio-blk 클라이언트+
│                                 # cpio(newc) 리더를 갖고, 그 위에서 procsrv 등 서비스
│                                 # 바이너리(procsrv/vfs/devmgr 등 "커널 서버", 하드코딩된
│                                 # 이름·순서)를 찾아 초기화 완료를 기다리며 순서대로
│                                 # 기동한 뒤, 마지막으로 유저 서비스 관리자 데몬
│                                 # (systemd류, servers/svcmgr 가칭 — "커널 서버"가 아닌
│                                 # 더 동적인 유저 서비스만 관리)을 하나 더 실행시키고
│                                 # 스스로 사라진다 — 그 데몬이 프로세스 트리의 새 루트가
│                                 # 되고, initrun의 남은 자식(커널 서버들)은 그 데몬으로
│                                 # 재부모화된다(ADR-131 §결정7 원안+ADR-192, OPEN-51
│                                 # 해소). devmgr/M15의 "진짜" virtio-blk 드라이버와는
│                                 # 별개 코드, FAT32 서버(M16)와도 무관
│
├── servers/                     # 시스템 서비스 (직접 구현, ADR-006/007/008)
│   │                             # 전부 k(C++ 유틸)+mc(C API 바인딩)만 링크한다 —
│   │                             # libc가 필요 없다 (ADR-132, 구 libk/libmc)
│   ├── procsrv/                  # 프로세스 서버: fork/exec/signal/wait/pid, fd 진실 공급원 (ADR-016)
│   ├── vfs/                       # 경로 탐색·마운트·핸들 위임 (ADR-018)
│   ├── fs/
│   │   └── memfs/                 # 최초 구현 대상: 메모리 파일시스템 (ADR-008 수직 슬라이스)
│   ├── netsrv/                    # 소켓·프로토콜 스택
│   ├── devmgr/                    # 디바이스 매니저: 버스 열거, 드라이버 감시·재시작 (ADR-007)
│   ├── cfgsrv/                    # 설정 리포지터리(레지스트리) 서버 (ADR-060, VFS와 완전 분리)
│   ├── svcmgr/                    # 유저 서비스 관리자(systemd류) — initrun이 마지막으로
│   │                                # spawn, 프로세스 트리 영구 루트. "커널 서버"가 아닌
│   │                                # 유저 서비스만 관리(ADR-192/196, @global/system/services
│   │                                # 레지스트리 소비, ADR-197)
│   └── drivers/                   # 유저 드라이버 (블록·네트워크·USB·GPU 등, ADR-007)
│
├── libc/                        # POSIX 계층 (ADR-005/008)
│   ├── CMakeLists.txt            # third_party/<libc>에 대한 빌드 글루
│   └── sysdeps/minicore/          # libc 내부 훅을 mc의 mc_* 호출로 연결하는 얇은 어댑터
│                                   # (ADR-132 — 프로토콜 자체는 여기서 구현하지 않는다)
│
├── userland/                     # 포팅된 셸·coreutils 등 (ADR-005)
│   └── <각 도구>/CMakeLists.txt   # third_party/<도구> 소스에 대한 빌드 글루
│
├── third_party/                  # git submodule 원본 (ADR-022), 무수정 상태로 유지
│   ├── <libc 구현>/
│   ├── <coreutils류>/
│   ├── <shell>/
│   └── patches/
│       └── <프로젝트>/*.patch     # minicore 이식용 패치, 원본은 건드리지 않음
│
├── tools/                        # 개발 도구
│   ├── apply-patches.*            # submodule 체크아웃 위에 patches/ 적용
│   ├── mkinitrd.*                 # initrun 자신 + disk.cfg(부트 디바이스 서술자,
│   │                                # ADR-131)를 커널 임베딩용 initrd로 패키징
│   ├── mkbootdisk.py               # procsrv 등 서비스 바이너리 + NNN-이름.ini 시동
│   │                                # 파일을 담는 cpio(newc) 부트 디스크 이미지 생성
│   │                                # (ADR-131/150, mkinitrd와 별개 산출물)
│   └── run-qemu.*                 # 아키텍처별 QEMU 실행 스크립트 (ADR-131 이후 부트
│                                    # 디스크를 virtio-blk로 붙이는 옵션 포함)
│
└── docs/                          # 이 문서 체계 (spec/plan/done/design/remind)
```

## CMake 오케스트레이션 원칙 (ADR-019, ADR-021)

- 최상위 `CMakeLists.txt`는 `MINICORE_ARCH`(x86_64|aarch64)와 툴체인 선택만 받고,
  실제 컴파일 규칙은 각 서브프로젝트(`kernel/`, `servers/*`, `libc/`, `userland/*`)의
  `CMakeLists.txt`가 소유한다.
- `CMakePresets.json`으로 `x86_64-clang`, `x86_64-gcc`, `aarch64-clang`, `aarch64-gcc`
  네 조합을 1급 진입점으로 제공한다 (ADR-020).
- 서버/유저랜드 컴포넌트 목록은 최상위에서 "이 arch에서 무엇을 빌드하는가" 배열로
  관리한다 — 초기에는 x86_64만 전 구성 요소를 빌드하고, aarch64는 `kernel/`만
  컴파일 검증한다 (ADR-009).
- `k`(구 `libk`)는 커널과 시스템 서버가 함께 링크하는 헤더 전용/정적 라이브러리로 취급한다
  (ADR-010의 "커널 및 시스템 서비스" 범위와 일치 — 포팅된 `libc`/`userland`는 대상 아님).
- `mc`(구 `libmc`)는 ADR-200부터 **커널도 헤더만 참조**한다 —
  `kernel/`이 `libs/mc/include/mc/`를 `MC_LAND_KERNEL` 매크로와
  함께 include하지만, `mc`의 구현(.c) 파일은 여전히 유저랜드
  타깃에만 링크한다.

## 서드파티 소스 흐름 (ADR-022)

1. `third_party/<project>`는 원본 저장소를 가리키는 git submodule. 무수정.
2. `third_party/patches/<project>/*.patch`가 minicore 이식용 변경을 보관.
3. `tools/apply-patches.*`가 빌드 전 체크아웃 위에 패치를 순서대로 적용한
   작업 트리(`build/_patched/<project>`류)를 만든다.
4. `libc/`, `userland/<도구>/`의 CMakeLists.txt는 그 패치된 트리를 소스로 참조한다.
5. 패치 충돌 대응 절차는 실제 포팅 착수 시점에 별도 문서화한다 (ADR-022 영향 항목).

## 아직 정하지 않은 것

- 구체적으로 어떤 libc(예: musl/newlib 계열 중 무엇)와 어떤 셸/coreutils를
  submodule로 채택할지는 여기서 정하지 않는다 — 실제 포팅 착수 시점의 별도 결정.
