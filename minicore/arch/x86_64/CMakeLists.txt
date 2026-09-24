# x86-64 아키텍처별 공통 코드 라이브러리 (SP-8B6B8D25 §3.0, RM-23F4B687
# §3 - 2026-09-24부로 유저/커널 공용 위치로 이전, PN-E612E714) - 커널
# 서비스/유저랜드 프로세스가 필요하면 직접 링크해 재사용할 수 있는
# 아키텍처 종속 코드(ISR 저지대 스텁, 포트 I/O, MSR, 컨텍스트
# 스위치, syscall 진입점). 순수 부팅 stub/bootstrap은 여기가 아니라
# minicore/boot/x86_64(구 minicore/arch/x86_64)에 있다 - 이름이
# 헷갈리기 쉬우니 주의(옛 arch/x86_64=부팅 stub, 지금 arch/x86_64=
# 아키텍처 종속 재사용 코드로 역할이 서로 맞바뀌었다).
#
# 5개 파일을 서브디렉터리로 더 쪼개지 않는다(PN-E612E714 결정,
# RM-23F4B687 §4 과설계 방지 원칙) - isr.S/context_switch.S/
# syscall_entry.S는 커널 전용(OBJECT 타깃으로 이미 논리적으로 분리),
# io_port.h/msr.h는 헤더 전용이라 유저랜드가 이 타깃을 링크하지
# 않고도 include만으로 쓸 수 있다.
add_library(minicore_arch_x86_64 OBJECT
    isr.S
    context_switch.S
    syscall_entry.S
)

# "x86_64/io_port.h"처럼 아키텍처 이름을 포함한 경로로 include하게
# 하려고, x86_64 디렉터리 자체가 아니라 그 부모(minicore/arch)를
# public include 경로로 노출한다.
target_include_directories(minicore_arch_x86_64 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
