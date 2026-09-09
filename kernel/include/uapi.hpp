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

// ipc::message와 바이트 단위로 동일한 레이아웃이어야 하는 보조 타입들
// (kernel/core/ipc/message.hpp 상단 주석 참고) — process_spawn_request
// (M13부터 handle_transfer를 스폰 시점 캐패빌리티 주입에도 재사용한다,
// ADR-151)와 message 양쪽이 이 타입들을 먼저 필요로 하므로 파일
// 앞부분으로 옮겨 둔다.
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

// object::k_right_can_*(kernel/core/object/kernel_objects.hpp)와 정확히
// 같은 비트값 — handle_transfer.rights_mask에 넣을 값을 유저 코드가
// 매직 넘버 없이 쓸 수 있게 노출한다(M13부터 vfs/procsrv 등이 실제로
// endpoint 프록시를 만들며 이 마스크를 지정해야 한다).
inline constexpr uint32_t k_right_can_send = 1u << 0;
inline constexpr uint32_t k_right_can_recv = 1u << 1;
inline constexpr uint32_t k_right_can_move = 1u << 2;
inline constexpr uint32_t k_right_can_map = 1u << 3;

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
// M13(system-servers-bringup.md §M13, ADR-151) — 새 프로세스가 다른
// 서버를 IPC로 부르려면 그 서버의 endpoint에 대한 핸들을 스폰
// 시점에 미리 쥐고 있어야 한다(등록/탐색 서비스가 아직 없다, OPEN-59
// 참고) — 그래서 스폰 자체가 캐패빌리티 주입 지점이 된다.
inline constexpr uint32_t k_max_spawn_inherited_handles = 4;

struct process_spawn_request {
    uint64_t elf_data = 0;
    uint64_t elf_size = 0;
    uint64_t argv_blob = 0;  // NUL로 구분된 문자열들이 이어진 블록, 마지막도 NUL.
    uint64_t argv_size = 0;
    bool grant_trusted = false;

    // true면 새 프로세스의 handle_table에 handle 1로 새 IPC endpoint
    // 소유 핸들을 만들어 준다(sys_ipc_recv로 그 위에서 받을 수 있게) —
    // handle 1 관례는 kernel_main.cpp::setup_initrun_process(M8)의
    // boot endpoint 배선과 정확히 같다. 성공하면 그 endpoint에 대한
    // 프록시 핸들(CAN_SEND만) 하나를 **호출자 자신의** handle_table에도
    // 만들어 out_endpoint_proxy_handle에 채운다 — 호출자(대개 initrun)가
    // 이 프록시를 나중에 스폰하는 다른 프로세스에게 inherited_handles로
    // 넘겨줘야, 그 프로세스가 지금 만든 새 프로세스를 호출할 수 있다.
    bool create_endpoint = false;
    uint32_t out_endpoint_proxy_handle = 0;  // 출력.

    // 호출자가 이미 들고 있는 핸들(대개 다른 서비스의 endpoint 프록시)을
    // 새 프로세스의 handle_table에 순서대로(create_endpoint가 handle 1을
    // 차지했다면 2부터) 미리 넣어 둔다 — src_handle은 호출자 자신의
    // handle_table 안 번호, rights_mask는 위임 시 적용할 축소 마스크
    // (ADR-029). 아직 새 프로세스가 존재하지 않는 시점에 하는 일이라
    // IPC 메시지가 아니라 이 구조체로 직접 지정한다.
    uint32_t inherited_handle_count = 0;
    handle_transfer inherited_handles[k_max_spawn_inherited_handles] = {};
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

// M12(ADR-147) — initrun의 임베디드 virtio-blk 클라이언트가 vring(디스크립터
// 테이블+avail/used 링)과 I/O 요청 버퍼에 쓸 물리적으로 연속인 메모리가
// 필요하다 — 디바이스가 DMA로 직접 읽는 물리주소를 알아야 하므로 보통의
// map_page류 매핑(가상주소만 노출)으로는 부족하다. trusted 프로세스
// (initrun)만 쓸 수 있다(process_ops.hpp 참고 — 물리주소 노출 자체가
// 격리를 우회하는 능력이라 신뢰 여부로 막는다).
// a1 = 이 구조체의 유저 가상주소(출력), a2 = order(4KiB<<order 바이트).
inline constexpr uint64_t k_syscall_alloc_dma_buffer = 5;

struct dma_buffer_result {
    uint64_t virt_addr = 0;
    uint64_t phys_addr = 0;
};

// M13(system-servers-bringup.md §M13, ADR-151) — vfs/memfs 같은 실제
// 서버가 유저모드에서 IPC 호출을 **받고 응답**하려면 k_syscall_ipc_call
// (클라이언트 전용)만으로는 부족하다 — ipc::sys_recv/sys_reply를 그대로
// syscall로 노출한다.
//
// sys_ipc_recv(a1=handle, a2=이 메시지의 유저 가상주소, a3 미사용) —
// 블록. 반환값 0=ipc_error::ok(그 외는 ipc::ipc_error 값). badge는
// 이 마일스톤에서 아직 쓰이지 않아 반환하지 않는다(다중 클라이언트
// 구분이 필요해지면 이후 재검토, kernel/core/ipc/endpoint.hpp의 badge
// 주석 참고 — 소유 핸들로 받으면 항상 0이라 M13의 단일 클라이언트
// 시나리오에는 의미가 없다).
inline constexpr uint64_t k_syscall_ipc_recv = 6;

// sys_ipc_reply(a1=이 메시지의 유저 가상주소, a2/a3 미사용) — 가장
// 최근 sys_ipc_recv로 받은 호출에 응답한다. 블록하지 않는다. 반환값
// 0=ipc_error::ok.
inline constexpr uint64_t k_syscall_ipc_reply = 7;

// M13(system-servers-bringup.md §M13) — vfs/memfs/procsrv 여러 유저
// 프로세스가 협력하는 시나리오를 klog(커널 전용 API, 유저에겐 노출된
// 적이 없다)로 직접 관찰할 방법이 없다 — M8의 boot IPC call은 그
// 자체가 관찰 수단이었지만 그건 initrun 전용 1회성 채널(커널 스레드가
// 받은 뒤 곧바로 종료)이라 재사용할 수 없다. 그래서 최소한의 디버그
// 로그 syscall을 하나 둔다 — a1=ASCII 바이트 포인터(유저 주소공간,
// NUL 불필요), a2=길이(k_max_debug_log_bytes로 잘림). 반환값 항상 0.
// 프로덕션 API가 아니라 순수 진단용이다(procsrv.md의 실제 로깅
// 서비스가 생기면 대체될 임시 수단).
inline constexpr uint64_t k_syscall_debug_log = 8;
inline constexpr uint32_t k_max_debug_log_bytes = 96;

// M12 self-test 임시 배선 — kernel_main.cpp::setup_initrun_process가
// initrun 자신의 원본 ELF 바이트를(자기 자신을 fork/process_spawn/exec으로
// 다시 만들어 볼 수 있게) initrun의 주소공간에도 매핑해 두고, 그
// 위치/크기를 이 고정 주소의 작은 구조체에 써 둔다. **procsrv/cpio가
// 실제로 생기면 이 배선은 통째로 제거된다** — 지금은 M12의 커널
// 프리미티브(fork/process_spawn/exec)를 실제 userland 코드로 검증할
// 방법이 이것뿐이라 임시로 둔다.
// self_elf_user_vaddr부터 self_info_user_vaddr 전까지가 원본 ELF
// 바이트를 담을 수 있는 최대 크기다(0xFD000=약 1MiB) — initrun.elf가
// virtio_blk.cpp 추가로 0xcc50→0xf270바이트로 커지면서 이전 간격
// (0xD000=약52KiB)을 실제로 넘어서 self_info 페이지 매핑이
// already_mapped로 깨지는 걸 겪었다(2026-09-09) — 앞으로도 계속
// 커질 걸 감안해 여유를 크게 둔다.
inline constexpr uint64_t k_m12_self_elf_user_vaddr = 0x0000700000003000ull;
inline constexpr uint64_t k_m12_self_info_user_vaddr = 0x0000700000100000ull;
struct m12_self_info {
    uint64_t elf_addr = 0;
    uint64_t elf_size = 0;
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
