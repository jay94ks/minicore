# minicore/epolltest - SP-6350DEBB/PN-7562DA62(epoll류 이벤트 다중화)
# 전용 최소 실측 검증 프로그램(서버 역할) - 기존 minicore/sockclient
# (클라이언트 역할, "socktest.sock" 하드코딩)를 그대로 재사용해 짝을
# 이룬다. socktest와 동일한 이유로 일반 유저 앱과 완전히 같은 실행
# 파일 형태를 써야 해서 userland/ 빌드 트리를 재사용한다.
# **의도적으로 부팅 매니페스트에는 포함하지 않는다** - main.cpp 상단
# 주석 참고, kmain.cpp의 TEMP 스폰 경로로만 실행된다. **socktest와
# 같은 부팅에 절대 함께 켜지 않는다** - 둘 다 sockclient가 하드코딩한
# "socktest.sock" 이름을 두고 경합하게 된다(이 세션이 이미 겪은
# NamedObjectTable 이름 충돌 함정, PN-6360E6E9 정정 절 참고).
add_executable(epolltest main.cpp)

set_target_properties(epolltest PROPERTIES
    LINK_DEPENDS ${MINICORE_USERLAND_LINKER_SCRIPT}
    SUFFIX ""
)

target_link_options(epolltest PRIVATE -T ${MINICORE_USERLAND_LINKER_SCRIPT})
target_link_libraries(epolltest PRIVATE libmc libcenv)
