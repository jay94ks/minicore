# libcpio 유저랜드 측 - minicore/libs/libcpio/cpio.cpp를 그대로
# 재사용한다(RM-7C249618 - "커널/유저 공용(표준 C++ 내장 타입만 사용)"
# 으로 이미 설계돼 있었으나 지금까지 유저랜드 빌드 트리에는 실제로
# 연결된 적이 없었다 - PN-012D6310이 첫 유저랜드 소비자). out-of-tree
# 소스 재사용(minicore/init 등이 이 userland 빌드 트리를 재사용하는
# 것과 정반대 방향 - 여기서는 이 트리가 minicore/libs 쪽 소스를
# 가져온다).
add_library(libcpio OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/../../../minicore/libs/libcpio/cpio.cpp)

target_include_directories(libcpio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../../minicore/libs)
