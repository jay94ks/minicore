# 설계 결정: 빌드 시스템·저장소 구조

CMake·툴체인·저장소 레이아웃·서드파티 소스 관리에 관한 결정. [repo-layout.md](repo-layout.md)의 근거가 되는 결정들이다.

[← 설계 문서 색인](index.md)

---

## ADR-019. 빌드 시스템: CMake

- **상태**: 확정 (2026-09-08)
- **결정**: 빌드 시스템으로 CMake를 사용한다. 아키텍처별 크로스 컴파일은
  CMake 툴체인 파일(`toolchain/<arch>.cmake`)로 분리한다.
- **근거**: 다중 아키텍처(ADR-002/ADR-009) 크로스컴파일 설정을 툴체인 파일로
  깔끔히 분리할 수 있고, IDE·CI 생태계 지원이 가장 넓다.
- **영향**:
  - 아키텍처 추가 시 툴체인 파일만 추가하면 되도록 최상위 빌드 스크립트를 설계한다.
  - 서브프로젝트(커널/서버/libc/유저랜드) 각각이 독립적으로 구성 가능해야 한다 (ADR-021).

## ADR-020. 컴파일러 툴체인: Clang 기본, GCC 폴백

- **상태**: 확정 (2026-09-08)
- **결정**: 1차 툴체인은 Clang/LLVM(`--target=<triple>`)이며, GCC 크로스 툴체인
  (`<arch>-elf-gcc`)도 폴백으로 지원한다. CI는 최소 Clang 경로를 검증한다.
- **근거**: Clang은 단일 바이너리로 여러 타겟을 다뤄 아키텍처 전환이 가장 가볍고,
  `clang-tidy`/`lld` 등 LLVM 도구체인과 자연스럽게 통합된다. GCC 폴백은 특정 환경
  (배포판 패키지 매니저로만 툴체인을 구하는 경우 등)의 진입 장벽을 낮춘다.
- **영향**:
  - CMake 툴체인 파일이 컴파일러별 분기(Clang: `--target`, GCC: 트리플 접두 바이너리)를
    감춰야 한다.
  - GCC 경로는 CI에서 빌드까지만 검증하고, 부팅 검증은 Clang 산출물 기준으로 한다
    (초기 범위 — 필요 시 확장).

## ADR-021. 저장소 구조: monorepo + 서브프로젝트(meta-build)

- **상태**: 확정 (2026-09-08)
- **결정**: 하나의 저장소 안에 커널, 각 시스템 서버, libc/POSIX 계층, 유저랜드
  포팅물을 **독립된 빌드 단위**(각자 자체 `CMakeLists.txt`)로 두고, 최상위
  CMake가 이들을 아키텍처별로 오케스트레이션한다.
- **근거**: ADR-006(직접구현)과 ADR-005/ADR-008(포팅 + 다중 서버) 조합상 구성 요소
  수가 많아질 것이 확실하다. 독립 빌드 단위로 나누면 한 서버의 빌드 실패가 다른
  서버 개발을 막지 않고, 서드파티 포팅물(ADR-022)을 별도 빌드 규칙으로 격리하기 쉽다.
- **영향**:
  - 최상위에서 "이 아키텍처엔 이 서버 목록을 빌드한다" 같은 조합 로직이 필요하다.
  - 구체적 디렉토리 트리는 `docs/design/repo-layout.md`에서 상세화한다.

## ADR-022. 서드파티/포팅 소스 관리: Git submodule + 패치 디렉토리

- **상태**: 확정 (2026-09-08)
- **결정**: 포팅 대상 소스(libc, coreutils류, 셸 등, ADR-005)는 원본을 **git
  submodule**로 참조하고, minicore 이식에 필요한 변경은 **별도 패치 디렉토리**
  (`third_party/patches/<프로젝트>/*.patch`)에 보관한다. 빌드 시 submodule 체크아웃
  위에 패치를 순서대로 적용한 뒤 컴파일한다.
- **근거**: 원본과의 diff가 항상 명확하게 유지되어 업스트림 갱신 시 병합이 쉽고,
  라이선스 준수(원본 이력 보존) 측면에서도 유리하다. vendor 복사본 방식보다
  저장소 용량 부담도 적다.
- **영향**:
  - `.gitmodules`와 패치 적용 스크립트(`tools/apply-patches.*`)가 필요하다.
  - CI는 submodule 체크아웃 → 패치 적용 → 빌드 순서를 파이프라인에 명시해야 한다.
  - 패치 충돌 시 대응 절차(재베이스 vs 패치 재작성)는 실제로 포팅을 시작하는
    시점에 별도 문서화한다.

## ADR-031. 호스트 크로스 툴체인 관리: 저장소 외부에서 별도 빌드, 저장소는 참조만

- **상태**: 확정 (2026-09-08)
- **결정**: Clang/LLVM 또는 GCC 크로스 툴체인(호스트에서 실행되어 minicore를
  컴파일하는 도구 자체)은 저장소 내부에 서브디렉토리(예: `third_party/gcc`,
  `third_party/llvm`)로 관리하지 않는다. 툴체인은 **저장소 밖의 별도 위치에서
  독립적으로 빌드/설치**하고, 저장소의 CMake 툴체인 파일(`toolchain/*.cmake`)은
  PATH 상의 실행파일 이름 또는 사용자가 지정한 prefix로 그 결과물만 참조한다.
- **근거**: ADR-022의 git submodule + 패치 정책은 "minicore가 이식해서 **실행**
  하는 대상 소프트웨어"(libc, coreutils, 셸)를 위한 것이다. 호스트 툴체인은
  성격이 다르다 — minicore의 산출물이 아니라 minicore를 **만드는 도구**이며,
  LLVM/GCC 전체 소스는 크기와 빌드 시간이 매우 커서 저장소에 얽매이면 클론·CI
  때마다 불필요한 부담이 된다. 분리해두면 툴체인을 캐싱하거나 패키지 매니저로
  설치하는 등 조달 방식을 자유롭게 선택할 수 있다.
- **영향**:
  - `toolchain/*.cmake`(ADR-020)는 컴파일러를 저장소 내부 경로로 하드코딩하지
    않고 PATH 탐색(`clang`, `x86_64-elf-gcc` 등 이름 기반) 또는 사용자가 넘기는
    캐시 변수로 해석되어야 한다 — 이미 작성된 4개 툴체인 파일이 이 정책과 일치함.
  - 향후 "툴체인 준비" 편의 스크립트를 `tools/`에 추가하더라도, 그 스크립트는
    저장소 밖(예: `$HOME/.minicore/toolchain`)에 설치하고 저장소 안에는 아무
    산출물도 커밋하지 않는다.
  - 저장소 스캐폴딩([repo-layout.md](repo-layout.md), [scaffold-repo-skeleton.md](../done/scaffold-repo-skeleton.md))에는
    변경이 필요 없다 — 애초에 툴체인 소스를 담은 디렉토리를 만든 적이 없다.

## ADR-091. QEMU 기능 공백 대응: 필요하면 자체 fork로 보강, ADR-031과 동일하게 저장소 밖에서 관리

- **상태**: 확정 (2026-09-08)
- **결정**:
  1. minicore 테스트/검증(kernel-bootstrap.md의 QEMU 부팅 검증,
     `tools/run-qemu.sh`)에 필요한 에뮬레이션 기능이 upstream QEMU에
     없거나(미구현), 구현될 계획이 없으면(로드맵에 없음), **"기본은
     stock upstream QEMU를 쓴다"는 원칙에 대한 예외**로 QEMU를 fork해
     필요한 구현을 직접 보강한다.
  2. 기본값은 항상 **stock upstream QEMU**다 — fork는 특정
     마일스톤/테스트를 실제로 막는 구체적 기능 공백이 확인되었을
     때만 만든다. "나중에 필요할 수도 있어서" 미리 fork해두지 않는다.
  3. fork는 **ADR-031의 크로스 툴체인과 동일한 원칙**을 따른다 —
     minicore 저장소 안에 QEMU 소스를 vendoring(서브모듈 포함)하지
     않는다. upstream QEMU를 fork한 **완전히 별도의 저장소**로
     관리하며, 그 저장소를 별도로 clone/빌드해 저장소 밖(예:
     `$HOME/.minicore/qemu-fork`)에 설치한다. `tools/run-qemu.sh`는
     PATH 또는 설정 가능한 경로 변수로 그 결과물
     (`qemu-system-x86_64`/`qemu-system-aarch64`)만 참조한다.
  4. fork 저장소는 upstream QEMU를 주기적으로 추적(rebase/merge)하며,
     minicore의 추가 구현은 식별 가능한 커밋/브랜치로 분리 유지한다 —
     upstream이 나중에 같은 기능을 자체 구현하면 그 델타를 쉽게
     제거할 수 있게 하기 위함이다.
- **근거**: ADR-031이 이미 "minicore를 만드는/검증하는 도구"는
  minicore의 산출물이 아니므로 저장소 밖에서 관리한다고 정했다 —
  QEMU도 정확히 같은 범주다: minicore가 배포하거나 대상 하드웨어에서
  실행하는 것이 아니라 개발 중 검증에만 쓰는 도구다. QEMU 소스 트리도
  LLVM/GCC와 마찬가지로 커서, 저장소에 얽매이면 클론·CI 비용이
  커진다는 점도 동일하다. ADR-022(포팅 소프트웨어의 서브모듈+패치
  방식)를 쓰지 않는 이유는, 그 방식은 "minicore가 이식해서 **실행**
  하는 대상 소프트웨어"를 위한 것이고 QEMU는 그 범주가 아니기
  때문이다 — 또한 fork를 우리가 직접 소유하므로, 별도 패치 파일을
  관리하는 것보다 fork 자체의 커밋 이력으로 변경을 추적하는 편이
  더 간단하다.
- **영향**:
  - `tools/run-qemu.sh`(현재 스텁, [scaffold-repo-skeleton.md](../done/scaffold-repo-skeleton.md))가
    실제로 작성되는 시점에 QEMU 바이너리 경로를 PATH 탐색 또는 캐시
    변수로 해석하도록 구현해야 한다(ADR-031의 크로스 컴파일러
    툴체인 파일과 동일한 패턴) — 지금은 정책만 정하며, 스크립트
    자체는 아직 수정하지 않는다.
  - fork가 실제로 필요해지는 시점(구체적 기능 공백이 확인되는
    시점)에: (a) fork 저장소를 만들고, (b) 무엇이 왜 부족했는지·어떤
    구현으로 보강했는지를 관련 설계 문서(예: [boot-and-drivers.md](boot-and-drivers.md)류)에
    후속 ADR로 기록한다 — 이 ADR은 정책만 정할 뿐, 이 시점에 확인된
    구체적 기능 공백은 없다.
  - fork 저장소의 정확한 이름·호스팅 위치는 실제로 fork를 만드는
    시점에 정한다.

## ADR-113. freestanding C++ 표준 헤더: libc++ 부재를 자체 shim으로 보강 (부분 해결, → OPEN-48)

- **상태**: 확정 (2026-09-08)
- **결정**: `docs/plan/kernel-bootstrap.md` M1 구현 중, ADR-031에 따라
  설치한 Clang/LLVM(winget `LLVM.LLVM`)에는 **libc++ 헤더가 전혀
  포함되어 있지 않다**는 사실을 확인했다 — 컴파일러 자체가 내장 제공하는
  freestanding C 헤더(`stdint.h`, `stddef.h`, `stdarg.h` 등, 리소스
  디렉토리 경유)는 정상 동작하지만, `cxx-conventions.md` §1이 허용
  목록으로 명시한 C++ 래퍼 헤더(`<cstdint>`, `<cstddef>` 등)는 이를
  제공하는 libc++/libstdc++ 자체가 `x86_64-unknown-none-elf` 같은
  freestanding 타깃용으로 설치되어 있지 않으면 어디에도 없다. 이
  타깃용 libc++ 런타임을 소스에서 직접 빌드하는 것은 별도의 큰 작업이므로
  (LLVM 소스 전체 clone + `runtimes` 빌드, ADR-031의 "저장소 밖에서
  관리" 원칙과는 별개로 시간이 오래 걸림), M1을 막지 않기 위해 **당장
  필요한 3개 헤더(`cstdint`, `cstddef`, `cstdarg`)만 자체 shim으로
  제공**하기로 한다: `toolchain/freestanding-cxx/`에 동명의 헤더 파일을
  두고, 각각 대응하는 C 헤더(`stdint.h` 등)를 include한 뒤 필요한
  심벌만 `namespace std`로 끌어올리는 얇은 래퍼로 작성한다.
  `toolchain/common.cmake`가 이 디렉토리를 `-isystem`으로 전역
  추가한다 — 모든 아키텍처·컴파일러 조합에 동일하게 적용된다.
- **근거**: `cstdint`/`cstddef`/`cstdarg`는 실제 libc++/libstdc++
  구현에서도 대응 C 헤더를 include하고 심벌을 재노출하는 몇 줄짜리
  래퍼에 불과해, 직접 작성해도 표준이 요구하는 내용과 사실상 동일하다
  (구현 세부가 아니라 표준이 보장하는 선언 집합 자체를 옮겨 적는
  수준). 반면 `<type_traits>`, `<concepts>`, `<atomic>` 등은 그 자체가
  상당한 구현체이므로 같은 방식으로 자체 shim을 만드는 것은 비현실적
  이다 — 이 문제는 M3(libk, `docs/plan/kernel-bootstrap.md`)가
  실제로 이 헤더들을 요구하는 시점까지 미룬다.
- **영향**:
  - `cxx-conventions.md` §1의 "허용" 목록은 문구상 변경 없음 — 다만
    실제로 그 헤더들을 컴파일 가능하게 만드는 수단이 (a) 진짜
    libc++ 또는 (b) 이 ADR의 자체 shim, 둘 중 하나임을 명시해야 한다
    (spec 갱신 필요).
  - `<type_traits>`/`<concepts>`/`<bit>`/`<limits>`/`<atomic>`/`<utility>`/`<new>`(placement)에
    대해서는 아직 아무 해결책도 없다 — M3 착수 전에 반드시 결정해야
    한다. → **미결정 (OPEN-48)**: 선택지는 (1) 이들도 개별적으로
    shim 작성, (2) freestanding 타깃용 libc++ 런타임을 실제로 빌드해
    저장소 밖에 설치(ADR-031과 동일 패턴), (3) 해당 표준 헤더 의존을
    포기하고 libk 자체 타입으로 대체. M3 계획 수립 시 결정한다.
  - 저장소 안에 두는 `toolchain/freestanding-cxx/`는 ADR-031이 말하는
    "저장소 밖에서 관리해야 할 툴체인 산출물"이 아니다 — LLVM/libc++
    자체를 vendoring하는 것이 아니라, minicore가 직접 작성한 몇 줄짜리
    소스 파일이므로 일반 저장소 코드와 동일하게 취급한다.

## ADR-115. 나머지 freestanding C++ 헤더: 헤더별 shim 우선, 어려우면 libk 대체 (해결: OPEN-48)

- **상태**: 확정 (2026-09-08)
- **결정**: ADR-113이 미해결로 남긴 `<type_traits>`/`<concepts>`/`<bit>`/
  `<limits>`/`<atomic>`/`<utility>`/`<new>`(placement)에 대해, "freestanding
  타깃용 libc++를 실제로 빌드"하는 선택지(ADR-113이 제시한 옵션 (2))는
  **채택하지 않는다**. 대신 헤더 하나하나에 대해:
  1. 컴파일러 내장 기능(`__has_builtin`류 intrinsic, `__atomic_*`,
     `__is_*` type trait builtin 등)이나 순수 템플릿 메타프로그래밍만으로
     동작이 재현 가능하면 `toolchain/freestanding-cxx/`에 그 헤더 이름
     그대로 shim을 작성한다(ADR-113과 같은 패턴).
  2. 특정 헤더의 표준 동작을 그대로 재현하는 것이 비현실적이거나
     불필요하게 크면(예: `<atomic>`의 메모리 순서 세분화 전체), 그
     표준 헤더 자체를 shim하는 대신 **libk가 필요한 부분집합만 자체
     타입으로 제공**하고(`libk.md`가 이미 `atomic<T>` 자체 래퍼를
     `ADR-010` 범위로 정해둔 것과 같은 방식), 커널·서버 코드는 표준
     헤더 대신 그 libk 타입을 쓴다.
  이 중 어느 쪽을 택할지는 헤더별로 실제 구현 시점(M3, libk 착수)에
  판단한다 — 이 ADR은 "libc++를 통째로 빌드하지 않는다"는 방향만
  확정한다.
- **근거**: 사용자가 대시보드 답변으로 "이들도 개별적으로 shim 작성.
  혹은 호환되는 자체 라이브러리/타입을 개발"이라고 명시했다 — ADR-113의
  세 옵션 중 (1)+(3) 조합을 선택하고 (2)(실제 libc++ 빌드)는 배제하는
  뜻이다. `<type_traits>`/`<concepts>`처럼 순수 컴파일타임 헤더는
  직접 shim이 표준과 사실상 동일한 결과를 내는 반면, LLVM 전체
  소스를 clone해 `runtimes`를 빌드하는 것은(ADR-113이 이미 지적한
  대로) 저장소 밖에서 별도로 관리해야 할 무거운 산출물이 되어 ADR-031의
  정신("minicore를 만드는 도구"를 가볍게 유지)과 맞지 않는다. libk가
  이미 `atomic`/`spinlock` 등 커널 전용 타입을 갖기로 한 것(`libk.md`)과도
  방향이 일치한다.
- **영향**:
  - `docs/spec/cxx-conventions.md` §1·§4가 "이 헤더들은 shim 또는
    libk 대체 중 하나로 제공된다"를 반영하도록 갱신해야 한다(이
    ADR과 함께 즉시 반영).
  - M3(libk) 계획 수립 시, `<type_traits>`/`<concepts>`/`<bit>`/`<limits>`/
    `<utility>`/`<new>`는 shim 우선으로, `<atomic>`은 libk의 `atomic<T>`
    자체 타입으로 대체하는 쪽을 기본 가정으로 삼는다 — 실제 구현
    난이도가 다르면 M3 착수 시 개별적으로 재판단할 수 있다.
  - OPEN-48은 "방향"만 해소한다 — 각 헤더의 실제 shim/대체 작성은
    아직 하지 않았으므로 M3 계획 문서에서 구체적 작업 항목으로
    다시 나열해야 한다.

## ADR-116. freestanding 빌드에도 memset/memcpy/memmove/memcmp를 직접 제공해야 함

- **상태**: 확정 (2026-09-08)
- **결정**: `docs/plan/kernel-bootstrap.md` M2(`boot_info_x86_64.cpp`,
  구조체 zero-init `boot::boot_info info{};`) 구현 중, `-ffreestanding`으로
  빌드해도 Clang이 구조체 초기화·큰 복사를 `memset`/`memcpy` 호출로
  낮출 수 있어(freestanding 여부와 무관한 코드생성 최적화) 링크 시점에
  `undefined symbol: memset`으로 실패함을 확인했다. `kernel/core/freestanding_mem.cpp`에
  `memset`/`memcpy`/`memmove`/`memcmp`의 최소 루프 기반 구현을 두고,
  컴파일러가 그 구현 자체를 다시 memcpy/memset 호출로 "최적화"해
  무한 재귀를 만들지 않도록 이 파일만 `-fno-builtin`으로 컴파일한다
  (`kernel/CMakeLists.txt`의 `set_source_files_properties`).
- **근거**: 이것은 freestanding C/C++ 커널 개발의 잘 알려진 요구사항
  이다 — C++ 표준은 freestanding 구현이 `<cstring>`을 제공할 의무를
  지우지 않지만, 컴파일러 코드생성기는 최적화 목적으로 이 심벌들의
  존재를 가정한 채 호출을 삽입할 수 있다(실제 이번에 M2에서 처음
  발생을 확인했다 — 정적 데이터라 `.bss`만으로 해결되는 M1 범위에서는
  드러나지 않았다). 직접 구현 외의 대안(예: compiler-rt만 링크)은
  이 시점에는 불필요하게 무겁다.
- **영향**:
  - 이후 마일스톤에서 구조체 zero-init/큰 배열 복사를 쓸 때마다 같은
    링크 오류가 재발할 수 있다는 점을 알아둔다 — 이미 해결되어 있으므로
    새로 조사할 필요는 없다.
  - `kernel/core/freestanding_mem.cpp`는 임시 위치다 — M3(libk) 착수
    시 정식 위치(libk 또는 별도 컴파일러 지원 라이브러리)로 옮기는
    것을 검토한다(파일 자체 주석에도 명시).
