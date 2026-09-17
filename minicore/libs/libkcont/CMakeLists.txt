# libkcont - 커널/유저 공용 제네릭 컨테이너 템플릿 라이브러리
# (SP-FAF768AB, PN-633BF2D8) - Node/List/Vector/Rbtree/RbMultiTree/
# Map/OrderedList/LruList/Queue. 순수 헤더(템플릿) 전용이라 컴파일할
# .cpp가 없으므로, libkenv/libcpio 등 기존 OBJECT 라이브러리들과
# 달리 INTERFACE 라이브러리로 둔다(실제 오브젝트 코드가 없음 - 이
# 라이브러리를 링크하는 타깃에 include 경로만 전파).
add_library(libkcont INTERFACE)

# "libkcont/vector.h"처럼 라이브러리 이름을 포함한 경로로 include하게
# 하려고, libkcont 디렉터리 자체가 아니라 그 부모(minicore/libs)를
# public include 경로로 노출한다(libkenv와 동일한 관례).
target_include_directories(libkcont INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
