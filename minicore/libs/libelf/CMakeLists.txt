# ELF64 파서(SP-8B6B8D25 §3 배치 규칙, libcpio와 같은 "kernel/userland
# 공용" 원칙) - 소스(elf.h/.cpp) 자체는 어느 쪽에도 종속되지 않지만,
# 커널과 유저랜드는 서로 다른 툴체인(mcmodel/PIE 등)으로 컴파일되는
# 완전히 별개의 빌드라 컴파일된 오브젝트 자체는 공유할 수 없다 - 지금은
# 이 저장소에 커널 빌드만 있어 이 타깃이 곧 그 "커널용 컴파일"이다.
# MINICORE_LIBELF_KERNEL은 이 타깃에서만 정의한다(설계자 지시 "기본값은
# 유저 영역용" - 소스 코드 자체의 기본 상태는 매크로 없음=유저 영역용
# 이고, 이 커널 타깃만 명시적으로 켠다). 유저랜드 빌드 체계
# (userland/, 계획 PN-16CA347D)가 생기면 같은 소스 파일을 이 매크로
# 없이 별도 타깃으로 다시 컴파일한다.
add_library(libelf OBJECT
    elf.cpp
)

target_include_directories(libelf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_SOURCE_DIR}/minicore/kernel)

# PUBLIC이어야 한다 - 이 매크로는 elf.h의 커널 전용 선언(loadIntoAddressSpace)
# 자체를 게이팅하므로, libelf를 링크하는 다른 타깃(minicore_kernel)이
# elf.h를 include할 때도 똑같이 보여야 그 선언을 볼 수 있다.
target_compile_definitions(libelf PUBLIC MINICORE_LIBELF_KERNEL=1)
