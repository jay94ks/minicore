set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# [교체, 2026-09-17, PN-22E5E9E7 lld PT_TLS 다중 오브젝트 크기 합산
# 결함(QU-90A616DA) 대응, 설계자 직접 지시] clang+lld(호스트 Ubuntu
# 24.04가 제공하는 x86_64-unknown-none-elf 프리스탠딩 타깃 흉내) 대신
# 진짜 x86_64-elf 크로스컴파일 툴체인(/opt/cross, GCC 13.2.0 + GNU
# Binutils 2.42 - 실제 이 타깃 전용으로 빌드됨)으로 전면 교체 - `-fuse-ld`
# 트릭 없이도 자신의 짝인 x86_64-elf-ld를 그대로 쓴다(같은 실제 오브젝트
# 세트로 PT_TLS.p_memsz를 정확히 계산함을 실측 확인). `--target=` 삼중
# 문자열은 clang 전용 흉내 메커니즘이라 진짜 크로스컴파일러에는 불필요
# (이미 그 타깃 전용으로 빌드돼 있음).
set(CMAKE_C_COMPILER /opt/cross/bin/x86_64-elf-gcc)
set(CMAKE_CXX_COMPILER /opt/cross/bin/x86_64-elf-g++)
set(CMAKE_ASM_COMPILER /opt/cross/bin/x86_64-elf-gcc)

# 부트로더가 넘겨준 링크 상태(크로스컴파일 + 프리스탠딩)에서는 CMake의
# 기본 컴파일러 점검(실행 파일 링크 시도)이 실패한다 - 정적 라이브러리
# 링크로 낮춰서 툴체인 자체는 정상 동작함을 확인하게 한다.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# [교체, 2026-09-17, QU-6A72AFE6 설계자 답변] `-mgeneral-regs-only`
# 대신 `-mno-mmx -mno-sse -mno-sse2` + `-mcmodel=large`로 교체 -
# 설계자가 예전 커널 개발 시 쓰던 플래그 세트 중 필요한 것만 채택
# (libjson/libutf8 착수 중 실측 발견: `-mgeneral-regs-only`는 SSE뿐
# 아니라 x87까지 전부 막아 `double` 산술이 있는 함수는 컴파일 자체가
# 실패했다). `-mno-sse`류는 SSE 레지스터"만" 금지하므로 컴파일러가
# `double`/`float` 산술을 x87 스택 명령어로 대체 - "당연히 no-mmx/
# no-sse/no-sse2가 적용되면 MMX/SSE/SSE2는 인라인 어셈블러나 외부
# 어셈블러 코드로만 사용 가능해"(설계자 코멘트, 일반 C++ 코드에서
# MMX/SSE 내장 함수는 여전히 못 씀 - 의도된 제약). `-mcmodel=kernel`
# 대신 `-mcmodel=large`로도 교체(설계자 지시 그대로) - 커널 자신의
# 링크 주소가 상위 2GiB 안에 있다는 가정 자체를 하지 않는 가장 보수적인
# 코드 모델이라 이 프로젝트의 higher-half 링커 스크립트 배치와 항상
# 호환된다(더 큰/느린 절대주소 addressing을 대가로 어떤 배치에서도
# 항상 올바름 - PN-F258698E의 FPU 지연 컨텍스트 저장/복원 로직이
# x87 레지스터 상태(FXSAVE/FNSAVE 등)에도 여전히 맞물리는지는 착수
# 세션이 실측 재검증).
set(_minicore_freestanding_flags
    "-ffreestanding -fno-stack-protector -fno-pic -fno-pie \
-mno-red-zone -mcmodel=large -mno-mmx -mno-sse -mno-sse2 \
-Wno-missing-field-initializers")

set(CMAKE_C_FLAGS_INIT "${_minicore_freestanding_flags}")
# [추가, 2026-09-17] `-fcoroutines` - GCC는 clang과 달리 `-std=c++20`
# 만으로 `co_return`/`co_await`(이 커널의 AsyncTaskHandler 전체가
# 의존하는 C++20 코루틴)를 켜지 않는다 - 명시적 플래그가 반드시 필요.
# `-fno-threadsafe-statics` - 함수 지역 static(예: LiveFs::instance()의
# Meyer's singleton)에 대해 GCC는 표준대로 `__cxa_guard_acquire/release`
# 스레드 안전 가드를 방출하는데, 이 심볼들을 이 프리스탠딩 커널이 아직
# 구현해 두지 않았다(clang은 같은 코드에서 이 가드 자체를 요구하지
# 않았다 - 컴파일러별 최적화 차이). 부팅 극초반 각 싱글턴은 실질적으로
# 항상 한 코어가 먼저 접근해 초기화를 끝내므로(SMP AP들은 그 이후에나
# 기동) 진짜 동시 초기화 경쟁은 없다고 보고, 가드 인프라를 새로 구현하는
# 대신 표준 커널/프리스탠딩 관례대로 이 플래그로 끈다.
set(CMAKE_CXX_FLAGS_INIT "${_minicore_freestanding_flags} -fno-exceptions -fno-rtti -fcoroutines -fno-threadsafe-statics")
set(CMAKE_ASM_FLAGS_INIT "-ffreestanding")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static")
