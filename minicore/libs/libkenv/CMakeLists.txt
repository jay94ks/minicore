# libkenv - early 환경 C/C++ 런타임 라이브러리 (SP-8B6B8D25 §3.1b).
add_library(libkenv OBJECT
    mem.cpp
    cxxabi.cpp
    maple_tree.cpp
)

# "libkenv/io_port.h"처럼 라이브러리 이름을 포함한 경로로 include하게
# 하려고, libkenv 디렉터리 자체가 아니라 그 부모(minicore/libs)를
# public include 경로로 노출한다.
target_include_directories(libkenv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
