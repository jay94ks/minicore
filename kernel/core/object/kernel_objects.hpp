// thread/address_space 커널 객체 (docs/spec/objects.md §7,
// docs/spec/scheduler.md §2). objects.md §7이 명시한 대로 이 헤더는
// 핸들 메커니즘이 아니라 두 객체의 필드 자체를 담는다 — 실제 값을
// 채우는 로직(레지스터 상태 저장, 페이지테이블 조작 등)은 이후
// 마일스톤(M5 스케줄러, M8 initrun)이 채운다.
#pragma once

#include <cstdint>

#include <libk/atomic.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/spinlock.hpp>

// ipc::message는 kernel/core/ipc(레이어상 object 위에 있음)가 정의한다 —
// 여기서는 포인터만 보관하므로 전방 선언으로 충분하다(ADR-002와 같은
// 정신의 계층 분리: object는 ipc를 몰라야 한다).
namespace ipc {
struct message;
}

namespace object {

class handle_table;  // handle_table.hpp(같은 namespace)가 정의 — thread가
                      // M8부터 자신의 핸들 테이블을 포인터로 들고 있어야
                      // 해서(유저 스레드의 syscall 진입점이 "현재 스레드의
                      // 테이블"을 찾아야 한다) 전방 선언만 둔다.

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

struct thread;  // ipc_state가 포인터로만 참조 — 아래 thread 정의보다 먼저 필요.

// M6(ipc.md §3/§5) — sys_call/sys_recv/sys_reply가 스레드별로 들고 있어야
// 하는 상태. sys_reply가 handle을 받지 않고 "가장 최근 sys_recv로 받은
// 호출"에 답하는 스펙 규칙(ipc.md §3) 자체가 이 상태를 요구한다.
struct ipc_state {
    const ipc::message* pending_call_msg = nullptr;  // sys_call이 서버 대기 중 blocked일 때: 보낼 메시지
    uint64_t pending_call_badge = 0;                 // 위와 짝 — 이 호출에 쓰인 handle의 badge
    ipc::message* recv_dest = nullptr;               // sys_recv가 caller 대기 중 blocked일 때: 받을 목적지
    uint64_t recv_badge = 0;                          // 위와 짝 — sys_recv가 반환할 badge
    ipc::message* reply_dest = nullptr;               // sys_call 완료 대기 중: 응답을 받을 목적지
    thread* reply_target = nullptr;                    // sys_recv로 받은 뒤: sys_reply가 깨울 대상
    uint32_t saved_boost_level = 0;                     // 도네이션 복원용(ADR-028)
};

struct thread {
    thread_sched_fields sched;
    address_space* owner_space = nullptr;
    list_hook run_queue_hook;  // scheduler.md의 run_queue(intrusive_list)가 M5부터 이 훅을 쓴다.
    list_hook ipc_wait_hook;   // endpoint의 대기열(M6, kernel/core/ipc)이 이 훅을 쓴다.
    ipc_state ipc;

    // 스레드가 실행 중이 아닐 때, 재개 시 이어서 실행할 지점의 스택
    // 포인터(M5, kernel/core/sched). 값의 실제 의미(스택에 무엇이 쌓여
    // 있는지)는 arch::context_switch(arch가 정의)만 알고 있다 — 이
    // 필드 자체는 "불투명한 재개 지점"으로만 다뤄 arch 독립을 유지한다.
    uint64_t context_rsp = 0;

    // M8(kernel-bootstrap.md, boot.md §4/§6) — 유저 스레드에만 의미
    // 있는 필드. owner_space가 nullptr이면(지금까지의 모든 커널
    // 스레드) 아래 필드는 전부 미사용이다.
    handle_table* handles = nullptr;  // syscall 진입 시 "이 스레드의 테이블"을 찾는 경로(syscall.cpp).
    uint64_t user_entry_rip = 0;      // 최초 유저모드 진입 시 RIP(ELF e_entry).
    uint64_t user_rsp = 0;            // 최초 유저모드 진입 시 RSP(유저 스택 top).
    uint64_t user_arg0 = 0;           // 최초 진입 시 RDI(boot.md §6 — "첫 인자" 관례. boot_info 등).

    // M9(ADR-127) — FXSAVE/FXRSTOR 대상 영역. FXSAVE는 16바이트 정렬을
    // 요구한다(정렬 안 된 주소로 실행하면 #GP). create_kernel_thread/
    // create_user_thread(scheduler.cpp)가 0으로 채운 뒤 FCW/MXCSR
    // 기본값을 patch한다 — 그 전까지는 내용이 정해지지 않은 상태다.
    alignas(16) uint8_t fxsave_area[512] = {};
};

// ipc.md §2 — Call/Reply가 오가는 대상. rights: CAN_SEND/CAN_RECV/
// CAN_MOVE/CAN_MAP(ADR-029). M6은 CAN_SEND/CAN_RECV만 실제로 검사한다 —
// CAN_MOVE/CAN_MAP은 페이지·핸들 전달이 생기는 M7에서 쓰인다.
constexpr uint32_t k_right_can_send = 1u << 0;
constexpr uint32_t k_right_can_recv = 1u << 1;
constexpr uint32_t k_right_can_move = 1u << 2;
constexpr uint32_t k_right_can_map = 1u << 3;

struct endpoint {
    spinlock lock;
    intrusive_list<thread, &thread::ipc_wait_hook> waiting_servers;  // sys_recv 대기 중
    intrusive_list<thread, &thread::ipc_wait_hook> waiting_callers;  // sys_call 대기 중(서버가 아직 없음)
};

// ipc.md §7 — 64비트 대기 중 비트셋 하나. sys_notify는 OR할 뿐 절대
// 실패하지 않는다(큐잉·버퍼링 없음) — sys_wait로 아직 아무도 기다리지
// 않아도 비트는 유지된다. waiter는 최대 하나만 추적한다 — 스펙이 여러
// 스레드의 동시 sys_wait를 다루지 않는다(ipc.md "아직 정하지 않은 것"
// 참고, 그건 sys_recv의 다중 클라이언트 이야기이지 notification은
// 아니다 — 최소 구현에서는 단일 대기자로 충분하다고 판단했다).
struct notification {
    spinlock lock;  // waiter 필드 보호(ADR-033)
    atomic<uint64_t> bits{0};
    thread* waiter = nullptr;
};

}  // namespace object
