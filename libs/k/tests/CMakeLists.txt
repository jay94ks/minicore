# k(구 libk) 호스트 네이티브 단위 테스트 (ADR-077/199) — 크로스 컴파일
# 트리(최상위 CMakeLists.txt, toolchain/*.cmake)와 완전히 독립된 별도
# CMake 프로젝트다. 개발 머신의 네이티브 툴체인으로 빌드·실행한다:
#
#   cmake -S libs/k/tests -B build/k-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/k-tests
#   ctest --test-dir build/k-tests
#
# (또는 빌드된 실행파일을 직접 실행해도 된다 — 표준 출력에 결과 요약,
# 실패 시 0이 아닌 종료 코드.) -DCMAKE_CXX_COMPILER=clang++이 필요한
# 이유: k가 GCC/Clang 공통 컴파일러 내장(__atomic_*, __is_constructible
# 등, ADR-020/072/115)을 직접 쓰는데, Windows에서 CMake 기본 제너레이터를
# 그냥 두면 MSVC가 선택되어(이 내장들을 모른다) 컴파일이 실패한다.
cmake_minimum_required(VERSION 3.20)
project(k_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(k_tests
    main.cpp
    host_stubs.cpp
)
target_include_directories(k_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

enable_testing()
add_test(NAME k_tests COMMAND k_tests)
