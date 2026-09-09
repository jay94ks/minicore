// 커널과 유저모드(초기에는 initrun만)가 공유하는 최소 syscall ABI
// (docs/plan/kernel-bootstrap.md M8, docs/spec/boot.md §6). repo-layout.md가
// "유저랜드와 공유하는 커널 ABI 헤더"라고 정의한 자리가 kernel/include다.
//
// message는 kernel/core/ipc/message.hpp의 ipc::message와 **필드 순서·
// 타입이 완전히 동일**해야 한다 — 유저(initrun)와 커널이 서로 다른
// 컴파일 단위(별도 실행파일)에서 각자 이 레이아웃을 알고 있어야 하기
// 때문에, kernel-internal 헤더를 유저 실행파일에 직접 include하는 대신
// 이렇게 ABI 전용으로 복제해 둔다(실제 커널들이 uapi 헤더를 이렇게
// 분리해 쓰는 것과 같은 이유). ipc::message가 바뀌면 이 파일도 함께
// 갱신해야 한다.
#pragma once

#include <cstdint>

namespace uapi {

// syscall 번호. RDI(1번 인자)에 싣는다 — syscall.S/syscall.cpp 참고.
inline constexpr uint64_t k_syscall_ipc_call = 0;  // ipc::sys_call 그대로 노출.

// M12(system-servers-bringup.md §M12, ADR-142) — ADR-131이 이름만
// 정해 둔 `sys_process_spawn`과, procsrv가 자기 자신을 fork/exec하는
// 데 필요한 `sys_fork`/`sys_exec`. 셋 다 새 프로세스/실행 이미지
// 교체를 다루므로 인자가 IPC보다 많아 구조체 포인터(a1)로 묶어
// 전달한다(ipc_call이 msg_in/msg_out 포인터를 그대로 넘기는 것과
// 같은 관례).
inline constexpr uint64_t k_syscall_process_spawn = 1;
inline constexpr uint64_t k_syscall_fork = 2;
inline constexpr uint64_t k_syscall_exec = 3;

// sys_process_spawn(a1 = 이 구조체의 유저 가상주소, a2/a3 미사용) —
// elf_data/argv_blob는 **호출자 자신의 주소공간**에 있는 유저
// 가상주소다(커널이 아직 호출자의 CR3로 실행 중인 시점에 읽으므로
// 그대로 역참조 가능 — ipc_call의 msg_in/msg_out과 같은 전제).
// argv_blob==0이면 인자 없음. 반환값 0=성공, 그 외
// process_spawn_error(process_ops.hpp)의 값.
struct process_spawn_request {
    uint64_t elf_data = 0;
    uint64_t elf_size = 0;
    uint64_t argv_blob = 0;  // NUL로 구분된 문자열들이 이어진 블록, 마지막도 NUL.
    uint64_t argv_size = 0;
    bool grant_trusted = false;
};

// sys_exec(a1 = 이 구조체의 유저 가상주소, a2/a3 미사용) — 성공하면
// 반환하지 않는다(procsrv.md §4 6단계와 동일한 관례: 새 실행 이미지가
// 이미 실행을 시작했으므로). 실패해야만 syscall이 정상적으로
// 반환되고, 반환값은 process_spawn_error 값이다.
struct exec_request {
    uint64_t elf_data = 0;
    uint64_t elf_size = 0;
    uint64_t argv_blob = 0;
    uint64_t argv_size = 0;
};

// sys_fork(인자 없음) — 반환값: 자식 프로세스에서는 0, 부모에서는
// 1(성공) 또는 process_spawn_error(실패). 커널은 pid를 모르므로(procsrv
// 소관, procsrv.md §2) "부모냐 자식이냐"만 구분하는 최소 신호다.

// M12(ADR-142) — 협조적 스케줄러에서 유저 스레드가 스스로 CPU를
// 물러나려면(예: fork()/sys_process_spawn()으로 만든 새 스레드에게
// 기회를 주기 위해) 최소한 하나의 자발적 양보 수단이 필요하다.
// sys_thread_exit(인자 없음)은 절대 반환하지 않는다(sched::exit()
// 그대로 노출) — 아직 sys_yield는 없다(이 스레드가 나중에 다시
// 실행될 필요가 없는 경우에만 쓸 수 있다).
inline constexpr uint64_t k_syscall_thread_exit = 4;

// M12 self-test 임시 배선 — kernel_main.cpp::setup_initrun_process가
// initrun 자신의 원본 ELF 바이트를(자기 자신을 fork/process_spawn/exec으로
// 다시 만들어 볼 수 있게) initrun의 주소공간에도 매핑해 두고, 그
// 위치/크기를 이 고정 주소의 작은 구조체에 써 둔다. **procsrv/cpio가
// 실제로 생기면 이 배선은 통째로 제거된다** — 지금은 M12의 커널
// 프리미티브(fork/process_spawn/exec)를 실제 userland 코드로 검증할
// 방법이 이것뿐이라 임시로 둔다.
inline constexpr uint64_t k_m12_self_elf_user_vaddr = 0x0000700000003000ull;
inline constexpr uint64_t k_m12_self_info_user_vaddr = 0x0000700000010000ull;
struct m12_self_info {
    uint64_t elf_addr = 0;
    uint64_t elf_size = 0;
};


inline constexpr uint32_t k_message_registers = 4;
inline constexpr uint32_t k_max_page_descriptors = 4;
inline constexpr uint32_t k_max_handle_transfers = 2;

enum class transfer_mode : uint8_t {
    copy = 0,
    move = 1,
    map = 2,
};

struct page_descriptor {
    uint64_t vaddr = 0;
    uint64_t length = 0;
    transfer_mode mode = transfer_mode::copy;
};

struct handle_transfer {
    uint32_t src_handle = 0;
    uint32_t rights_mask = 0;
};

// ipc::message와 바이트 단위로 동일한 레이아웃 — kernel/core/ipc/message.hpp
// 상단 주석 참고(pages[]/handles[]의 방향 규약도 그대로 적용된다).
struct message {
    uint32_t label = 0;
    uint32_t page_count = 0;
    uint32_t handle_count = 0;
    uint64_t regs[k_message_registers] = {};
    page_descriptor pages[k_max_page_descriptors] = {};
    handle_transfer handles[k_max_handle_transfers] = {};
};

}  // namespace uapi
