# 프로세스 서버: fork/exec/signal/wait/pid, fd 진실 공급원 (ADR-016).
# M12(system-servers-bringup.md) 골격 — main.cpp 상단 주석 참고.
# init/initrun/CMakeLists.txt와 같은 이유로 x86_64 전용이다.
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_procsrv INTERFACE)
  return()
endif()

# M32(real-libc-syscall-layer.md §M32) — musl-hello가 execve()할
# "다른 실행 이미지"(userland/musl-exec-target)의 원본 바이트를
# procsrv 자신의 컴파일 시점 데이터로 심는다(procsrv가 부팅 시 VFS
# (memfs)에 그 바이트를 미리 써 둬야 musl-hello가 경로로 열 수
# 있다 — 커널이 initrun.elf를 자기 이미지에 .incbin하는 것과 같은
# 목적이지만, procsrv는 일반 유저 프로세스 빌드 드라이버라 .incbin을
# 못 써서 tools/bin2c.py로 대신한다).
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(MUSL_EXEC_TARGET_BLOB_H ${CMAKE_CURRENT_BINARY_DIR}/generated/musl_exec_target_blob.h)
add_custom_command(
    OUTPUT ${MUSL_EXEC_TARGET_BLOB_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bin2c.py
            $<TARGET_FILE:minicore_musl_exec_target_elf>
            ${MUSL_EXEC_TARGET_BLOB_H}
            g_musl_exec_target_elf
    DEPENDS minicore_musl_exec_target_elf
            ${CMAKE_SOURCE_DIR}/tools/bin2c.py
    COMMENT "Generating musl-exec-target ELF blob header (tools/bin2c.py)"
    VERBATIM
)
add_custom_target(minicore_procsrv_musl_exec_target_blob DEPENDS ${MUSL_EXEC_TARGET_BLOB_H})

# M43(user-service-manager.md §M43) — op_spawn_delegated_unit(ADR-218)이
# 스폰할 ELF. svcmgr가 IPC regs로 넘긴 포인터는 svcmgr **자신의**
# 주소공간을 가리켜 procsrv가 그대로 역참조할 수 없다(서로 다른
# 프로세스) — 위 musl-exec-target과 같은 이유로, procsrv 자신이
# userland/svcmgr-demo-unit의 같은 ELF를 컴파일 시점 데이터로
# 직접 심어 둔다(exec_path는 여전히 범위 밖이라 system-scope 유닛과
# 같은 데모 ELF 하나만 쓴다, M41/M42와 같은 제약).
set(USER_SERVICE_UNIT_BLOB_H ${CMAKE_CURRENT_BINARY_DIR}/generated/user_service_unit_blob.h)
add_custom_command(
    OUTPUT ${USER_SERVICE_UNIT_BLOB_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bin2c.py
            $<TARGET_FILE:minicore_svcmgr_demo_unit_elf>
            ${USER_SERVICE_UNIT_BLOB_H}
            g_user_service_unit_elf
    DEPENDS minicore_svcmgr_demo_unit_elf
            ${CMAKE_SOURCE_DIR}/tools/bin2c.py
    COMMENT "Generating user-service-unit ELF blob header (tools/bin2c.py)"
    VERBATIM
)
add_custom_target(minicore_procsrv_user_service_unit_blob DEPENDS ${USER_SERVICE_UNIT_BLOB_H})

# docs/plan/musl-userland-porting.md §M52(ADR-221) — 자체 작성 최소
# coreutils(echo/ls/cat)를 위 musl-exec-target과 같은 이유로 procsrv
# 자신의 컴파일 시점 데이터로 심는다 — userland/msh가 execve()로
# "/bin/echo" 등을 열 수 있으려면 그 바이트가 부팅 시 VFS(memfs)에
# 먼저 쓰여 있어야 한다. M53(ADR-224)부터는 msh 자신도 같은 방식으로
# 심는다 — 다만 msh는 VFS에 쓰지 않는다(procsrv가 로그인 성공 시
# sys_process_spawn의 elf_data로 이 바이트를 직접 넘겨 스폰하므로,
# 경로로 다시 열어 볼 필요가 없다).
function(minicore_embed_userland_elf name target)
    set(blob_h ${CMAKE_CURRENT_BINARY_DIR}/generated/${name}_blob.h)
    add_custom_command(
        OUTPUT ${blob_h}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bin2c.py
                $<TARGET_FILE:${target}>
                ${blob_h}
                g_${name}_elf
        DEPENDS ${target}
                ${CMAKE_SOURCE_DIR}/tools/bin2c.py
        COMMENT "Generating ${name} ELF blob header (tools/bin2c.py)"
        VERBATIM
    )
    add_custom_target(minicore_procsrv_${name}_blob DEPENDS ${blob_h})
endfunction()

minicore_embed_userland_elf(echo minicore_echo_elf)
minicore_embed_userland_elf(ls minicore_ls_elf)
minicore_embed_userland_elf(cat minicore_cat_elf)
minicore_embed_userland_elf(msh minicore_msh_elf)

add_executable(minicore_procsrv_elf
    main.cpp
)

add_dependencies(minicore_procsrv_elf minicore_procsrv_musl_exec_target_blob
                  minicore_procsrv_user_service_unit_blob
                  minicore_procsrv_echo_blob minicore_procsrv_ls_blob minicore_procsrv_cat_blob
                  minicore_procsrv_msh_blob)

target_include_directories(minicore_procsrv_elf PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/mc/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(minicore_procsrv_elf PRIVATE minicore_libk minicore_libmc)

# init/initrun/CMakeLists.txt와 같은 이유(SSE/MMX 미사용, red zone 불필요).
target_compile_options(minicore_procsrv_elf PRIVATE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-3dnow
)

# init/initrun/CMakeLists.txt와 같은 이유(Windows 호스트 clang++ 링크
# 드라이버 우회) — ld.lld를 직접 호출한다.
set(CMAKE_CXX_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_procsrv_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)

set_target_properties(minicore_procsrv_elf PROPERTIES
    LINKER_LANGUAGE CXX
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
    SUFFIX ".elf"
)

# 부트 디스크(bootdisk.img) 조립은 M13부터 여러 서비스(procsrv/vfs/
# memfs)를 함께 담아야 해서 servers/CMakeLists.txt로 옮겼다 —
# 거기서만 이 셋을 모두 참조할 수 있다.
