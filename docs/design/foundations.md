# 설계 결정: 프로젝트 기반 원칙

프로젝트 전체에 걸쳐 적용되는 목표, 아키텍처 범위, 언어, 코드 소유 경계에 관한 결정.

[← 설계 문서 색인](index.md)

---

## ADR-001. 프로젝트 목표: 범용 OS 지향

- **상태**: 확정 (2026-09-08)
- **결정**: 장기적으로 파일시스템·네트워크·셸을 갖춘 자립형 시스템을 목표로 한다.
  유저랜드 서버가 시스템의 무게중심이 되며, 커널은 그것을 지탱하는 최소 기반이다.
- **근거**: 목표가 "학습"이 아니라 "동작하는 시스템"이므로, 설계 판단이 갈릴 때는
  **확장 가능성**과 **서버 작성의 편의성**을 우선한다.
- **영향**:
  - 커널 API는 처음부터 다중 프로세스·다중 서버를 전제로 설계한다.
  - 단순함을 위해 기능을 빼기보다, 커널 밖(유저랜드)으로 미는 쪽을 택한다.
  - 성능은 "나중에 최적화 가능한 구조"만 확보하고, 초기엔 정확성 우선.

## ADR-002. 타겟 아키텍처: 다중 (HAL 분리)

- **상태**: 확정 (2026-09-08)
- **결정**: 처음부터 아키텍처 추상화 계층을 두고 2개 이상의 아키텍처를 지원한다.
- **근거**: 나중에 이식성을 넣는 것은 사실상 재작성이다. 초기 비용을 감수한다.
  두 번째 아키텍처는 "arch 의존 코드가 새어나갔는지"를 검출하는 리트머스 시험지 역할을 한다.
- **영향**:
  - 커널 코어(`kernel/core`)는 arch 헤더를 직접 include 하지 않는다.
  - CI는 모든 지원 아키텍처를 빌드해야 한다.
  - 구체적인 아키텍처 조합/1순위는 별도 결정 필요. → **미결정 (OPEN-1)**

## ADR-003. 구현 언어: C++ (freestanding)

- **상태**: 확정 (2026-09-08)
- **결정**: freestanding C++로 커널을 구현한다.
- **근거**: 템플릿/`constexpr`/RAII로 커널 자료구조의 불변식을 컴파일 타임에 표현하면서도,
  생성되는 코드는 C 수준으로 예측 가능하게 유지할 수 있다.
- **영향**:
  - 런타임 의존성(예외, RTTI, 동적 초기화 등) 제거 정책이 필요하다. → **미결정 (OPEN-2)**
  - 링커 스크립트 및 `crt` 초기화 경로를 직접 관리해야 한다.

## ADR-005. 유저랜드: 기존 소프트웨어 포팅/확장

- **상태**: 확정 (2026-09-08)
- **결정**: 셸과 사용자 툴은 새로 작성하지 않고 기존 소프트웨어(coreutils/busybox류,
  기존 셸, 기존 라이브러리)를 **포팅하거나 확장**하여 구성한다.
- **근거**: 범용 OS(ADR-001)의 유저랜드를 처음부터 작성하는 것은 비현실적이다.
  포팅 가능성을 확보하면 생태계 전체를 빌려올 수 있다.
- **영향** (매우 큼):
  - **POSIX 호환 계층이 선택이 아니라 필수 요구사항이 된다.** 커널 API는
    `fork`/`exec`/`fd`/`signal`/`mmap`의 의미론을 **구현 가능하게** 만들어야 한다.
    - 특히 `fork`는 마이크로커널에서 비싸다 → 주소공간 복제/COW를 유저랜드에서
      표현할 수 있어야 한다 (커널이 페이지 폴트를 유저 pager에게 위임하는 능력 필요).
  - **C 툴체인이 1급 시민이다.** 커널은 C++이지만, 유저랜드 ABI는 C이어야 하며
    표준 libc(newlib/musl 등) 이식 대상이 되어야 한다.
  - **파일 디스크립터 추상화**가 필요하다. POSIX fd → 커널 객체 핸들 매핑 계층 설계 필요.
  - 빌드 시스템은 크로스 컴파일 sysroot 및 서드파티 소스 패치 관리를 다뤄야 한다.
  - 구체적인 POSIX 계층 배치는 별도 결정 필요. → **미결정 (OPEN-8)**

## ADR-006. 커널·드라이버·시스템 서비스는 직접 구현

- **상태**: 확정 (2026-09-08)
- **결정**: 커널, 디바이스 드라이버, 시스템 서비스는 **외부 코드를 가져오지 않고 직접 구현**한다.
  포팅(ADR-005)은 그 위 계층 — 애플리케이션, 셸, 사용자 툴 — 에만 허용된다.
- **경계선**:

  | 계층 | 정책 |
  |---|---|
  | 커널 (IPC, 스케줄러, 주소공간, 객체) | 직접 구현 |
  | 디바이스 드라이버 (콘솔, 타이머, 블록, NIC, 버스) | 직접 구현 |
  | 시스템 서비스 (VFS, 파일시스템, 네트워크 스택, 프로세스 서버, 디바이스 매니저) | 직접 구현 |
  | ─── 이 선 위로는 포팅 허용 ─── | |
  | libc / POSIX 호환 계층 | 직접 구현 또는 기존 libc 이식 (미결정 OPEN-8) |
  | 셸, coreutils류, 애플리케이션 | **포팅/확장** (단, 개념 불일치 시 대체 구현 허용 — ADR-049) |

- **근거**: 이 프로젝트의 본질적 가치는 커널과 시스템 서비스의 설계에 있다.
  그 아래를 빌려오면 배울 것도, 통제할 것도 남지 않는다. 반대로 그 위는
  빌려오지 않으면 완성되지 않는다.
- **영향**:
  - 파일시스템·TCP/IP·드라이버를 **전부 자체 구현**해야 하므로 작업량이 매우 크다.
    → 계획 단계에서 우선순위와 최소 집합(MVP)을 엄격히 정해야 한다.
  - "포팅된 앱이 요구하는 기능"과 "직접 구현할 서비스의 범위"가 만나는 지점이
    곧 POSIX 계층이며, 이것이 프로젝트의 병목이 된다.
  - 드라이버가 커널 내부인지 유저 프로세스인지는 별도 결정. → **미결정 (OPEN-9)**

## ADR-007. 드라이버/서비스 실행 위치: 코어만 커널, 나머지는 유저 (해결: OPEN-9)

- **상태**: 확정 (2026-09-08)
- **결정**: 커널 내부에는 다음만 둔다 — 타이머, 인터럽트 컨트롤러(APIC/GIC),
  시리얼/초기 콘솔(디버그용), MMU/페이지테이블 조작. **그 외 모든 드라이버**
  (블록, 네트워크, USB, GPU, 버스 열거 등)는 유저 프로세스로 구현한다.
- **근거**: ADR-006(직접구현)과 결합하면 유저 드라이버가 "장애 격리·재시작 가능한
  마이크로커널"이라는 프로젝트의 정체성을 지킨다. 코어 예외는 부트 초기 단계(유저
  프로세스가 아직 없는 시점)와 스케줄링에 근본적으로 필요한 것들뿐이다.
- **영향**:
  - 커널은 유저 드라이버에게 인터럽트를 **비동기 알림(ADR-004)**으로 전달하고,
    디바이스 MMIO 영역을 해당 드라이버 주소공간에 매핑할 수단(capability)을 제공해야 한다.
  - 드라이버 격리·재시작 정책(감시자/재시작 서버)은 시스템 서비스 설계에서 다룬다.

## ADR-008. POSIX 계층: 서버 분해 (Hurd류) (해결: OPEN-8)

- **상태**: 확정 (2026-09-08)
- **결정**: POSIX 의미론을 하나의 "POSIX 서버"에 몰아넣지 않고, 역할별 서버로 분해한다.
  최소 구성: **프로세스 서버**(fork/exec/signal/wait/pid), **VFS 서버**(경로 탐색,
  마운트, fd 계층의 상위 라우팅), **파일시스템 서버**(마운트당 1개 이상, VFS 뒤에 위치),
  **네트워크 서버**(소켓, 프로토콜 스택). libc는 이 서버들과 통신하는 클라이언트가 된다.
- **근거**: ADR-001(범용)과 ADR-006(직접구현 다수)을 감안하면 파일시스템·네트워크는
  각각 독립적으로 죽고 재시작될 수 있어야 한다. 단일 POSIX 서버는 이 프로젝트가
  떠안을 구현량(파일시스템 여러 개, TCP/IP)에 비해 단일 장애점 위험이 너무 크다.
- **영향** (Hurd의 알려진 어려움을 그대로 인지하고 감수):
  - **fork 의미론이 프로젝트에서 가장 어려운 문제가 된다.** 자식 프로세스는
    부모가 열어둔 모든 fd(여러 서버에 흩어진 상태)를 이어받아야 한다.
    → 프로세스 서버가 "fd 테이블의 진실 공급원"이 되고, 각 fd는
    (서버 캐패빌리티, 서버측 핸들) 쌍으로 표현한다. 상세 설계는 design 문서로 분리 예정.
  - VFS 서버와 파일시스템 서버 간의 프로토콜(lookup/read/write/getattr 등)을
    안정적인 인터페이스로 명세해야 한다 (향후 spec 대상).
  - 초기 구현 순서는 반드시 "프로세스 서버 + 최소 VFS + 메모리 파일시스템 1개"로
    수직 슬라이스를 먼저 완성하는 것을 권장 (계획 단계에서 확정).

## ADR-009. 아키텍처 우선순위: x86_64 우선, aarch64 후속 (해결: OPEN-1)

- **상태**: 확정 (2026-09-08)
- **결정**: 1차 타겟은 **x86_64**(QEMU q35), 2차 타겟은 **aarch64**(QEMU virt)로 한다.
  HAL 인터페이스는 처음부터 두 아키텍처를 함께 염두에 두고 설계하지만, 구현은
  x86_64를 먼저 끝까지(부팅→유저모드→IPC→드라이버) 완성한 뒤 aarch64로 이식한다.
- **근거**: x86_64는 자료·툴체인·QEMU 지원이 가장 풍부해 초기 개발 마찰이 적다.
  aarch64는 예외 레벨과 GIC가 x86의 APIC/GDT/IDT와 충분히 달라 HAL 경계를 검증하기 좋다.
- **영향**:
  - GDT/IDT/APIC/페이지테이블(4-level, 추후 5-level 고려)은 x86_64 arch 계층에 캡슐화.
  - CI에 aarch64 빌드를 처음부터 넣되(컴파일까지), 부팅 검증은 x86_64가 안정된 후 추가.
  - riscv64는 로드맵에는 두되 현재 범위에는 포함하지 않는다.

## ADR-010. C++ 부분집합: 예외/RTTI 없음 + freestanding 표준 헤더 허용 (해결: OPEN-2)

- **상태**: 확정 (2026-09-08)
- **결정**: 커널 및 시스템 서비스는 `-fno-exceptions -fno-rtti`로 빌드한다.
  프리스탠딩이 보장하는 표준 헤더(`<type_traits>`, `<concepts>`, `<bit>`, `<limits>`,
  `<atomic>`, `<cstdint>`, `<utility>`, `<new>`(placement) 등)는 허용한다.
  동적 할당을 전제하는 표준 컨테이너/`<new>`의 일반 형태, `<iostream>`류는 금지.
  오류 처리는 예외 대신 반환 기반(`result<T,E>`/`optional<T>` 자체 구현)을 사용하되,
  ADR-010 시점에는 강제(`[[nodiscard]]` 필수화)까지는 정하지 않고 관례로 시작한다.
- **근거**: 예외/RTTI는 마이크로커널에 요구되는 결정론적 실행 시간·작은 이진 크기와
  상충한다. 반면 freestanding 표준 헤더는 링크 의존성이 없어 "쓰지 않을 이유가 없다".
  result 강제는 초기 개발 속도를 늦출 수 있어 지금은 관례로 두고 필요 시 ADR로 승격한다.
- **영향**:
  - 자체 코어 라이브러리(`kernel/libk` 또는 유사)에 `result`, `optional`, `span`,
    `intrusive_list`, `atomic` 래퍼 등을 구현해야 한다.
  - 유저랜드 libc(포팅 대상, ADR-005)는 이 정책의 적용 대상이 아니다 — C 코드이거나
    포팅된 코드이므로 별도 언어 정책을 따른다.

## ADR-042. 네이밍 컨벤션: snake_case + 인터페이스는 `_interface` 포스트픽스

- **상태**: 확정 (2026-09-08)
- **결정**: 커널·시스템 서버(ADR-010 범위) 코드의 모든 식별자(타입,
  함수, 변수, 네임스페이스, enum 값)는 **snake_case**를 사용한다.
  타입·함수 구분을 위한 별도의 대소문자 스타일은 두지 않는다. 순수
  인터페이스(모든 멤버가 pure virtual인 추상 베이스 클래스)는 `IFoo`
  처럼 이름 앞에 프리픽스를 붙이지 않고, **`_interface` 포스트픽스**로
  표시한다 — 예: `block_device_interface`, `driver_interface`.
- **근거**: 이 문서 체계에 이미 작성된 스펙들(boot.md, ipc.md 등)의
  코드 예시가 PascalCase(`BootInfo`, `Message` 등)로 쓰여 있었는데,
  프리픽스 기반 인터페이스 구분(`IFoo`류)보다 포스트픽스가 이름의
  핵심 의미를 앞에 유지해 가독성이 낫다는 판단으로 스타일을 통일한다.
  C++ 표준 라이브러리도 `std::optional`, `std::span`처럼 소문자
  스타일을 쓰므로 낯설지 않다.
- **영향**:
  - 이미 작성된 spec 문서(`boot.md`, `ipc.md`, `debug-console.md`,
    `pcie.md`)와 이 결정 기록 자체의 코드 예시를 전부 snake_case로
    소급 수정했다 — `boot_info`, `memory_region`, `message`,
    `page_descriptor`, `transfer_mode`, `ipc_error`, `badge`,
    `endpoint`, `notification`, `handle`, `mcpack_entry`,
    `mcpack_header`, `devmgr_op`, `result<T,E>`, `optional<T>`,
    `span<T>`, `intrusive_list`, `atomic` 등.
  - 앞으로 작성되는 모든 커널·시스템 서버 코드와 spec 문서 예시는
    이 컨벤션을 따른다.
  - 포팅된 코드(libc/userland, ADR-005/022)는 원본 프로젝트의
    컨벤션을 그대로 따르며 이 규칙의 적용 대상이 아니다.
  - 매크로·상수(`k_` 프리픽스 등)의 세부 관례는 아직 완전히
    확정하지 않았다 — 실제 코드 작성 시 `libk` 라이브러리에서
    선례를 만들며 정착시킨다.

## ADR-049. 포팅 실패·개념 불일치 시 대체 구현 허용 (ADR-005 보강)

- **상태**: 확정 (2026-09-08)
- **결정**: ADR-005의 "기존 소프트웨어는 포팅/확장" 원칙에 예외를
  둔다. 특정 소프트웨어가 (a) minicore의 커널·VFS 모델과 근본적으로
  다른 설계 개념(예: 다른 프로세스/파일시스템/스레딩 가정)에 의존해
  패치만으로는 현실적으로 포팅이 불가능하거나, (b) 포팅 비용이
  새로 작성하는 것보다 명백히 크다고 판단되면, 그 컴포넌트에 한해
  **포팅을 포기하고 minicore 전용으로 새로 설계·구현**하는 것을
  허용한다.
- **근거**: ADR-005의 목적은 "생태계를 빌려와 유저랜드 완성을
  가속화"하는 것이지 포팅 자체가 목적이 아니다. 개념 불일치로 인한
  무한 패치·유지보수 지옥까지 감수하며 포팅을 강제하면 오히려 원래
  목표(빠른 완성)에 반한다.
- **영향**:
  - 포팅 대신 대체 구현을 선택한 근거(포팅 비용 vs 재작성 비용
    추정)는 해당 컴포넌트의 계획 단계에서 `docs/plan` 또는
    `docs/design`에 문서화해야 한다 — "그냥 귀찮아서"가 아니라
    구체적 불일치 지점을 남긴다.
  - 대체 구현으로 결정된 컴포넌트는 유저랜드 애플리케이션이어도
    ADR-006의 "직접 구현" 계열로 취급한다 — ADR-006 경계선 표에
    각주로 반영.
  - 이 예외는 남용하지 않는다 — 기본값은 여전히 포팅(ADR-005)이며,
    대체 구현은 근거가 명확할 때만 선택한다. 지금 시점에는 구체적으로
    어떤 컴포넌트를 대체할지 정하지 않는다 — 실제 포팅 착수 시점에
    컴포넌트별로 판단한다.

## ADR-132. libmc: minicore 네이티브 유저랜드 API를 순수 C+어셈블러로 제공하는 기반 라이브러리, libc는 그 위의 클라이언트 (ADR-005/008 보강)

- **상태**: 확정 (2026-09-09)
- **결정**: 커널 syscall과 시스템 서버 IPC 프로토콜에 대한 **1:1(또는
  최소한으로 확장된) C 바인딩**을 제공하는 새 라이브러리
  **`libmc`**(**LIB**rary for **M**iniCore userland APIs)를 만든다.
  순수 **C(freestanding에 가까운 부분집합)와 어셈블러로만** 작성하고
  C++을 전혀 쓰지 않는다.
  1. **유저랜드 계층 구조를 명시적으로 3단으로 정리한다**:
     - **0단(커널)**: syscall ABI(ADR-122).
     - **1단(`libmc`)**: 이 ADR. 모든 syscall의 얇은 1:1 C 래퍼
       (`mc_ipc_call`/`mc_ipc_recv`/`mc_ipc_reply`/`mc_ipc_notify`/
       `mc_ipc_wait`/`mc_handle_close`, ADR-131의
       `mc_process_spawn`(원본 ELF 바이트를 받아 프로세스를 만드는
       래퍼) 등)와, 각 시스템 서버
       프로토콜(procsrv의 fork/exec/계정/로그인, VFS의 open/stat,
       FS 서버 공통 프로토콜의 read/write/close/lock/공유모드, devmgr
       등록, cfgsrv get/set)에 대한 **표준 클라이언트 구현**을 담는다
       — 메시지 조립·다중 Call 분할(procsrv.md §4.1의 "handle_count
       초과 시 여러 번 나눠 호출" 같은 반복 패턴) 등 프로토콜
       그 자체의 "약속을 지키는 코드"를 여기 한 곳에만 둔다.
     - **2단**: 여기서 갈라진다 — (2a) **minicore 네이티브 서버/앱**
       (procsrv/vfs/devmgr/cfgsrv/drivers/initrun, ADR-006/007이
       직접구현으로 정한 것들)은 `libk`(C++ freestanding 유틸)와
       `libmc`(C 바인딩, `extern "C"`로 링크)만 링크하고 **libc가
       전혀 필요 없다**. (2b) **포팅된 libc**(ADR-005/008)는
       `libmc`를 링크하고, `libc/sysdeps/minicore/`는 이제 "커널
       syscall·서버 IPC를 직접 구현"하지 않고 **libc 내부 훅을
       `libmc`의 `mc_*` 호출로 옮기기만 하는 얇은 어댑터**가 된다.
     - **3단**: 포팅된 유저랜드 앱/셸(ADR-005)은 2b의 libc만 쓴다 —
       `libmc`를 직접 보지 않는다.
  2. **프로토콜 와이어 포맷(구조체/오퍼레이션 레이블)의 단일 출처는
     `libmc`의 C 헤더**로 삼는다 — 서버 쪽 C++ 구현(예: procsrv
     자신)도 이 헤더를 `extern "C"`로 포함해 클라이언트/서버가 같은
     레이아웃에 합의함을 컴파일 타임에 보장한다(POD 구조체는 C/C++
     양쪽에서 레이아웃이 같다). `docs/spec/*.md`의 프로토콜 의사코드는
     여전히 사람이 읽는 명세로 유지하되, 실제 정의는 `libmc` 헤더가
     정본이다.
  3. **네이티브 프로세스 진입점**: `libmc`가 최소한의 `_start`(어셈블러)를
     제공한다 — ADR-131이 정한 진입 레지스터 관례(RDI=boot_info,
     RSI=argv blob)를 파싱해 통상적인
     `int main(int argc, char** argv, const struct mc_boot_info* bi)`
     형태로 넘겨준다. `sys_process_spawn`/향후 일반 exec() 어느
     경로로 만들어진 프로세스든 이 하나의 진입 관례를 공유한다.
  4. **저장소 배치**: 새 최상위 디렉터리 `libmc/`
     (`libmc/include/mc/`, `libmc/src/<arch>/`(syscall 트램폴린 asm),
     `libmc/src/ipc/`(서버별 프로토콜 클라이언트, `servers/` 하위
     구조와 이름을 맞춘다)) — `libk/`·`libc/`와 나란한 위치.
     [repo-layout.md](repo-layout.md)를 이 ADR과 함께 갱신한다.
- **근거**: 사용자가 "libc 포팅과 별개로, 자체 메커니즘과 1:1로
  대응/확장되는 순수 C+어셈블러 유저랜드 라이브러리가 있으면 libc
  포팅이 단순해질 것"이라고 제안했다 — 확인해보니 이 필요는
  ADR-008이 이미 "libc는 서버들과 통신하는 클라이언트가 된다"고
  선언해 둔 자리에 정확히 들어맞는다: ADR-008은 **누가** 클라이언트
  역할을 하는지는 말했지만 **그 클라이언트 구현을 어디 둘지**는
  정하지 않았다 — 지금까지는 암묵적으로 "libc 포팅 글루 코드 안에"
  였는데, 이러면 libc 구현체(musl/newlib 등, ADR-005)를 바꾸거나
  여러 개를 동시에 지원하려 할 때마다, 또는 (2a)의 네이티브 서버가
  같은 프로토콜을 다시 구현해야 할 때마다 **프로토콜 로직 자체가
  중복·분기**된다. `libmc`를 libc와 완전히 독립된 순수 C 라이브러리로
  분리하면: (a) 어떤 libc를 포팅하든 동일한 `libmc`를 그대로 재사용
  하고 `sysdeps/minicore`만 얇게 다시 쓰면 되며, (b) 애초에 POSIX와
  전혀 무관한 minicore 네이티브 서버들(procsrv 자신부터가 그렇다)도
  자기가 만드는 프로토콜의 클라이언트 절반(다른 서버를 호출하는
  쪽)을 위해 `libmc`를 그대로 쓸 수 있어 서버 코드에서도 중복이
  없어진다. **순수 C(+asm)로 못박은 이유**는 이식 대상 libc 구현체
  대부분이 C로 작성되어 C++ 런타임(예외 처리 없음이어도
  네임맹글링·정적 초기화 관례 등)에 대한 의존을 요구하지 않는 것이
  안전하기 때문이다 — `libk`(ADR-066+)는 이미 C++ 전용으로 확정돼
  있으므로, `libmc`를 `libk` 위에 쌓지 않고 **완전히 별개의 낮은
  계층**으로 둬야 이 제약이 지켜진다.
- **영향**:
  - `docs/plan/system-servers-bringup.md`(M12~M20)에 `libmc`가
    각 마일스톤에 걸쳐 점진적으로 채워지는 교차 트랙으로
    반영돼야 한다(이 문서와 함께 갱신) — M12에서 syscall 래퍼
    +procsrv 클라이언트, M13에서 VFS/FS 프로토콜 클라이언트,
    M14에서 devmgr 등록 클라이언트, M17에서 콘솔/로그인 클라이언트,
    M18에서 신뢰 위임 관련 호출, M19에서 cfgsrv 클라이언트, M20에서
    비로소 `libc/sysdeps/minicore`가 이를 소비.
  - `servers/*`(procsrv 등)의 CMake 링크 대상이 `libk`+`libmc`로
    바뀐다 — repo-layout.md의 servers/ 서브프로젝트 설명을 이
    ADR과 함께 갱신한다.
  - `libc/sysdeps/minicore/`의 역할 설명("커널 syscall·서버 IPC를
    libc에 연결하는 직접 구현 계층")이 낡았다 — "libmc 호출로
    연결하는 얇은 어댑터"로 repo-layout.md를 갱신한다.
  - `libmc` 자체의 빌드 방식(정적 라이브러리 하나로 전체 아키텍처를
    감당할지, 프로토콜별로 더 잘게 나눌지)과 각 syscall/프로토콜
    함수의 정확한 시그니처는 이 ADR의 범위 밖 — M12 착수 시점에
    `docs/spec/`(가칭 `libmc-abi.md`)로 구체화한다(ADR-043이 이미
    확립한 "세부는 착수 시점에" 관례).
  - aarch64 syscall 트램폴린 asm은 aarch64 이식 계획(별도, ADR-009)
    시점에 추가한다 — 지금은 x86_64만 작성한다.

## ADR-170. M20 범위 좁힘: libc 채택 보류 + libmc 최소 부분집합 + 커스텀 셸(대체 구현)

- **상태**: 확정 (2026-09-09)
- **결정**: `system-servers-bringup.md` §M20("libc/POSIX 최소 포팅 +
  로그인 후 셸")을 시작하며, 사용자에게 `AskUserQuestion`으로 확인한
  세 가지 범위 결정을 기록한다.
  1. **실제 서드파티 libc(musl 등) 포팅은 이번 라운드에서 하지
     않는다.** ADR-132가 이미 지적한 대로 `libmc`가 M12부터
     점진적으로 채워졌어야 했지만 실제로는 `libmc/` 디렉터리 자체가
     아직 없다 — 이번 라운드는 그 첫 착수로, ADR-132가 정의한 **1단
     (`libmc`)만** 실제로 만든다. libc 자체(2b/3단)를 어떤
     구현체로 채택할지는 여전히 미결정으로 남긴다(repo-layout.md
     §"아직 정하지 않은 것"이 이미 그렇게 정해 뒀다) — 다음 기회에
     다시 판단한다.
  2. **"로그인 후 셸"은 실제 포팅된 셸이 아니라 minicore 전용으로
     새로 작성한 최소 셸이다** — ADR-049(포팅 실패·개념 불일치 시
     대체 구현 허용)를 근거로 삼는다: 이 커널은 syscall이 12개뿐이고
     open/read/write/mmap/brk/fork/exec가 전부 procsrv/VFS/FS IPC로
     구현되어 있어, 실제 POSIX 셸(dash/bash 등)이 기대하는 fd·프로세스
     모델과 근본적으로 다르다(ADR-005 §영향이 이미 예견한 지점).
     이 셸(`userland/shell/`)은 **libc 없이 `libmc`만 링크하는 순수
     C 네이티브 앱**이다(ADR-132 §결정 1의 "2a: libc가 전혀 필요
     없는 minicore 네이티브 앱"에 해당 — 애초 구상은 서버·드라이버만
     염두에 뒀지만, 이 셸도 같은 조건(순수 C, IPC 클라이언트뿐)을
     만족해 같은 분류로 둔다).
  3. **`ls`/`cat`은 별도 프로세스가 아니라 셸 프로세스 안의
     빌트인 명령이다** — ADR-008이 이 프로젝트의 "가장 어려운 문제"
     로 이미 지적한 fork 의미론(procsrv가 여러 서버에 흩어진 fd를
     자식에게 상속시키는 문제)을 이번 라운드에서 실제로 풀지 않고
     명시적으로 미룬다. 셸이 su/sudo류로 다른 신원의 **별도 외부
     프로그램**을 실행하는 일반 메커니즘(ADR-090)은 이번 범위 밖이다.
- **근거**: 세 결정 모두 사용자가 명시적으로 선택했다(1은 세 옵션 중
  "libmc 먼저", 2·3은 각각의 권장 단순화). 규모상 이 마일스톤은
  M12~M19 어느 것보다 훨씬 크다 — 실제 musl 포팅과 실제 fork 의미론
  구현을 한 라운드에 모두 시도하면 검증 가능한 결과물 없이 끝날
  위험이 크다고 판단해, "로그인 후 셸 프롬프트+ls/cat으로 파일 조회"
  라는 계획의 **기능적** 완료 기준(구현 방법을 특정하지 않음)을
  최소 인프라로 실제로 충족하는 쪽을 택했다.
- **영향**:
  - `libmc/`를 새 최상위 디렉터리로 만든다 — repo-layout.md가 이미
    이 자리를 예약해 뒀다. 이번 라운드는 syscall 1:1 래퍼
    (`mc_ipc_call`/`mc_ipc_recv`/`mc_ipc_reply`/`mc_debug_log`/
    `mc_thread_exit`)와 VFS/FS 공통 프로토콜·콘솔·ps2 클라이언트만
    채운다 — procsrv 클라이언트(로그인/su), devmgr 등록, cfgsrv
    클라이언트는 이번 셸이 쓰지 않아 비워 둔다(다음에 실제로
    필요해지는 시점에 추가).
  - "로그인 후 셸이 시작되는" 정확한 메커니즘은 ADR-089가 구상한
    "procsrv가 session_program을 exec()"이 아니라 **더 단순한
    대체**를 쓴다 — [security-model.md](security-model.md)에 별도
    ADR로 기록한다(ADR-089 자체는 대체하지 않는다 — 이번 라운드의
    단순화일 뿐, 실제 계정별 session_program exec은 이후 과제로
    남는다).
  - `ls`가 필요로 하는 목록 조회 오퍼레이션이 fs-protocol에 아직
    없다 — [filesystem.md](filesystem.md)에 별도 ADR로 fs-protocol
    v4(`OP_LIST`, memfs 전용)를 기록한다.
  - `docs/plan/system-servers-bringup.md` §M20의 "구현" 문구(서드파티
    libc+셸 서브모듈)는 이번 라운드에 실행하지 않은 계획으로 남는다
    — 계획 문서 자체는 수정하지 않는다(문서 체계 원칙).

## ADR-182. M26 범위 좁힘: musl 문자열 함수 부분집합만 실제로 포팅(전체 syscall 계층은 범위 밖)

- **상태**: 확정 (2026-09-10)
- **결정**: [general-purpose-completion.md §M26](../plan/general-purpose-completion.md)
  ("M20/ADR-170이 미뤄 둔 실제 서드파티 libc 포팅을 M21~M24가 갖춘
  전제 위에서 다시 시도")을 시작하며, "어디까지 포팅할지는 착수
  시점에 다시 범위를 좁힌다"는 계획 자신의 문구를 그대로 행사한다.
  1. **musl을 `third_party/musl`(v1.2.6 고정, git submodule, ADR-022)
     로 채택한다.** repo-layout.md §"아직 정하지 않은 것"이 미뤄 둔
     결정이다.
  2. **musl 소스 중 문자열 함수(`src/string/*.c`)만 무수정으로
     빌드한다** — `memcpy`/`memmove`/`memset`/`memcmp`/`memchr`/
     `memrchr`/`strcmp`/`strncmp`/`strcpy`/`stpcpy`/`strncpy`/
     `strcat`/`strncat`/`strchr`/`strchrnul`/`strrchr`/`strspn`/
     `strcspn`/`strlen`/`strdup`(그리고 이들이 내부적으로 요구하는
     지원 함수) — 이 전부가 syscall/스레드/락 없이 완결된다(YAGNI —
     이 부분집합만으로도 "재구현이 아닌 실제 서드파티 소스"라는
     계획의 취지를 충족한다). musl 자신의 빌드 시스템(Makefile,
     `./configure`)은 통째로 쓰지 않는다 — 이 커널의 타깃 트리플
     (freestanding `x86_64-unknown-none-elf`)에 맞지 않고, 이
     부분집합에는 필요하지도 않다. 대신 musl의 `bits/alltypes.h`
     생성 규칙(`tools/mkalltypes.sed`)만 파이썬으로 재현한다
     (`tools/gen-musl-alltypes.py` — sed 출력과 개행 차이만 있고
     내용은 동일함을 확인했다, 이 프로젝트의 다른 생성 스텝들과
     셸 의존성을 통일하는 김에).
  3. **`strdup`이 요구하는 `malloc`/`free`/`calloc`/`realloc`은
     M24(ADR-180)의 `mc_malloc`/`mc_free`로 연결한다**
     ([libc/sysdeps/minicore/mem_shim.c](../../libc/sysdeps/minicore/mem_shim.c)
     — repo-layout.md가 이미 예약해 둔 "libc 내부 훅을 libmc의
     mc_* 호출로 연결하는 얇은 어댑터" 자리를 처음으로 채운다).
  4. **진짜 syscall 계층(open/read/write/mmap/fork/exec을 musl
     자신의 경로로), 동적 링커, 스레드(pthread), locale, stdio
     (FILE/printf 계열)는 이번 라운드에 포함하지 않는다** — 이건
     M20이 원래 "M12부터 점진적으로 채워졌어야 했다"고 지적한
     `libmc`의 2b/3단(ADR-132)에 해당하는 훨씬 큰 작업이고, 이
     커널의 14개 syscall을 musl이 기대하는 syscall(2) 형태로 감싸는
     새 레이어가 필요하다(사실상 M22~M24가 이미 하고 있는 procsrv
     매개 POSIX 호환 작업의 훨씬 큰 확장). **M26의 실제 검증
     목표("M20의 완료 기준을 포팅된 libc+포팅된 셸/coreutils로
     다시 달성")는 이 범위 좁힘으로 문자 그대로는 충족되지
     않는다** — 셸/coreutils 자체는 여전히 minicore 네이티브
     구현(ADR-170)이고, 다만 그 안에서 문자열 처리 일부가 이제
     **진짜 musl 소스**로 이루어진다는 것만 증명한다.
- **근거**: ADR-170(M20)과 같은 이유(규모상 이 마일스톤은 검증
  가능한 결과물 없이 끝날 위험이 크다)에, 이 커널의 근본적
  아키텍처(모든 I/O가 procsrv/VFS/FS IPC로 구현되고 syscall이
  12개 — 이제 M22~M24로 14개 — 뿐)가 musl이 기대하는 전제(POSIX
  syscall ABI, 진짜 fd, 진짜 mmap)와 아직도 근본적으로 다르다는
  ADR-170의 진단이 그대로 유효하다. 다른 점은, 이번엔 "libmc를
  먼저 만든다" 대신 "musl의 **syscall과 무관한 부분**부터 실제
  소스로 증명한다"는 더 구체적인 다음 단계를 골랐다는 것이다 —
  전체 포팅으로 가는 길에서 실제로 가장 먼저, 가장 안전하게 뗄 수
  있는 조각이기 때문이다(문자열 함수는 이 커널의 어떤 IPC/syscall
  모델과도 마찰이 없다).
- **실제로 겪은 문제(빌드 시스템)**: `target_include_directories`로
  musl의 `arch/x86_64`·`arch/generic`·생성된 `bits/alltypes.h`를
  `minicore_libc` 자신에게만(PRIVATE) 노출했다가, `string.h`를
  쓰는 소비자(`userland/shell`)가 그 헤더들이 내부적으로 요구하는
  `bits/alltypes.h`/`bits/stdint.h`를 못 찾아 두 번 연달아 빌드가
  깨졌다 — musl의 공개 헤더(`string.h` 등)를 쓰는 모든 소비자는
  그 헤더가 참조하는 arch별 `bits/*.h`와 생성 헤더까지 함께 봐야
  하므로, 이 셋을 전부 **PUBLIC**으로 승격해 해결했다(반면
  `src/include`/`src/internal`의 `weak_alias` 등은 musl 자신의
  `.c` 파일만 필요로 해 PRIVATE로 남긴다).
- **검증 결과(QEMU 실측)**: [general-purpose-completion-m26.md](../done/general-purpose-completion-m26.md)
  참고 — `userland/shell`이 musl의 `strcpy`+`strcat`으로 문자열을
  조립하고 `strdup`+`strlen`+`memcmp`로 왕복 검증한다
  (`"[shell] libc strcpy/strcat/strdup ok=1"`, `tools/smoke-test-x86_64.sh`
  에 추가). QEMU 첫 실행에 바로 통과했다(빌드 시스템 문제 두 건은
  전부 컴파일 단계에서 잡혔고, 런타임 동작 자체는 처음부터
  올바랐다). smoke/net/SMP/NUMA/AVX 5개 스위트 전부 회귀 없음.
- **영향**:
  - `docs/plan/general-purpose-completion.md` §M26의 "구현" 문구
    (전체 libc 포팅+포팅된 셸/coreutils)는 이번 라운드에 실행하지
    않은 계획으로 남는다 — 계획 문서 자체는 수정하지 않는다(문서
    체계 원칙, ADR-170과 동일).
  - `third_party/musl`은 이제 이 저장소의 첫 실제 git submodule이다
    (`.gitmodules`, ADR-022가 예상해 둔 그대로) — `third_party/patches/musl/`
    는 비어 있다(이번 라운드는 musl 소스를 무수정으로 쓴다, 패치가
    필요 없었다). `tools/apply-patches.sh`는 여전히 TODO 스텁이다
    (이번 라운드는 패치가 필요 없어 구현하지 않았다 — 실제 패치가
    필요해지는 다음 기회로 미룬다).
  - **OPEN-66**: 전체 syscall 계층/동적 링커/스레드/stdio 포팅은
    여전히 없다 — [open-items.md](open-items.md) 참고.

## ADR-183. 실제 musl syscall 계층 포팅 전략: 커널이 아니라 musl 자신의 syscall 진입점을 패치해 유저랜드에서 우회

- **상태**: 확정 (2026-09-10, 계획 단계 — [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
  전체(M27~M39) 착수 전에 전략만 먼저 결정한다)
- **결정**:
  1. OPEN-66(진짜 syscall 계층)을 풀기 위해 **커널에 Linux 호환
     syscall ABI 진입점을 새로 추가하지 않는다.** 대신 musl 소스
     자신의 `arch/x86_64/syscall_arch.h`(모든 `__syscall0`~`__syscall6`
     호출이 반드시 거치는 단일 지점)를 `third_party/patches/musl/`의
     실제 패치(ADR-022가 예약해 둔 자리, M26까지는 한 번도 쓰이지
     않았다)로 재작성해, 원래 raw `syscall` 어셈블리 명령 대신 새
     C 디스패처 `long __minicore_syscall_dispatch(long n, ...)`
     (`libc/sysdeps/minicore/syscall_shim.c`, `extern "C"`)를
     호출하게 만든다. 이 디스패처가 Linux syscall 번호(musl이 그대로
     쓰는 표준 x86_64 Linux ABI 번호)를 보고 필요한 최소 집합만
     실제 동작(`libmc`의 VFS/FS/procsrv 클라이언트 호출, 또는 기존
     14개 커널 syscall 중 하나)으로 번역하고, 나머지는 `-ENOSYS`를
     반환한다.
  2. **정적 링킹이 기본값이다** — 동적 링커(`ld.so`)는 이 ADR
     시점에는 포팅 대상이 아니다(musl은 정적으로 링크된 실행파일만
     만든다). 이후 ADR-189(kernel-memory.md)가 "단일 `.so` 최소
     증명"만 범위를 좁혀 별도로 다룬다 — 일반적인 동적 링킹까지
     지원하는 것으로 이 결정을 번복하지는 않는다.
  3. `tools/apply-patches.sh`(M26까지 TODO 스텁이었다 — 패치가
     필요 없어서 구현되지 않았다)를 이 전략을 위해 실제로 구현한다.
  4. **`syscall_shim.c`는 항상 `libmc`를 거쳐서만 커널/서버와
     통신한다 — 절대 새 IPC/프로토콜 로직을 직접 구현하지 않는다.**
     구현해야 할 Linux syscall 번호에 대응하는 `libmc` 함수(예:
     `mc_open`/`mc_read`/`mc_arch_prctl`/`mc_thread_create`/
     `mc_futex_wait` 등)가 아직 없으면, **`syscall_shim.c`를 고치기
     전에 먼저 `libmc`에 그 함수를 추가**한다(헤더는
     `libmc/include/mc/`, 구현은 `libmc/src/`) — ADR-132가 이미
     정한 "0단 커널 syscall의 1:1 C 래퍼 + 서버별 프로토콜 클라이언트는
     전부 `libmc` 한 곳에만 둔다"는 원칙을 musl 포팅에도 예외 없이
     적용한다. `libc/CMakeLists.txt`의 `minicore_libc`(정적)/
     `minicore_libc_dynamic`(동적, M29~)는 항상 `libmc`와 링크해
     빌드한다(M26이 `mem_shim.c`→`mc_malloc`으로 이미 시작한 패턴을
     이 계획 전체로 일반화한다).
- **근거**: ADR-006/007("커널 내부는 최소한만 직접 구현, 나머지는
  유저랜드로 미룬다")과 정확히 같은 논리다 — "Linux syscall 번호를
  minicore IPC로 번역하는 것"은 순전히 유저랜드 문제(어떤 번호가
  어떤 VFS/procsrv 오퍼레이션에 대응하는가)이고, 커널이 새로 알아야
  할 것이 없다. 이 커널의 기존 14개 syscall과 IPC 프로토콜은 이미
  이 번역에 필요한 능력(open/read/write→VFS/FS IPC, fork/exec→
  sys_fork/sys_exec, brk→sys_brk)을 제공한다 — "새 커널 syscall ABI"를
  또 하나 만드는 것은 이미 있는 능력의 중복이자, 커널이 영구히
  떠안을 "다른 커널을 흉내 내는 코드"가 된다. 정적 링킹만 지원하는
  이유는 동적 링커가 이 문제와 독립적으로 크고(ELF 로더 확장, PLT/GOT,
  `dlopen` 류) 이 계획의 핵심 목표(syscall 계층 자체를 증명)와
  직접 관련이 없기 때문이다.
- **영향**:
  - `libc/sysdeps/minicore/syscall_shim.c`(신규, M28)가 "POSIX
    syscall 번호 → `libmc` 호출" 번역의 단일 출처가 된다 — 이
    파일 자체는 최대한 얇게 유지되고(번호별 `switch`+`libmc` 호출
    한 줄), 실제 로직은 항상 `libmc` 쪽에 쌓인다.
  - `libmc`가 이 계획(M28~M39) 전체에 걸쳐 실질적으로 성장한다 —
    ADR-132(M12 착수 시점 기준)가 예상한 범위보다 훨씬 넓은 syscall
    집합(파일 I/O, 메모리 매핑, 스레드/futex, signal)까지 `libmc`
    함수로 채워진다. 이는 ADR-132의 원래 의도("어떤 libc를 포팅하든
    같은 `libmc`를 재사용")를 그대로 실현하는 것이다 — 나중에 다른
    libc(newlib 등)를 시도해도 이 `libmc` 확장을 그대로 재사용할 수
    있다.
  - `third_party/patches/musl/`가 이 저장소에서 처음으로 실제
    내용을 갖는다(M26은 무수정으로 충분했다) — repo-layout.md가
    이미 예약해 둔 자리다.
  - 실제 어떤 Linux syscall 번호를 언제 구현할지는 각 마일스톤
    (M28, M30~M32) 착수 시점에 필요한 만큼만 추가한다(YAGNI, 이전
    라운드들과 같은 패턴) — 이 ADR은 전략만 정하고 구체적인 번호
    목록은 정하지 않는다.
  - pthread/동적 링커/locale/완전한 signal 의미론은 이 ADR의
    범위 밖으로 남는다 — [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
    참고(단, 이 넷 모두 이후 이 계획에 편입됐다 — 동적 링커는
    ADR-189(kernel-memory.md)로 오히려 M29로 앞당겨졌고, locale은
    ADR-188(이 문서)의 M35, signal은 ADR-186(kernel-scheduler.md)의
    M36, pthread는 ADR-187(kernel-scheduler.md)의 M37이 각각 다룬다).

## ADR-188. musl locale: "C"/"POSIX" 고정, 실제 로케일 데이터 없음

- **상태**: 확정 (2026-09-10, 계획 단계 — [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
  M35 착수 전에 범위만 먼저 결정한다)
- **결정**: musl의 locale 서브시스템 중 **"C"/"POSIX" 로케일 하나만**
  실제로 동작하게 한다. `setlocale()`이 다른 이름을 요청하면 실패
  (`NULL`) 반환, `nl_langinfo`/`ctype`/`toupper` 등은 musl이 기본
  제공하는 C 로케일 테이블(문자열 함수처럼 이미 syscall-free)을
  그대로 쓴다. 실제 로케일 데이터 파일(`/usr/share/locale` 류)이나
  `LC_*` 환경변수 기반 전환, `iconv` 다국어 인코딩 변환은 범위 밖이다.
- **근거**: musl의 기본 동작이 이미 "C" 로케일을 syscall 없이
  제공하므로, 이 범위는 사실상 "아무것도 깨지지 않게 두는 것"에
  가깝다 — 실제 다국어 지원 수요가 생기기 전에는 이보다 넓힐 이유가
  없다(YAGNI, ADR-001의 "정확성 우선, 나중에 확장 가능한 구조만
  확보"와 일치).
  - **영향**: 거의 없음 — M35의 검증 프로그램에 `setlocale(LC_ALL, "")`
    (POSIX 관례상 항상 성공해야 하는 호출)를 넣어 깨지지 않는지
    확인하는 정도만 추가한다.

## ADR-210. M35 완성: musl setlocale() 왕복 — 계획 문서의 "미지원 로케일은 실패해야 한다"는 실제 musl 동작과 다름을 발견해 검증 목표를 조정

- **상태**: 확정 (2026-09-10, [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
  M35 실행 중 확정)
- **배경**: ADR-188의 결정("C"/"POSIX" 고정)을 실제로 구현·검증한다.
  계획 문서 §M35 본문은 "`setlocale(LC_ALL, "ko_KR.UTF-8")`가
  실패해야 한다(`NULL` 반환 확인)"고 명시했었다.
- **실행 중 발견**: `third_party/musl/src/locale/locale_map.c::
  __get_locale()`을 실제로 읽어 보면(musl 소스 무수정 원칙상 이
  동작을 바꿀 수 없다) 이는 musl의 실제 동작과 다르다 — musl은
  **알려지지 않은 로케일 이름을 실패시키지 않는다.** 내부 실패
  센티널(`LOC_MAP_FAILED`)은 malloc 실패나 이름에 `/`·선행 `.`이
  있을 때만 반환되고, 그 외의(진짜 로케일 아카이브가 없어도) 모든
  이름은 "요청한 이름을 기억하되 실제 데이터는 `C.UTF-8`로 조용히
  대체"해 **성공**으로 처리된다 — 이것이 바로 ADR-188이 원래 겨냥한
  "실제 로케일 데이터 없음"의 진짜 모습이다(실패가 아니라 조용한
  대체). 계획 문서 작성 시점의 "실패해야 한다"는 가정이 틀렸다.
- **결정**: 검증 목표를 실제 musl 동작에 맞게 조정한다 —
  1. `setlocale(LC_ALL, "")`가 성공함을 확인한다(계획 원문 그대로,
     변경 없음).
  2. `setlocale(LC_ALL, "ko_KR.UTF-8")`도 **성공**(`NULL`이 아님)함을
     확인한다 — 실패 여부가 아니라 "미지원 로케일이 실제로 아무
     효과가 없다"를 검증 대상으로 바꾼다.
  3. 위 호출 뒤에도 `toupper('a')=='A'`/`tolower('B')=='b'`처럼
     ctype 동작이 여전히 C 로케일 그대로임을 확인한다 — 이것이
     "실제 로케일 데이터 없음"을 직접 증명하는 조건이다.
  2번을 검증하려면 `setlocale`+`locale_map.c`(`__get_locale`)+
  `c_locale.c`+`__mo_lookup.c`+`getenv`+`toupper`/`tolower`(+`isupper`/
  `islower`, 이들의 링크 의존성)를 처음으로 컴파일에 포함해야 했다.
  `locale_map.c`가 무조건 참조하는 `__map_file`(MUSL_LOCPATH 환경
  변수 탐색 — 이 프로젝트의 envp는 항상 비어 있어 실제로는 절대
  실행되지 않는 경로)은 원본(open+fstat+mmap) 대신
  `sysdeps/minicore/locale_shim.c`의 항상-실패 대체로 바꿨다 —
  `lock_shim.c`/`malloc_shim.c`와 같은 이유(실행되지 않을 코드를
  위해 `fstat`/`fstatat`/`statx`류 새 의존성을 끌어올 이유가 없다).
- **영향**: 계획 문서(`real-libc-syscall-layer.md` §M35) 자신은
  수정하지 않는다(그 문서는 "실행 전 계획"만 남기는 자리다) — 이
  ADR과 `docs/done/real-libc-syscall-layer-m35.md`가 실제로 무엇을
  검증했는지의 정본이다.

## ADR-198. 네임스페이스 컨벤션: `kern::*` 계층(커널)·`kernsrv::<서버명>`(서버)·`__internals__` 은닉 마커·`proto` 예외 (ADR-042 보강)

- **상태**: 확정 (2026-09-10, 사용자 지시. **규칙만 확정** — 기존
  코드에 실제로 적용(리네임)하는 작업은 이 ADR의 범위 밖이며 별도
  실행 여부를 사용자에게 확인한다, 아래 "영향" 참고)
- **결정**:
  1. **커널(및 커널과 함께 컴파일되는 코드)은 `kern` 최상위
     네임스페이스 아래, 모듈별로 세분화된 하위 네임스페이스에
     둔다.** 예:
     ```
     kern            // 최상위
     kern::arch      // arch 공통(HAL 경계, ADR-002)
     kern::arch::x86_64
     kern::ipc
     kern::mm
     kern::proc
     ```
     정확히 이 넷만이 아니라, 커널 코어의 각 서브시스템(`kernel/core/*`
     디렉터리 하나당 하나씩이 기본 원칙)마다 하나의 `kern::<모듈>`을
     둔다.
  2. **외부(다른 `kern::*` 하위 모듈, 혹은 커널 밖)에 노출되길
     원하지 않는 네임스페이스는 `__internals__`로 명명해 그 모듈
     밑에 둔다** — 예: `kern::ipc::__internals__`. 이는 **익명
     네임스페이스(`namespace {}`)를 대체하지 않는다** — 둘은 은닉
     범위가 다르다:
     - 익명 네임스페이스: 그 `.cpp` 파일 **하나**에서만 보인다(외부
       링크 자체가 없음) — 지금처럼 그대로 유지한다.
     - `__internals__`: 한 모듈의 **여러** `.cpp`/헤더가 서로 호출해야
       하지만, 그 모듈 밖(다른 `kern::*` 서브모듈, 서버, 외부
       소비자)에는 "이건 계약이 아니라 구현 세부"라고 신호하고 싶을
       때 쓴다.
  3. **커널 서버(`servers/*`, ADR-006/007)는 `kernsrv::<서버명>`
     네임스페이스에 둔다** — 예:
     ```
     kernsrv::devmgr
     kernsrv::procsrv
     ```
     각 서버는 자기 실행파일 하나이므로 `kern`처럼 깊게 세분화할
     필요는 없다 — 서버 내부에서 더 나누고 싶으면 그 서버 재량으로
     `kernsrv::<서버명>::<하위모듈>`을 추가할 수 있다(강제하지 않음).
  4. **예외 — 프로토콜(와이어 포맷) 정의는 별도 네임스페이스**를
     쓴다. IPC 데이터 포맷, TCP/UDP/IP 같은 네트워크 프로토콜 등
     "여러 소비자가 합의해야 하는 레이아웃"을 담는 코드는 그 코드가
     속한 모듈의 네임스페이스가 아니라:
     - 커널이 정의하는 프로토콜 → `kern::proto`
     - 서버가 정의하는 프로토콜 → `kernsrv::proto`
     여기 둔다 — 어느 특정 서버/모듈 소유가 아니라 "클라이언트와
     서버가 함께 참조하는 계약"이라는 성격을 이름으로도 드러낸다.
  5. **이 규칙의 적용 범위가 아닌 것**:
     - **`libk/`** — 커널과 서버 양쪽이 공유하는 크로스컷 유틸리티
       라이브러리라 "커널"도 "커널서버"도 아니다. 이 ADR은 `libk`를
       건드리지 않는다 — 기존 `libk_detail`(사실상 이미
       `__internals__`와 같은 역할을 해 온 이름)을 그대로 두거나,
       일관성을 위해 `libk::__internals__`로 다시 쓸지는 별도 결정
       대상으로 남긴다.
     - **`libmc/`**(ADR-132) — 순수 C+어셈블러라 네임스페이스 자체가
       문법적으로 없다. `mc_` 접두사 컨벤션을 그대로 유지한다.
     - **`libc/`/`userland/`**(ADR-005/022, 포팅 코드) — 원본 프로젝트
       컨벤션을 따르며 이 규칙의 적용 대상이 아니다(ADR-042가 이미
       정한 예외와 동일).
- **근거**: 사용자가 명시적으로 이 계층 구조를 지정했다. 기존
  네임스페이스(`object`/`ipc`/`mm`/`sched`/`arch_x86_64`/`uapi` 등)는
  전부 최상위 평면에 나란히 떠 있어서, 이름만 보고는 "이게 커널
  코드인지, 서버 코드인지, 프로토콜 정의인지"를 구분할 수 없었다 —
  특히 `uapi`(커널·유저 공유 syscall ABI 헤더)처럼 실질적으로
  "프로토콜"인 것이 다른 커널 코어 모듈과 구분 없이 같은 층위에
  있었다. `kern::`/`kernsrv::` 프리픽스와 `proto` 예외는 이런 혼선을
  이름 자체로 없앤다.
- **기존 코드 매핑(제안, 실제 적용 전 확인용)** — 현재
  `kernel/`·`libk/`·`servers/`에 실제로 존재하는 네임스페이스를
  조사해 만든 표다:

  | 현재 | 새 이름(제안) | 비고 |
  |---|---|---|
  | `object`(`kernel/core/object/`, `kernel/arch/x86_64/{fpu,process_ops,tss}.hpp`) | `kern::object` | |
  | `ipc`(`kernel/core/ipc/`) | `kern::ipc` | |
  | `mm`(`kernel/core/mm/`) | `kern::mm` | |
  | `sched`(`kernel/core/sched/`) | `kern::sched` | |
  | `arch_x86_64`(`kernel/arch/x86_64/`) | `kern::arch::x86_64` | |
  | `boot`(`kernel/core/boot_info_dump.cpp`, `kernel/include/boot_info.hpp`) | **변경 없음(`boot`로 유지)** | M44 실행 중 발견 — `kernel/include/boot_info.hpp`는 `kernel/include/`(유저랜드와 공유하는 ABI 헤더 자리)에 있고 실제로 `init/initrun/main.cpp`가 직접 include해 쓴다. `uapi.hpp`와 같은 "커널·유저 공유 ABI"라 `kern::`(커널 전용) 대상이 아니다 — ADR-200이 `uapi.hpp`에 적용한 것과 같은 처리(궁극적으로 `mc`로 흡수)가 맞는 방향이나, 이 ADR/M44는 범위를 넓히지 않고 이름을 그대로 둔다. 후속 결정 대상 |
  | `initrd`(`kernel/core/initrd/mcpack.*`) | `kern::initrd`(최상위) | M44 실행 시점에 확정 — `kern::boot`를 새로 만들지 않기로 했으므로(위 `boot` 행) 그 하위에 둘 이유도 없다. `initrd` 사용은 순수 커널 내부(`kernel_main.cpp`, `mcpack.*`)로 확인됨 |
  | `klog`(`kernel/core/klog.cpp`, `kernel/arch/x86_64/klog_uart.cpp`) | `kern::klog` | |
  | `uapi`(`kernel/include/uapi.hpp`) | **`kern::proto`** | 이 예외 규칙이 정확히 겨냥하는 대상 — syscall ABI/메시지 레이아웃, 유저랜드와 공유 |
  | `kernel/arch/x86_64/process_ops.*`의 fork/exec/spawn/kill 의미론 | **`kern::arch::x86_64`에 그대로 유지(분리 안 함)** | M46 실행 시점에 확정 — 레지스터 수준 컨텍스트 조작과 fork/exec 개념이 한 파일에 강하게 얽혀 있어 분리 비용이 순수 리네임의 범위를 넘는다고 판단했다([done](../done/namespace-refactor-m46.md)). `kern::proc`은 만들지 않는다 — 실제로 arch 독립 인터페이스 분리 설계가 나오는 시점에 재검토 |
  | `libk_detail`(`libk/include/libk/*.hpp`) | 이 ADR 범위 밖(위 §결정5) — 제안: `libk::__internals__` | |
  | `servers/*`의 각 서버(현재 전부 무네임스페이스, 파일 내부 익명 네임스페이스만 존재) | `kernsrv::<서버명>` — `procsrv`/`vfs`/`devmgr`/`cfgsrv`/`login`/`netsrv`/`fs::memfs`/`fs::fat32`/`fs::ext4`/`drivers::ps2`/`drivers::usb`/`drivers::console`/`drivers::virtio_blk`/`drivers::virtio_net`/`svcmgr`(계획 단계, [user-service-manager.md](../plan/user-service-manager.md)) | 서버 내부 세분화(`kernsrv::fs::memfs`류)는 이 표에서 제안하는 것일 뿐 강제 아님(§결정3) |
  | (아직 없음) 서버 내부의 순수 C++ 프로토콜/와이어 포맷 코드 — 예: netsrv의 이더넷/IP/UDP 헤더 구조체(M25) | `kernsrv::proto` | `libmc`가 정본인 프로토콜(procsrv/fs 등, ADR-132)은 C 헤더라 이 규칙 대상이 아니다 — `kernsrv::proto`는 libmc로 노출되지 않는, 서버 내부 전용 C++ 프로토콜 포맷 코드 자리다 |
- **영향**:
  - **이 ADR은 규칙만 확정한다 — 기존 코드에 실제로 적용(전면
    리네임)하는 작업은 아직 실행하지 않았다.** 위 매핑표에 표시한
    두 판단 지점(`initrd`의 정확한 자리, `kern::proc`과
    `kern::arch::x86_64`의 경계)은 실제 적용 시점에 확정한다.
  - 실제 적용은 별도 계획 [namespace-refactor.md](../plan/namespace-refactor.md)
    (M44~M48, 사용자가 "별도 계획으로 분리해서 지금 세우기"를
    선택)로 분리했다 — 순수 기계적 리네임이라 이 프로젝트가 지금까지
    지켜온 "바꾼 뒤 QEMU 5개 스위트로 회귀 확인" 원칙을 마일스톤마다
    그대로 적용한다.
  - `docs/spec/cxx-conventions.md`§6을 이미 목표 상태로 갱신해
    뒀다(신규 코드는 지금부터 이 컨벤션을 따른다) — 기존 코드
    리네임 완료 여부는 그 문서의 "아직 실행되지 않았다" 문구로
    추적한다.
- **M44 실행 결과(2026-09-10, [done](../done/namespace-refactor-m44.md))**:
  `object`/`ipc`/`mm`/`sched`/`klog`/`initrd` 6개 네임스페이스를
  실제로 `kern::*`로 리네임했다(총 48개 파일). 이 과정에서 위
  `boot` 행의 발견(커널·유저 공유 ABI라 `kern::` 대상이 아님)을
  했고, `initrd`는 `kern::initrd`(최상위)로 확정했다. 빌드 성공+
  smoke/SMP/NUMA/AVX/net 5개 스위트 전부 회귀 없음(SMP 11, NUMA 24,
  AVX 12, net 6개 확인 문자열 전부 통과) 확인.

## ADR-200. `mc`(구 `libmc`)를 커널·유저 공용으로 통합 — 전처리기 매크로로 kernel-land/user-land 구분, `uapi.hpp`의 손 복제를 폐지 (ADR-132 보강)

- **상태**: 확정 (2026-09-10, 사용자 지시. 규칙만 확정 — 실제 구현은
  [libs-restructure.md](../plan/libs-restructure.md) M50이 다룬다)
- **결정**:
  1. **`mc`(ADR-199로 `libmc`에서 개명)의 헤더가 커널과 유저랜드
     양쪽이 공유하는 단일 출처가 된다.** 지금 `kernel/include/uapi.hpp`
     가 `kernel/core/ipc/message.hpp`의 `ipc::message`를 "필드
     순서·타입이 완전히 동일해야 한다"는 주석만 믿고 손으로 복제해
     유지하는 상태(그 파일 자신의 상단 주석이 이미 이 위험을
     스스로 지적하고 있었다)를 폐지한다 — `uapi.hpp`는 사라지고,
     그 내용(message 레이아웃, syscall 번호, 각종 요청/응답 구조체)
     은 `mc`의 헤더로 옮겨진다.
  2. **커널-랜드/유저-랜드 구분은 소비자가 선언하는 전처리기
     매크로로 한다** — `mc`의 헤더를 include하기 전에 소비자가
     자신이 "커널-랜드"임을 밝히는 매크로(가칭 `MC_LAND_KERNEL`,
     정확한 이름은 착수 시점에 확정)를 정의하면 헤더 내용이
     조건부로 달라진다:
     - **커널-랜드**(`kernel/`이 정의): 구조체·enum·상수(메시지
       레이아웃, syscall 번호, 프로토콜 label 값)만 노출한다.
       syscall 트램폴린(실제 `syscall` 명령을 실행하는 함수)이나
       IPC/서버 프로토콜 **클라이언트** 함수는 노출하지 않는다 —
       커널은 자기 자신을 syscall로 호출할 이유가 없다.
     - **유저-랜드**(매크로 미정의가 기본값 — 지금까지의 동작과
       동일): 구조체+syscall 트램폴린+프로토콜 클라이언트 함수
       전부 노출한다.
  3. **`mc`의 헤더는 C와 C++ 양쪽에서 유효해야 한다** — 커널은
     C++(ADR-010)이고 유저랜드 소비자(포팅된 libc 포함, ADR-183)는
     C다. ADR-132 §결정2가 이미 확립한 패턴(POD 구조체+`extern "C"`)
     을 그대로 유지하면 이 요구사항은 이미 충족된다 — 새 기법이
     필요하지 않다.
  4. **`mc`의 구현(.c) 파일은 여전히 유저-랜드 전용이다** — 이
     ADR은 헤더(선언)만 커널-랜드에 공유한다. syscall 트램폴린/
     프로토콜 클라이언트의 실제 구현은 커널이 링크하지 않는다
     (커널은 그 함수들을 애초에 호출하지 않으므로 필요 없다).
- **근거**: `uapi.hpp`는 ADR-132가 "프로토콜 와이어 포맷의 단일
  출처는 `libmc`(→`mc`)의 C 헤더로 삼는다"고 이미 정한 원칙에서
  유일하게 벗어나 있던 예외였다 — 커널 ABI만은 그 원칙을 따르지
  못하고 "커널-internal 헤더를 유저 실행파일에 직접 include하는
  대신 복제한다"는 실용적이지만 위험한 타협을 했었다(동기화가
  깨지면 컴파일 에러 없이 조용히 잘못된 메모리를 읽는 사고로
  이어진다 — ADR-195가 프로토콜 일반에 대해 지적한 것과 정확히
  같은 위험). 전처리기 매크로로 "누가 소비하는가"만 가르면 복제
  없이 하나의 정본으로 양쪽을 만족시킬 수 있다.
- **영향**:
  - `kernel/include/uapi.hpp`는 폐지된다 — `kernel/`이 이제
    (헤더만) `mc`를 참조한다. 지금까지 `kernel/`은 `libmc`를 전혀
    참조하지 않았다(유저 전용이었다)는 점에서 저장소 의존관계
    그래프의 실질적 변화다.
  - `kernel/arch/x86_64/{syscall.cpp,process_ops.*,kernel_main.cpp}`
    가 `uapi::`(ADR-198/[namespace-refactor.md](../plan/namespace-refactor.md)
    M45가 계획했던 `kern::proto::`)로 참조하던 것을 `mc::`(커널-랜드
    매크로 하에 노출된 것)로 바꿔야 한다 — **이 작업은
    namespace-refactor.md M45의 범위와 겹친다.** M45("uapi를
    `kern::proto`로 리네임")는 이 ADR로 **대체된다** — 단순 리네임이
    아니라 폐지+`mc` 흡수로 목표가 바뀌었다. namespace-refactor.md
    자체는 수정하지 않고(문서 체계 원칙), [libs-restructure.md](../plan/libs-restructure.md)
    M50이 이 대체를 실행한다.
  - 정확한 매크로 이름과, 커널-랜드에서 정확히 어떤 대상까지
    숨길지(예: 프로토콜 클라이언트 함수의 시그니처 선언만 숨길지,
    아예 그 헤더 파일 전체를 안 보이게 할지)의 세부는 실행 착수
    시점에 확정한다.

## ADR-220. 익명 파이프(M51): 새 커널 프리미티브 대신 전용 서버+재시도 폴링으로 구현 — ADR-183과 같은 전략의 pipe/dup2 적용

- **상태**: 확정 (2026-09-10~11, [musl-userland-porting.md](../plan/musl-userland-porting.md)
  §M51 실행).
- **결정**:
  1. `pipe()`/`pipe2()`/`dup2()`를 커널에 새 오브젝트 종류나 새
     syscall로 추가하지 않는다 — ADR-183 §결정2가 이미 확립한 전략
     ("커널을 확장하지 않고 musl의 syscall 진입점을 패치해
     유저랜드로 우회")을 그대로 이어, 새 전용 서버
     `servers/pipesrv`(libk+libmc만, procsrv/cfgsrv/svcmgr와 같은
     순수 minicore 네이티브 서버)를 만들고 `libc/sysdeps/minicore/
     syscall_shim.c`가 그 클라이언트(`mc/pipesrv_client.h`)를 부르는
     것으로 대체한다.
  2. **pipesrv는 절대 회신을 미루지 않는다** — procsrv/cfgsrv와
     완전히 같은 "단일 요청-응답 루프" 모양이다(OPEN-67이 이미
     지적한 것과 같은 근본 이유: 이 커널의 `sys_reply`는 스레드당
     한 슬롯(ADR-216이 재진입 한 단계까지는 감당하게 했지만, 여러
     독립된 대기자를 임의 순서로 깨우는 일반적인 경우는 여전히
     지원하지 않는다)이라, 서버가 "지금 회신 못 함, 나중에 조건이
     맞으면 회신"을 하려면 멀티스레드+임의 순서 재개라는 훨씬 큰
     기계장치가 필요하다). 대신 파이프가 비었으면(read) 또는
     가득 찼으면(write) 즉시 `MC_PIPE_STATUS_WOULD_BLOCK`을 돌려주고,
     **호출자**(syscall_shim.c)가 `mc_yield()`+재시도로 블로킹을
     흉내낸다 — `mc_wait()`(procsrv 클라이언트, OPEN-67)가 이미 쓰는
     것과 정확히 같은 패턴을 파이프에도 그대로 적용한 것뿐이다.
  3. **id는 프로토콜-레벨 정수이지 커널 핸들이 아니다**
     (registry-decisions.md ADR-169 §결정4와 같은 정신) — 읽기 쪽/
     쓰기 쪽 각각 참조 카운트(`read_refcount`/`write_refcount`)를
     둔다. `fork()`/`dup2()`로 같은 id를 여러 프로세스(또는 한
     프로세스의 fd 슬롯 여러 개)가 들고 있을 수 있는데, pipesrv는
     이런 복제를 스스로 관찰할 방법이 전혀 없다(`fork()`는 이
     서버가 전혀 모르는 사이에 호출자의 주소공간을 통째로 COW
     복제할 뿐이다) — 그래서 호출자가 그 시점마다 명시적으로
     `op_dup`을 불러 참조 카운트를 알려줘야 한다는 계약으로
     풀었다. 참조 카운트가 0이 되는 쪽이 "그 끝이 진짜로 닫혔다"는
     뜻이다 — 쓰기 쪽이 0이면 read는 WOULD_BLOCK 대신 진짜 EOF
     (status=OK, len=0)를, 읽기 쪽이 0이면 write는
     `MC_PIPE_STATUS_BROKEN_PIPE`를 돌려준다.
  4. 파이프당 고정 4096바이트 원형 버퍼 하나, 동시에 열 수 있는
     파이프 최대 8개(YAGNI — 계획 문서가 이미 이렇게 단순화하기로
     정했다).
- **근거**: 이 프로젝트는 이미 "새 기능은 커널을 확장하지 말고
  기존 IPC 프리미티브 위의 유저랜드 서버로 푼다"는 원칙을 반복해서
  선택해 왔다(ADR-183의 syscall 우회, ADR-196 §결정7의 "새 종료/
  저장 메커니즘을 만들지 않는다" 등). 파이프도 본질적으로 "바이트를
  버퍼링하고 읽기/쓰기 커서를 관리하는" IPC 그 자체이므로, 이미
  있는 Call/Reply 왕복과 이미 검증된 "폴링+양보" 요령(procsrv의
  OP_WAIT)을 그대로 재사용하는 것이 새 블로킹 커널 프리미티브를
  발명하는 것보다 훨씬 작고 안전한 변경이다.
- **영향**: `mc/pipesrv_protocol.h`(신규, ADR-195 마크업)+
  `mc/pipesrv_client.h`/`.c`(신규)+`servers/pipesrv`(신규)+
  `libc/sysdeps/minicore/syscall_shim.c`(`SYS_pipe`/`SYS_pipe2`/
  `SYS_dup2`/`SYS_read`/`SYS_write`/`SYS_close`/`SYS_fork` 확장).
  실행 중 발견: x86_64가 실제로는 레거시 `SYS_pipe`(22)도 갖고
  있어서(i386 전용이 아니었다) musl의 `pipe.c`가 `SYS_pipe2`(293)
  대신 `SYS_pipe`로 온다는 것을 처음엔 놓쳤다 — 둘 다 같은 핸들러로
  처리하도록 고쳤다. 이 파이프 fd 표(`g_pipe_fds[]`)는 fd 0/1/2도
  `dup2()`로 덮어씌울 수 있어야 해서 기존 VFS 파일 표(`g_open_files[]`,
  "fd-3" 오프셋 관례)와 달리 fd 번호로 직접 인덱싱한다 — 새 fd를
  할당할 때(open()/pipe2()) 두 표 모두와 충돌하지 않는지 확인하는
  `fd_is_free()`를 새로 두었다.

## ADR-221. M52 방향 전환: BusyBox(서드파티) 도입 철회, 셸/coreutils를 이 저장소에서 직접 작성 — 정적 musl 링크 자체는 그대로 유지

- **상태**: 확정 (2026-09-11), [musl-userland-porting.md](../plan/musl-userland-porting.md)
  §M52 착수 중 사용자 결정.
- **배경**: M52는 원래 BusyBox(sh+coreutils 단일 정적 바이너리)를
  `third_party/`에 git submodule로 들여 이 저장소의 musl에 정적으로
  링크하는 것이었다. 실제로 vendoring까지는 문제없이 됐지만
  (`third_party/busybox`, release `1_36_1`), 빌드 연결 착수 중
  **두 겹의 서로 다른 환경 문제**를 만났다:
  1. BusyBox의 `Makefile`(`scripts/trylink`)은 `$(CC)` 하나가
     컴파일과 링크를 전부 처리하는 정상적인 hosted gcc/clang이라고
     전제한다 — 이 저장소는 정확히 반대다(ADR-020/M38: clang은
     컴파일에만 쓰고 최종 링크는 `ld.lld`를 커스텀 link.ld와 함께
     직접 부른다, 이 Windows 호스트에서 clang/gcc를 링커로 쓰면
     `-fuse-ld=lld`가 조용히 무시되고 MSYS2 gcc의 collect2로 새는
     것을 직접 확인했다). 이건 컴파일/링크 모드를 구분해 링크를
     `ld.lld` 호출로 바꿔치는 `CC` 셈 스크립트로 실제로 풀렸다
     (`tools/busybox-cc-shim.sh`, 이번에 되돌리며 함께 삭제).
  2. 그 셈이 통과한 뒤 맞닥뜨린 **완전히 다른 층위의 문제**: BusyBox
     의 `.config`조차 만들려면 Kconfig의 호스트 도구(`fixdep` 등,
     크로스 타깃과 무관하게 **이 빌드 호스트 자신**을 위해 컴파일돼야
     하는 코드)가 필요한데, 이 MSYS2 설치의 호스트 `gcc`
     (`x86_64-pc-msys` 타깃)에 표준 헤더 자체가 없었다(`/usr/include`
     가 존재하지 않음 — `msys2-runtime-*-devel` 패키지가 설치돼 있지
     않은 이 저장소 밖 로컬 환경 문제). 패키지 설치로 고칠 수 있는
     문제였지만, 사용자가 이 시점에서 BusyBox 도입 자체를 재고하기로
     결정했다.
- **결정**:
  1. **BusyBox(그리고 일반적으로 Kbuild/Makefile 기반의 "정상적인
     hosted 컴파일러" 전제가 깊게 박힌 외부 프로젝트)를 이 계획의
     대상에서 뺀다.** `third_party/busybox` submodule과
     `.gitmodules`의 항목, `tools/busybox-cc-shim.sh`를 전부
     되돌린다.
  2. **셸과 coreutils(`ls`/`cat`/`echo`)는 이 저장소 안에서 직접
     작성한다** — `userland/musl-hello`/`userland/pipe-test`가 이미
     증명한 패턴 그대로(순수 CMake+clang 컴파일+`ld.lld` 직접
     링크, 이 저장소의 다른 모든 유저 프로그램과 완전히 같은 빌드
     경로) 새 실행파일들을 추가한다. **정적으로 이 저장소의 musl에
     링크한다는 원래 결정(ADR-203)은 그대로 유지**된다 — 바뀐 건
     "어디서 소스를 가져오는가"뿐, "어떻게 빌드/링크하는가"는
     전혀 바뀌지 않았다.
  3. 셸은 M51이 만든 진짜 syscall(fork/execve/waitpid/pipe/dup2)
     을 실제로 행사해야 한다 — ADR-170(M20)의 minicore 네이티브
     대체 셸(빌트인 명령 디스패치, fork/exec 없음)과 혼동하지
     않는다. 이 새 셸은 명령을 **별도 실행파일로 fork+exec**해야
     M39/M52~M54가 원래 세운 검증 목표("ls/cat 같은 명령이 빌트인이
     아니라 별도 실행파일")가 그대로 성립한다 — "서드파티 소스를
     그대로 가져왔는가"만 빠졌을 뿐, "진짜 프로세스 분리로 명령을
     실행하는가"라는 원래 목표의 핵심은 유지된다.
- **근거**: BusyBox 도입에서 실제로 부딫힌 두 문제 모두 이
  프로젝트의 아키텍처(정적 musl 링크, 커스텀 link.ld+`ld.lld`
  직접 링크)와 무관한 **외부 빌드 시스템 통합 비용**이었다 —
  Kbuild가 스스로 확인하려는 수십 가지 "정상적인 Linux 호스트"
  가정(호스트 도구 컴파일, `trylink`의 다단계 프로브) 중 이
  프로젝트에 실제로 의미 있는 건 거의 없다. 반면 이 저장소 자신의
  CMake+clang+`ld.lld` 경로는 M27~M51 내내 단 한 번도 이런 종류의
  마찰 없이 15개 이상의 실행파일을 만들어 왔다 — 같은 경로를 셸/
  coreutils에도 그대로 쓰는 것이 훨씬 적은 위험으로 M52~M54의
  실제 목표(진짜 fork/exec+파이프/리다이렉션으로 명령을 실행하는
  로그인 셸)를 달성한다.
- **영향**:
  - `docs/plan/musl-userland-porting.md` 문서 자체는 수정하지
    않는다(문서 체계 원칙 — M29/ADR-203이 이미 같은 방식으로
    처리한 선례). M52의 실제 실행 결과(이 방향 전환 포함)는
    `docs/done/musl-userland-porting-m52.md`에 정직하게 기록한다.
  - **OPEN-74**(BusyBox의 Makefile↔이 저장소의 링크 방식 불일치)는
    이 ADR로 해소된다 — 그 문제 자체가 더 이상 적용되지 않는
    대상(BusyBox)에 대한 것이었기 때문이다.
  - 새 유저 프로그램들의 정확한 이름·목록(예: `userland/mush`
    (minicore shell)+`userland/coreutils-*` 또는 단일 멀티콜
    바이너리 등)은 M52 실행 착수 시점에 확정한다.
