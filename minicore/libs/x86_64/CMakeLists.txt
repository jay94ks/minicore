# x86-64 아키텍처별 공통 코드 라이브러리 (SP-8B6B8D25 §3.0) - 커널
# 서비스가 필요하면 직접 링크해 재사용할 수 있는 아키텍처 종속 코드
# (ISR 저지대 스텁, 포트 I/O). 순수 부팅 stub/bootstrap은 여기가
# 아니라 minicore/arch/x86_64에 있다.
add_library(minicore_libs_x86_64 OBJECT
    isr.S
)

# "x86_64/io_port.h"처럼 아키텍처 이름을 포함한 경로로 include하게
# 하려고, x86_64 디렉터리 자체가 아니라 그 부모(minicore/libs)를
# public include 경로로 노출한다.
target_include_directories(minicore_libs_x86_64 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
