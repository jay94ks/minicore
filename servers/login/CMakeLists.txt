# 로그인 프롬프트 프로세스(M17, security-model.md ADR-165). main.cpp
# 상단 주석 참고. init/initrun/CMakeLists.txt와 같은 이유로 x86_64
# 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_login INTERFACE)
  return()
endif()

add_executable(minicore_login_elf
    main.cpp
)

target_include_directories(minicore_login_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/mc/include
)

target_link_libraries(minicore_login_elf PRIVATE minicore_libk minicore_libmc)

target_compile_options(minicore_login_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_login_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_login_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)
