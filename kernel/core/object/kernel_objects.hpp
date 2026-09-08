// thread/address_space 커널 객체 (docs/spec/objects.md §7,
// docs/spec/scheduler.md §2). objects.md §7이 명시한 대로 이 헤더는
// 핸들 메커니즘이 아니라 두 객체의 필드 자체를 담는다 — 실제 값을
// 채우는 로직(레지스터 상태 저장, 페이지테이블 조작 등)은 이후
// 마일스톤(M5 스케줄러, M8 initrun)이 채운다.
#pragma once

#include <cstdint>

#include <libk/intrusive_list.hpp>

namespace object {

// ADR-085. jail/guest 격리 메커니즘 자체(§2.2가 말하는 실제 정책 적용)는
// security-model.md 영역이라 M1~M8 범위 밖이다 — 여기서는 값만 보관한다.
enum class confinement_tier : uint8_t { normal = 0, guest = 1, jail = 2 };

struct address_space {
    bool trusted = false;                                // ADR-063
    confinement_tier confinement = confinement_tier::normal;  // ADR-085
    uint64_t page_table_root = 0;  // arch별 최상위 페이지테이블 물리주소(x86_64는 PML4)
};

// scheduler.md §2 그대로 — band/preferred_node/boost_level/타임슬라이스.
// M4는 필드만 정의한다: 실제로 스케줄링에 쓰이는 것은 M5(run_queue)부터다.
enum class priority_band : uint32_t { kernel = 0, user = 1 };

struct thread_sched_fields {
    priority_band band = priority_band::user;
    uint32_t preferred_node = 0;  // ADR-034/036. 기본값: 부모 스레드의 노드 상속(M5+에서 실제 적용)
    uint32_t boost_level = 0;     // 0 = 기본(ADR-025/027)
    uint64_t base_time_slice_us = 0;
};

struct thread {
    thread_sched_fields sched;
    address_space* owner_space = nullptr;
    list_hook run_queue_hook;  // scheduler.md의 run_queue(intrusive_list)가 M5부터 이 훅을 쓴다.

    // 스레드가 실행 중이 아닐 때, 재개 시 이어서 실행할 지점의 스택
    // 포인터(M5, kernel/core/sched). 값의 실제 의미(스택에 무엇이 쌓여
    // 있는지)는 arch::context_switch(arch가 정의)만 알고 있다 — 이
    // 필드 자체는 "불투명한 재개 지점"으로만 다뤄 arch 독립을 유지한다.
    uint64_t context_rsp = 0;
};

}  // namespace object
