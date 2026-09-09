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

    // M12(system-servers-bringup.md §M12, ADR-141) — 이 유저 스레드가
    // SYSCALL로 커널에 들어올 때 전환할 커널 스택의 top(create_user_thread가
    // 그 스레드의 커널 스택을 만들면서 미리 계산해 채워 둔다, 이후
    // 바뀌지 않는 고정값). M8은 이 값을 스레드마다 따로 두지 않고
    // 전역 스크래치 하나(g_syscall_kernel_rsp)로 관리했다 — 유저
    // 스레드가 정확히 하나뿐이고 그 스레드의 IPC Call도 블로킹 없이
    // 바로 응답이 오는 시나리오였던 M8 데모에서는 드러나지 않았지만,
    // 두 유저 스레드 중 하나가 IPC로 블록된 채 다른 하나가 syscall을
    // 걸면 전역 하나로는 두 스레드의 커널 스택이 서로를 덮어쓴다 —
    // fork()로 두 번째 유저 스레드가 실제로 생기기 전에 미리 고쳐
    // 둔다(scheduler.cpp가 이 스레드로 전환할 때마다 전역 g_syscall_kernel_rsp를
    // 이 값으로 동기화한다).
    uint64_t syscall_kernel_rsp = 0;

    // M9(ADR-127)는 이 자리에 512바이트(FXSAVE 전용) 배열을 직접
    // 내장했다. M11b(ADR-133, XSAVE/AVX)에서 1024바이트+alignas(64)로
    // 바꿔봤다가 실제로 QEMU에서 `#GP`로 깨지는 것을 확인했다 —
    // slab_alloc()이 보장하는 정렬은 16바이트뿐이다(ADR-134가 그
    // 정도로 고쳤을 뿐, 64바이트를 보장하지 않는다). `object::thread`
    // 자체가 slab에서 나오므로, 그 안의 배열에 아무리 `alignas(64)`를
    // 붙여도 실제 런타임 주소는 지켜지지 않는다. 그래서 ADR-138로
    // 이 필드를 "슬랩이 아니라 별도 페이지(mm::alloc_pages, 항상
    // 4096바이트 정렬)에서 나온 포인터"로 바꿨다 —
    // create_kernel_thread/create_user_thread(scheduler.cpp)가 그
    // 페이지를 0으로 채운 뒤 FCW/MXCSR 기본값을 patch한다. nullptr이면
    // 아직 할당되지 않은 상태(생성 실패 경로에서만 잠깐 존재).
    uint8_t* fpu_save_area = nullptr;

    // M14(ADR-154, OPEN-58 해소) — 이 스레드가 sys_io_activate로
    // 활성화해 둔 I/O 포트 범위. 둘 다 0이면 "활성 범위 없음"(모든
    // 포트 접근 거부). ADR-147 시절의 "TSS 하나에 전역으로 공유"를
    // 대체한다 — 컨텍스트 스위치마다 arch_sync_io_permission이 이
    // 필드를 읽어 TSS IOPB를 그 스레드 전용으로 재프로그래밍한다.
    // sys_fork(ADR-142)는 자식에게 이 값을 그대로 물려준다(trusted
    // 상속과 같은 정신) — 제한하는 fork 변형은 아직 없다(YAGNI,
    // ADR-154 §결정5).
    uint32_t io_port_base = 0;
    uint32_t io_port_count = 0;

    // ADR-159/161(kernel-ipc-objects.md, OPEN-61 해소) — 이 스레드가
    // IPC pages[]로 마지막으로 받은 매핑의 프레임 물리주소들. 고정
    // 슬롯(kernel/core/ipc/message.hpp::k_ipc_mapped_pages_user_vaddr)
    // 에 매핑돼 있다 — 배열 크기는 ipc::k_max_page_descriptors(=4)와
    // 같아야 하지만 object는 ipc를 몰라야 하므로(위 전방 선언 주석과
    // 같은 정신) 리터럴로 둔다. 이 스레드가 deliver_message의
    // 목적지로 다시 선택되는 시점 직전에 endpoint.cpp가 이 기록을
    // 읽어 이전 매핑을 frame_release한다.
    uint32_t ipc_mapped_page_count = 0;
    uint64_t ipc_mapped_frames[4] = {};
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
