# 커널이 직접 기동하는 최초 유저 프로세스 (ADR-017). 골격 단계.
# 부팅 후 기동 매니페스트(형식 미정, OPEN-19)에 따라 procsrv 등 코어 서버를 실행한다.
#
# 소스가 추가되면 add_executable로 전환한다.

add_library(minicore_initrun INTERFACE)
