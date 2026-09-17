# libcenv - 유저랜드 프리스탠딩 런타임 최소 지원(memcpy/memset 등,
# minicore/libs/libkenv의 유저랜드 짝). PN-185406F6 항목4(pubreg)
# 착수 중 발견 - 그 전까지 유저랜드 프로그램은 컴파일러가 memcpy/
# memset 호출을 만들어낼 만큼 큰 구조체 대입을 쓴 적이 없었다.
add_library(libcenv OBJECT
    mem.cpp
)

# "libcenv/mem.h"처럼 라이브러리 이름을 포함한 경로로 include하게
# 하려고, 디렉터리 자체가 아니라 그 부모(userland/libs)를 public
# include 경로로 노출한다(libmc와 동일한 관례).
target_include_directories(libcenv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
