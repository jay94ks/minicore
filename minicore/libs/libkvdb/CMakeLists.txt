# libkvdb - 고정 용량 Key-Value 저장소(SP-30FCC8AE §1-D, PN-24A2B6F5
# §1-C/D 항목1) - 순수 헤더(템플릿) 전용이라 컴파일할 .cpp가 없으므로
# libkcont와 동일하게 INTERFACE 라이브러리로 둔다.
add_library(libkvdb INTERFACE)

# "libkvdb/kvdb.h"처럼 라이브러리 이름을 포함한 경로로 include하게
# 하려고, libkvdb 디렉터리 자체가 아니라 그 부모(minicore/libs)를
# public include 경로로 노출한다(libkcont/libkenv와 동일한 관례).
target_include_directories(libkvdb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
