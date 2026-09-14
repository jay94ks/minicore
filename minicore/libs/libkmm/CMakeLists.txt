# Slab 할당자(SP-8B6B8D25 §3.1 - "메모리 관리자 구현 중 재사용 가능한
# 코드만 모은 라이브러리"). Bonwick 매거진/디포 모델(SP-D7013B26)이
# PreemptionGuard(scheduler.h)/PageFrameAllocator/direct map(kPhysToVirt)
# 등 커널 서비스에 직접 의존하므로(설계 문서의 코드 스케치 자체가 이미
# 그렇게 되어 있음 - 별도 추상화 계층 없이 커널 API를 바로 호출), 다른
# libs 모듈(libcpio 등)과 달리 minicore/kernel 헤더를 직접 include한다.
add_library(libkmm OBJECT
    slab.cpp
)

target_include_directories(libkmm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_SOURCE_DIR}/minicore/kernel)
