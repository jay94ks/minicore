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

## ADR-125. 커널 디버깅 인프라: 디버그 심볼 상시 포함 + QEMU 진단 플래그는 opt-in 환경변수

- **상태**: 확정 (2026-09-08)
- **결정**:
  1. `toolchain/common.cmake`의 `MINICORE_COMMON_COMPILE_OPTIONS`에
     `-g`와 `-fno-omit-frame-pointer`를 조건 분기 없이 상시 추가한다
     (release/최적화 빌드 프리셋 자체가 아직 없으므로 지금은 켜고 끄고를
     구분할 대상이 없다 — 그런 프리셋이 생기면 그때 그 프리셋에서만
     빼는 후속 결정을 한다). `-fno-omit-frame-pointer`는
     [boot-and-drivers.md ADR-126](boot-and-drivers.md)의 패닉 스택
     백트레이스가 rbp/x29 프레임 체인을 걷는 전제 조건이다.
  2. `tools/run-qemu.sh`에 두 개의 opt-in 환경변수를 추가한다 —
     기본값(미설정)에서는 기존 동작과 완전히 동일하다:
     - `MINICORE_QEMU_GDB=1`: QEMU에 `-S -gdb tcp::1234`를 추가해
       CPU를 즉시 정지시키고 GDB 스텁을 연다. 별도 터미널에서 새로
       만든 `tools/debug-gdb.sh <arch>`로 붙는다(같은 커널 ELF를
       심볼과 함께 그대로 읽는다 — 1번 결정 덕분에 별도 준비 불필요).
     - `MINICORE_QEMU_TRACE=1`: `-d cpu_reset,guest_errors,int -D
       <빌드 디렉토리>/qemu-trace.log`를 추가해 트리플폴트·CPU 리셋·
       (아직 IDT는 없지만 이후를 대비한) 인터럽트 이벤트를 파일로
       남긴다.
- **근거**: `tools/smoke-test-x86_64.sh`는 고정 타임아웃(15초) 뒤 QEMU를
  강제 종료해 로그를 검사하는 완전 자동화 경로다 — `-S`로 QEMU가 GDB를
  기다리며 멈추면 이 경로는 항상 타임아웃 실패로 깨진다. 그래서 GDB
  스텁은 기본 off인 opt-in이어야 한다. 트레이스 로그도 이벤트가 많으면
  파일이 커지고 매 부팅마다 남길 이유가 없어 같은 opt-in 패턴을 따른다.
  `-g`는 QEMU의 PVH ELF Note 직접 부팅 경로(ADR-114)가 표준 섹션만
  로드하고 디버그 섹션(`.debug_*`)은 그냥 무시하므로 부팅 동작에
  영향이 없다 — 상시 켜 둬도 손해가 없고, 껐다 켰다 할 대상(release
  빌드)이 아직 없으므로 조건부로 만들 이유도 없다. 실제로 `-g`/
  `-fno-omit-frame-pointer` 추가 후 `smoke-test-x86_64.sh`를 재실행해
  기존 M1~M8 전체 검증 문자열이 그대로 통과함을 확인했다.
- **영향**:
  - 이후 release/최적화 빌드 프리셋이 생기면, 그 프리셋에서 `-g`를
    빼거나 별도 `objcopy --strip-debug` 단계를 추가할지는 그 시점의
    새 ADR 대상이다 — 이 ADR은 "지금은 상시 포함"만 확정한다.
  - `tools/debug-gdb.sh`는 호스트에 (크로스가 아닌) 일반 `gdb`가 있고
    x86_64 타깃을 지원한다는 전제로 만들었다 — 이 개발 머신에서
    `gdb`(mingw64 배포판) 존재만 확인했고, 실제 브레이크포인트·소스
    라인 스테핑까지 왕복 검증하지는 않았다(`-S -gdb tcp::1234`로
    QEMU가 올바르게 멈춰 대기하는 것과, 그 상태에서 커널이 정상
    부팅 로그를 낸다는 것만 확인했다).

## ADR-190. minicore 타깃 크로스 툴체인/SDK 내보내기 — 저장소 밖에서도 minicore 프로그램을 빌드할 수 있게

- **상태**: 확정 (2026-09-10, 계획 단계 — [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
  M38 착수 전에 전략만 먼저 결정한다)
- **결정**:
  1. 새 도구 `tools/export-sdk.py`(ADR-031과 같은 "빌드는 저장소 밖"
     원칙 — SDK 산출물 자체는 저장소에 커밋하지 않는다)가, 이미 빌드된
     minicore 트리에서 다음을 뽑아 `<out>/sdk/x86_64-minicore/`
     한 디렉터리에 모은다:
     - musl의 패치된 공개 헤더(`include/`+`arch/x86_64`+`arch/generic`+
       생성된 `bits/alltypes.h`, M26/M28)
     - `libc.a`(정적, M28~)와 `libc.so`/`ld-musl-x86_64.so.1`(동적,
       M29~)
     - `libmc.a`+헤더(`libmc/include/mc/`) — musl 없이 `libmc`만 직접
       쓰는 순수 C 프로그램(ADR-132의 "2a" 계층)도 지원
     - musl 자신의 crt 객체(`crt1.o` 등, M28이 실제 시작 경로를
       빌드하며 함께 만들어진다)
     - **새 공용 링커 스크립트** — 지금까지 `servers/*`/`userland/*`
       각자가 개별 `link.ld`를 두던 패턴(M12~)과 달리, 저장소 밖
       프로그램을 위한 **하나의 범용 유저 프로그램용 link.ld**를
       새로 만든다(ADR-160의 고정 가상주소 슬롯 배치를 그대로 따름).
  2. **컴파일러 래퍼** `x86_64-minicore-clang`(스크립트) — 이미
     설치된 크로스 clang(ADR-031, [toolchain-setup.md](../done/toolchain-setup.md))
     에 `--target=x86_64-linux-musl --sysroot=<sdk 경로>`를 자동으로
     얹어 호출한다. **musl을 그대로 `x86_64-linux-musl` 트리플로
     컴파일한다** — 이 트리플 자체를 바꿀 이유가 없다(컴파일 단계는
     타깃 커널과 무관하다, 런타임에 실제로 다른 것은 syscall 경로뿐
     이고 그건 ADR-183의 패치된 `syscall_arch.h`가 이미 흡수한다) —
     "sysroot만 minicore 것으로 바꾼 표준 트리플"이라는 흔한
     크로스 SDK 패턴(임베디드/게임 콘솔 SDK들이 흔히 쓰는 방식)을
     그대로 따른다.
  3. **최소 CMake 툴체인 파일**(`sdk/x86_64-minicore.cmake`) — 외부
     프로젝트가 `-DCMAKE_TOOLCHAIN_FILE=.../x86_64-minicore.cmake`
     하나만 지정하면 되도록 위 래퍼와 sysroot 경로를 미리 박아 둔다.
     minicore 자신의 `toolchain/x86_64-clang.cmake`(freestanding
     플래그 등, 커널/서버 빌드용)와는 **별개**로 관리한다 — 같은
     clang 설치를 재사용하지만 용도(저장소 안 vs 밖)가 다르다.
- **근거**: 지금까지 모든 유저랜드 코드(셸, musl 문자열 함수, 앞으로의
  syscall_shim 테스트 프로그램)는 minicore 저장소 **안에서**
  `third_party/`+`libc/`+`userland/*`의 CMake 서브프로젝트로만
  빌드됐다(M20/M26이 그렇게 했다) — 이 방식은 "minicore 자체를
  개발하는 사람"에게는 자연스럽지만, "minicore용 프로그램 하나만
  새로 만들고 싶은 사람"에게는 전체 커널/서버 소스를 체크아웃하고
  빌드 시스템 전체를 이해해야 하는 과도한 진입장벽이다. 실제
  운영체제 SDK(예: 각종 임베디드/콘솔 개발킷)가 "컴파일러+sysroot
  패키지 하나"로 이 문제를 푸는 것과 같은 해법을 채택한다.
- **영향**:
  - `tools/mkbootdisk.py`(기존)가 이 SDK로 빌드된 ELF도 그대로
    받아들이는지 확인한다 — 형식(정적/동적 ELF, 진입점 관례)이
    minicore 자체 빌드 산출물과 같으므로 수정이 필요 없을 가능성이
    높지만, M38 착수 시점에 실제로 확인한다.
  - 이 SDK가 내보내는 sysroot의 **버전 고정** 문제(minicore 자체가
    계속 바뀌는데 이미 내보낸 SDK가 낡아지는 것)는 이번 라운드
    범위 밖이다 — 지금은 "매번 새로 export한다"는 전제로 충분하다
    (배포/버저닝 정책은 실제 외부 사용자가 생기는 시점의 후속 결정).

## ADR-213. M38 완성: `export-sdk.py`가 정적 libc.a/libmc.a+musl 헤더만 내보냄(동적 링킹 계획은 빠짐) — CMake 경로는 컴파일러 래퍼가 아니라 툴체인 파일이 clang을 직접 부름

- **상태**: 확정 (2026-09-10, [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
  M38 실행 중 확정)
- **배경**: ADR-190(계획 단계)의 결정을 실제로 구현한다. 계획이
  예정한 3개 산출물(export-sdk.py, 컴파일러 래퍼, CMake 툴체인
  파일)을 전부 만들었지만, 실행 중 계획 문서 작성 시점엔 몰랐던
  두 가지를 발견해 범위를 조정했다.
- **결정**:
  1. `tools/export-sdk.py` — 이미 빌드된 트리(`build/<arch>-clang`)
     에서 `<out>/x86_64-minicore/`로 뽑는다: `include/musl/{arch/
     x86_64,arch/generic,generated,include}/`(libc/CMakeLists.txt가
     `minicore_libc`에 PUBLIC으로 거는 include 디렉터리 4개와
     정확히 같은 목록·순서 — 순서가 실제로 의미 있다는 것을 M36이
     이미 알아냈다), `include/mc/`(libmc 공개 헤더), `lib/libc.a`+
     `lib/libmc.a`, 공용 `link.ld`(저장소 안
     `userland/musl-hello/link.ld`를 그대로 재사용 — 그 자체가
     musl-hello 특화 내용이 없었다), `bin/x86_64-minicore-clang`
     (참고용 셸 스크립트, 아래 §결정3), `x86_64-minicore.cmake`.
  2. **동적 `libc.so`/`ld-musl-x86_64.so.1`은 내보내지 않는다** —
     ADR-190 원안은 M29의 동적 링킹 결과물을 가정했지만, M29 자신이
     이미 ADR-203으로 정적 링킹으로 되돌아가 있다(이 계획 문서
     자체가 그렇게 기록해 뒀다) — 존재하지 않는 산출물을 내보낼
     수는 없으므로, SDK는 처음부터 정적 `libc.a`만 다룬다(계획
     문서를 다시 읽고서야 이 불일치를 알아챘다).
  3. **타깃 트리플은 ADR-190 원안의 `x86_64-linux-musl`이 아니라
     `x86_64-unknown-none-elf`로 확정한다** — minicore 저장소 안의
     `libc.a` 자신이 실제로 이 트리플로 컴파일됐다
     (`toolchain/x86_64-clang.cmake`, ADR-020). 이 SDK의 헤더/
     라이브러리를 다른 트리플로 컴파일하는 조합은 한 번도 검증된
     적이 없어, 검증된 조합을 그대로 유지한다 — "sysroot만 바꾼
     표준 트리플"이라는 ADR-190의 원래 그림보다는 덜 "표준"이지만,
     실제로 동작이 확인된 조합을 우선한다.
  4. **CMake 경로는 `bin/x86_64-minicore-clang`(bash 스크립트)를
     `CMAKE_C_COMPILER`로 직접 가리키지 않는다** — cmake/ninja는
     컴파일러를 셸을 거치지 않고 OS 프로세스 실행기로 직접
     실행하는데, 이 세션의 호스트(Windows)에서는 bash 스크립트를
     그렇게 실행할 수 없다는 것을 실제로 겪었다("%1 is not a valid
     Win32 application"). `x86_64-minicore.cmake`는 대신
     `toolchain/x86_64-clang.cmake`(ADR-020)와 완전히 같은 방식으로
     `CMAKE_C_COMPILER=clang`+`CMAKE_C_FLAGS_INIT`(타깃 트리플+
     include 경로)를 직접 설정해 clang을 그대로 부른다 — 이 경로가
     크로스플랫폼에서 실제로 검증된 방식이다. 셸 스크립트 래퍼는
     CMake를 안 쓰는 사용자(수동 명령줄, 또는 셸을 거치는
     Makefile)를 위한 참고 자료로만 남긴다. 최종 링크는 두 경로
     모두 clang을 링크 드라이버로 쓰지 않고 `ld.lld`를 직접
     부른다(`userland/musl-hello/CMakeLists.txt`와 완전히 같은
     이유 — clang의 링크 모드는 자기 자신의 기본 crt/libc 탐색을
     가정해서 이 SDK의 정적 라이브러리+커스텀 `link.ld` 조합과 안
     맞는다).
- **검증**: 저장소 밖 스크래치 디렉터리에 `printf`+`malloc`+
  `strcpy`만 쓰는 순수 C "hello world"(minicore 소스 트리를 전혀
  참조하지 않음)를 작성하고, `x86_64-minicore.cmake` 하나만 지정해
  CMake+ninja로 컴파일·링크에 성공했다. 결과 ELF를
  `tools/mkbootdisk.py --service=sdk-hello=<그 ELF>
  --linux-abi-stack=sdk-hello`(수동 호출, 저장소의
  `servers/CMakeLists.txt`는 건드리지 않았다 — 이 검증은 일회성
  증명이라 영구 기능으로 편입하지 않는다)로 임시 bootdisk에 넣고,
  `MINICORE_QEMU_BOOTDISK` 환경변수로 그 이미지를 지정해
  `tools/run-qemu.sh`로 부팅해 "hello from minicore SDK (23 bytes)"
  가 정확히 출력됨을 확인했다 — `tools/mkbootdisk.py`는 수정 없이
  그대로 받아들였다(ADR-190이 미리 걸어 둔 "확인한다" 항목,
  실제로 형식이 같아 수정이 필요 없었다).
- **범위 밖**: SDK 버전 고정/배포 정책(ADR-190이 이미 범위 밖으로
  남김), 동적 링킹 지원(M29/ADR-203의 정적 링킹 복귀가 유효한 한
  이 SDK도 정적만 다룬다), Linux/macOS 호스트에서의 실제 검증(이
  세션은 Windows 호스트에서만 확인했다 — 셸 스크립트 래퍼 자체는
  다른 호스트에서 더 유용해질 수 있지만 이번 라운드는 검증하지
  않았다).

## ADR-195. 와이어 프로토콜 정의: 각 서버 헤더에 추출 가능한 마크업 + Python 추출 도구 (OPEN-54 방법론 확정)

- **상태**: 확정 (2026-09-10, 계획 단계 — [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
  M27이 procsrv 와이어 프로토콜을 확정하기 **전에** 이 방법론부터
  적용한다. 사용자 지시: "바이트 단위 메시지 포맷은 각 서비스
  헤더에 정의하고 Python 등 스크립트 언어가 추출할 수 있는 형태로
  마크업해야 한다 — 치명적 결과를 유발할 수 있으니 헤더 작성과
  동시에 설계 문서에 반영하고 계획을 먼저 세운다.")
- **결정**:
  1. **와이어 프로토콜(메시지 레이아웃, `label` 값, `regs[]`/`pages[]`
     배치)의 정본은 여전히 `libmc/include/mc/<server>_protocol.h`
     (ADR-132 §결정2가 이미 정한 자리)다** — 이 ADR은 그 자리를
     바꾸지 않고, 그 헤더에 **기계 추출 가능한 마크업**을 추가하는
     관례만 새로 정한다.
  2. **마크업 형식**: 일반 C 주석 안에 고정 태그를 쓴다(별도
     전처리기 확장 없이 표준 C 헤더로 그대로 컴파일된다는 것이
     핵심 — clang 프리스탠딩 빌드(ADR-010)를 방해하지 않는다):

     ```c
     // @wire-op label=1 name=fork request=none reply="int32 child_pid"
     // @wire-op label=2 name=exec request="bytes path; bytes argv" reply=none
     ```

     각 `struct`/`enum` 값 바로 위에 한 줄로 적는다 — 필드명:타입
     쌍은 세미콜론으로 구분한 아주 단순한 문법만 지원한다(YAGNI,
     복잡한 IDL 문법을 새로 발명하지 않는다).
  3. 새 도구 `tools/gen-wire-docs.py`(다른 codegen 도구들과 같은
     이유로 Python — `tools/mkinitrd.py`/`gen-musl-alltypes.py`와
     셸 의존성을 통일)가 지정된 헤더들에서 이 `@wire-op` 태그를
     정규식으로 추출해 사람이 읽는 참조 표
     (`docs/spec/generated/<server>-wire.md`, 자동 생성 — 손으로
     수정하지 않는다는 것을 파일 상단에 명시)로 렌더링한다. CI는
     아직 없지만(이전 라운드들과 동일하게 범위 밖), 이 스크립트를
     빌드 후 수동으로도 돌릴 수 있게 `tools/`에 둔다.
  4. **새 와이어 프로토콜을 설계할 때는 반드시 이 순서를 따른다**:
     (a) `libmc` 헤더에 구조체/enum과 `@wire-op` 마크업을 함께
     작성 → (b) `tools/gen-wire-docs.py`로 생성한 참조 표를
     확인해 실수(레이블 중복, 필드 누락)를 조기에 잡음 → (c) 그제서야
     클라이언트(`libmc` 구현)/서버(각 `servers/*`) 양쪽을 구현한다.
     "헤더 먼저, 코드는 그 다음"이라는 순서 자체가 이미 ADR-132
     §결정2의 취지이지만, 이 ADR은 그 순서를 **마크업+추출 도구로
     실제로 강제**한다.
- **근거**: 사용자가 지적한 대로 클라이언트/서버가 와이어 포맷에
  대해 합의가 깨지면(필드 순서, 크기, label 값 불일치) 그 결과는
  단순 컴파일 에러가 아니라 **런타임에 조용히 잘못된 메모리를
  읽는** 치명적 버그가 된다(POD 구조체를 그대로 IPC 페이로드로
  쓰는 이 프로젝트의 관례, ADR-132 §결정2). 사람이 손으로 문서와
  코드를 동기화하면 이 동기화가 깨지는 것은 시간 문제다 — 코드
  (헤더)에서 문서를 **추출**하면 둘이 어긋날 방법이 원천적으로
  없어진다. Python을 선택한 이유는 이 프로젝트가 이미 코드생성에
  일관되게 Python을 쓰기 때문이다(ADR-182의 `gen-musl-alltypes.py`
  등) — libclang 같은 무거운 의존성 대신 정규식 기반 경량 추출로
  충분한 이유는, 이 마크업 자체를 단순한 한 줄 형식으로 제한했기
  때문이다(3번 결정).
- **영향**:
  - **OPEN-54의 "방법론"이 이 ADR로 확정된다** — 실제 procsrv
    와이어 프로토콜의 바이트 레이아웃 자체는 여전히 M27
    착수 시점에 정해진다(OPEN-54 완전 해소는 M27 완료 후).
  - M27은 이제 "구현" 첫 단계로 **이 마크업 컨벤션+
    `tools/gen-wire-docs.py`를 먼저 만든 뒤** procsrv 프로토콜
    헤더를 작성한다 — [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
    M27에 반영했다.
  - 기존에 이미 작성된 와이어 프로토콜 헤더(vfs/fs-protocol,
    devmgr 등)에 이 마크업을 소급 적용할지는 이 ADR의 범위 밖이다 —
    **새로 작성하는 프로토콜부터** 적용한다(기존 것은 필요해지면
    나중에 마크업을 추가).

## ADR-199. 이 저장소가 직접 만들고 유지·관리하는 라이브러리는 `libs/` 하위로, 이름은 `lib` 접두사 없이

- **상태**: 확정 (2026-09-10, 사용자 지시. 규칙만 확정 — 실제 이동/
  리네임은 [libs-restructure.md](../plan/libs-restructure.md)로
  분리)
- **결정**:
  1. 저장소 최상위에 새 디렉터리 `libs/`를 두고, 이 저장소가 직접
     만들고 유지·관리하는 라이브러리(현재 `libk/`, `libmc/`)를 전부
     그 아래로 옮긴다.
  2. **명명 규칙**: 라이브러리 이름에 `lib` 접두사를 붙이지 않는다 —
     디렉터리명은 그 라이브러리의 짧은 이름 그대로다.
     - `libk` → `k`(`libs/k/`)
     - `libmc` → `mc`(`libs/mc/`)
     - 앞으로 이 저장소에 추가되는 라이브러리도 이 규칙을 따른다.
  3. **인클루드 경로**: `libmc/include/mc/`는 이미 내부 서브디렉터리
     이름이 `mc`였으므로(`lib` 접두사가 원래 없었다) `libs/mc/include/mc/`
     로 **최상위 두 세그먼트만** 바뀐다 — `#include <mc/...>`
     문구 자체는 소스에서 바뀌지 않는다(CMake의
     `target_include_directories`만 새 경로를 가리키면 된다).
     반면 `libk/include/libk/`는 내부 세그먼트도 `libk`였으므로
     `libs/k/include/k/`로 **두 곳 다** 바뀐다 — `#include <libk/...>`
     를 쓰던 모든 소비자가 `#include <k/...>`로 바뀐다(더 넓은
     변경).
  4. CMake 타깃 이름(`minicore_libk`, `minicore_libmc`)을 이 명명
     규칙에 맞출지는 이 ADR이 확정하지 않는다 — 실행 계획 착수
     시점에 정한다(예: `minicore_k`로 바꿀 수도, CMake 타깃명은
     디렉터리/인클루드 규칙과 별개로 유지할 수도 있다).
- **근거**: 사용자가 명시적으로 지정했다 — 디렉터리/타깃 이름의
  `lib` 접두사는 결과 산출물(정적 라이브러리 파일 자체, 예:
  `libk.a`)이 이미 갖는 접두사와 중복되는 정보라, 소스 트리 이름에서는
  빼는 쪽이 간결하다.
- **영향**:
  - [repo-layout.md](repo-layout.md)의 디렉토리 트리를 목표 상태로
    갱신했다(아직 실행 전임을 명시).
  - 실제 이동/리네임/CMake 경로 수정은
    [libs-restructure.md](../plan/libs-restructure.md) M49가
    다룬다.
  - 이미 작성된 과거 ADR·계획 문서 안의 `libk/`/`libmc/` 언급은
    **소급 수정하지 않는다** — 그 문서가 쓰인 시점의 정확한
    사실이었다(문서 체계 원칙, ADR 불변). 아직 실행되지 않은
    계획 문서([real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md)
    등)의 경로 언급은 실제 이동이 끝나는 시점에 함께 갱신한다.
