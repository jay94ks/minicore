// 새 프로세스 생성/복제/실행 이미지 교체 (docs/plan/system-servers-bringup.md
// §M12, ADR-142). ADR-131이 이름만 정해 둔 `sys_process_spawn`과,
// procsrv가 자기 자신을 fork/exec하는 데 필요한 sys_fork/sys_exec의
// 커널 측 구현. 셋 다 kernel/arch/x86_64에 두는 이유: create_address_space_root/
// load_elf/map_page(전부 arch) + create_user_thread/create_forked_thread
// (core/sched) + create_handle_table(core/object)을 모두 조합해야 하는
// 조립 코드라, kernel_main.cpp::setup_initrun_process(M8)가 이미 같은
// 위치에서 같은 조합을 해 온 것과 같은 자리다.
#pragma once

#include <cstdint>

#include <mc/syscall.h>

namespace kern::object {
class handle_table;  // core/object/handle_table.hpp — 전방 선언만 필요(ADR-002와 같은 정신, tss.hpp의 thread 전방 선언과 동일한 관례).
}

namespace kern::arch::x86_64 {

enum class process_spawn_error : uint32_t {
    ok = 0,
    not_a_user_process,  // 커널 스레드(owner_space==nullptr)가 시도함.
    elf_load_failed,
    out_of_memory,
    invalid_argument,  // argv_size가 한 페이지보다 큼(M12는 이 이상을 다루지 않음).
    // ADR-160(kernel-memory.md) — 가변 크기 페이로드가 캐패빌리티
    // 슬롯 예산(k_capability_slot_size=1MiB)을 넘으려 함(지금은
    // self_elf 슬롯뿐). 이웃 슬롯과 충돌해 already_mapped로 우회
    // 발견되던 것을 명시적 오류로 바꾼다.
    capability_slot_overflow,
};

// sys_process_spawn — elf_data[0..elf_size)를 완전히 새 주소공간에
// 적재해 새 유저 스레드로 기동한다(enqueue까지 마친다). elf_data/
// argv_blob는 **호출자 자신의 주소공간**에 있는 유저 가상주소다 —
// 아직 호출자의 CR3가 살아있는 시점에 그대로 역참조한다(ipc_call의
// msg_in/msg_out과 같은 전제). argv_blob==nullptr(argv_size==0)이면
// 인자 없음. grant_trusted: ADR-074의 "부여 권한" — fork()와 달리
// 완전히 새 신원을 시작하는 연산이므로 여기서 처음 정해진다.
//
// M13(ADR-151) — create_endpoint/inherited_handles/inherited_handle_count는
// mc_process_spawn_request와 정확히 같은 의미(그 파일 상단 주석
// 참고) — 스폰 시점 캐패빌리티 주입. 성공하고 create_endpoint가
// true면 out_endpoint_proxy_handle에 **호출자 자신의** handle_table에
// 새로 생긴 프록시 핸들을 채운다.
// M22(general-purpose-completion.md §M22, ADR-178) — 성공하면
// out_thread_handle에도 새 스레드를 가리키는 object_kind::thread
// 소유 핸들(k_right_can_kill만 부여)을 호출자 자신의 handle_table에
// 만들어 채운다(out_endpoint_proxy_handle과 같은 "호출자 소유" 자리).
// 호출자가 커널 스레드(handles==nullptr)면 조용히 건너뛴다(0 유지).
process_spawn_error process_spawn(const uint8_t* elf_data, uint64_t elf_size,
                                   const uint8_t* argv_blob, uint64_t argv_size,
                                   bool grant_trusted, bool create_endpoint,
                                   const mc_handle_transfer* inherited_handles,
                                   uint32_t inherited_handle_count,
                                   uint32_t& out_endpoint_proxy_handle,
                                   uint32_t& out_thread_handle);

enum class process_kill_error : uint32_t {
    ok = 0,
    invalid_handle,     // 핸들이 없거나 이미 닫힘.
    wrong_object_type,  // object_kind::thread가 아님.
    permission_denied,  // k_right_can_kill 없음.
};

// sys_process_kill — h(호출자 자신의 handle_table 안, object_kind::thread,
// k_right_can_kill 필요)가 가리키는 스레드에게 강제 종료를 요청한다
// (kern::sched::request_kill, kernel/core/sched/scheduler.hpp 참고 — 실제
// 폐기는 즉시가 아니라 그 스레드가 다음에 스케줄러에 뽑히려는
// 시점이다).
process_kill_error process_kill(kern::object::handle_table& caller_handles, uint32_t h);

// sys_fork — 호출자의 주소공간을 COW로 복제해 새 프로세스를 만든다
// (ADR-016/140). 반환값은 **부모 관점의 syscall 반환값**이다: 1=성공,
// 그 외는 process_spawn_error 값. 자식은 이 함수를 통해 반환받지
// 않는다(create_forked_thread가 만든 스레드가 arch_fork_child_resume
// 으로 직접 진입해, syscall_dispatch에게는 RAX=0으로만 보인다).
// fd 테이블 복제는 커널이 하지 않는다(procsrv.md §3.6 — procsrv 자신이
// IPC로 각 소유 서버에 요청할 몫) — 자식은 빈 handle_table로
// 시작한다.
uint64_t fork_current(uint64_t saved_user_rip, uint64_t saved_user_rflags,
                       uint64_t saved_user_rsp, uint64_t saved_rbx, uint64_t saved_rbp,
                       uint64_t saved_r12, uint64_t saved_r13, uint64_t saved_r14,
                       uint64_t saved_r15);

// sys_exec — 호출한 스레드의 실행 이미지를 완전히 새 주소공간+ELF로
// 교체한다. pid/identity에 해당하는 어떤 것도 이 계층에는 없으므로
// (procsrv 소관) 그냥 "이 스레드가 앞으로 실행할 코드"만 바뀐다.
// 성공하면 이 함수는 반환하지 않는다(usermode로 직접 진입,
// procsrv.md §4 6단계와 동일한 관례) — 실패했을 때만 값을 반환한다.
// **알려진 단순화**(M12 범위): 이전 주소공간의 페이지테이블/프레임은
// 회수하지 않고 그대로 버려둔다(누수) — 실제 회수는 이후 마일스톤.
process_spawn_error exec_current(const uint8_t* elf_data, uint64_t elf_size,
                                  const uint8_t* argv_blob, uint64_t argv_size);

// sys_alloc_dma_buffer(ADR-147) — 물리적으로 연속인 4KiB<<order 바이트를
// 확보해 호출자의 주소공간에 매핑하고, 그 가상주소와 물리주소를 모두
// out_virt_addr/out_phys_addr에 채운다. **trusted 프로세스만** 쓸 수
// 있다 — 물리주소를 유저에게 그대로 알려주는 것 자체가 격리를
// 우회하는 능력이라, "이 프로세스가 실제로 하드웨어(virtio-blk)를
// 직접 다뤄야 한다"는 사실이 이미 확인된 신원(ADR-074)에게만
// 내준다. 이 프로세스 안에서 딱 하나만 있으면 충분하므로(부트
// 디바이스 클라이언트 하나가 vring+I/O 버퍼를 전부 여기 담는다)
// 고정 가상주소(k_dma_buffer_user_vaddr, process_ops.cpp)에 매핑한다
// — 두 번째 호출은 그 매핑을 그대로 덮어써 버리므로 호출자가
// 한 번만 쓴다고 가정한다.
process_spawn_error alloc_dma_buffer(uint32_t order, uint64_t& out_virt_addr,
                                      uint64_t& out_phys_addr);

// sys_map_phys(M14, ADR-007/038/039가 예고한 "MMIO 캐패빌리티") —
// 호출자가 알고 있는 임의의 물리주소 범위(하드웨어 소유, 페이지
// 할당자가 모르는 영역 — ACPI 테이블, PCIe ECAM, 디바이스 BAR 등)를
// 그대로 자기 주소공간에 매핑한다. alloc_dma_buffer와 달리 새 메모리를
// 확보하지 않으므로 frame 참조 카운트를 건드리지 않는다. trusted
// 프로세스만 쓸 수 있다(alloc_dma_buffer와 같은 이유). phys_addr은
// 페이지 정렬 불필요 — 이 함수가 내림/올림을 처리하고, out_virt_addr에
// phys_addr의 페이지 내 오프셋까지 보정된 값을 채운다. 고정 가상주소
// 슬롯 하나를 재사용한다(alloc_dma_buffer와 같은 단순화 — 동시에 두
// 매핑이 필요 없는 순차적 사용만 가정, process_ops.cpp 참고).
process_spawn_error map_phys(uint64_t phys_addr, uint64_t size, uint64_t& out_virt_addr);

// sys_io_activate/sys_io_deactivate(ADR-154, OPEN-58 해소) — 호출한
// 스레드 자신의 활성 I/O 포트 범위(kern::object::thread::io_port_base/count)
// 를 설정/해제하고, 지금 실행 중인 스레드이므로 TSS IOPB에도 즉시
// 반영한다(kern::arch::x86_64::sync_io_permission을 직접 부른다 — 다음
// 컨텍스트 스위치까지 기다리지 않는다). trusted 프로세스만 쓸 수
// 있다(alloc_dma_buffer/map_phys와 같은 이유 — I/O 포트 직접 접근도
// 격리를 우회하는 능력이다).
process_spawn_error io_activate(uint16_t io_base, uint16_t count);
process_spawn_error io_deactivate();

// sys_brk(general-purpose-completion.md §M24, ADR-180) — 호출한
// 프로세스의 힙(익명 페이지)을 늘린다. increment==0은 조회
// (out_old_top에 현재 heap_top만 채운다). increment<0은
// invalid_argument(축소 미지원, 이번 라운드 범위 밖). 성공하면
// out_old_top에 증가 **전** heap_top(고전적 sbrk() 관례 — 새로
// 확보된 영역의 시작 주소)을 채운다. 고정 가상주소 슬롯 하나
// (1MiB, kernel-memory.md ADR-160 슬롯 5)를 예산으로 쓴다 — 그
// 이상을 요청하면 out_of_memory.
process_spawn_error brk(int64_t increment, uint64_t& out_old_top);

}  // namespace kern::arch::x86_64
