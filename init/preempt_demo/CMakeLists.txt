# M21(선점형 스케줄링, docs/plan/general-purpose-completion.md) 검증용
# 데모 유저 프로세스 두 개(busy/counter). init/initrun과 같은 이유로
# top-level CMakeLists.txt가 kernel보다 먼저 이 서브디렉터리를
# 추가한다 — kernel_main.cpp의 데모 경로가 initrun과 똑같이 이
# 결과물을 (같은 initrd MCPACK 이미지 안에) .incbin으로 심어 쓴다.
# x86_64 전용이다(init/initrun/CMakeLists.txt와 같은 이유).
if(NOT MINICORE_ARCH STREQUAL "x86_64")
  add_library(minicore_preempt_demo INTERFACE)
  return()
endif()

add_executable(minicore_preempt_busy_elf busy.c)
add_executable(minicore_preempt_counter_elf counter.c)

foreach(tgt minicore_preempt_busy_elf minicore_preempt_counter_elf)
  target_compile_options(${tgt} PRIVATE
      -mno-red-zone
      -mno-mmx
      -mno-sse
      -mno-sse2
      -mno-3dnow
  )

  set_target_properties(${tgt} PROPERTIES
      LINKER_LANGUAGE C
      LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
      SUFFIX ".elf"
  )
endforeach()

# kernel/arch/x86_64/CMakeLists.txt와 같은 이유(Windows 호스트에서
# clang의 링크 드라이버가 --target=*-unknown-none-elf 링크를 POSIX
# 호스트용 g++/링커에 잘못 위임한다) — ld.lld를 직접 호출한다.
set(CMAKE_C_LINK_EXECUTABLE
    "ld.lld -m elf_x86_64 <LINK_FLAGS> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>"
)

target_link_options(minicore_preempt_busy_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)
target_link_options(minicore_preempt_counter_elf PRIVATE
    -static
    --build-id=none
    -T ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
)
