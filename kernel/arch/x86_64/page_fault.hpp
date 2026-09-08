// #PF(벡터 14) 핸들러 (docs/plan/system-servers-bringup.md §M12,
// ADR-016) — COW(Copy-on-Write) 쓰기 폴트만 다룬다. 그 외의 모든
// 폴트(진짜 권한 위반, not-present, 아직 없는 매핑 등)는 이 계획의
// 범위 밖(유저에게 SIGSEGV류를 전달하는 절차가 아직 없다) — idt.cpp가
// try_handle_cow_write_fault()가 false를 반환하면 M10의 catch-all
// 진단(diagnose_and_halt)으로 그대로 떨어뜨린다.
#pragma once

#include <cstdint>

namespace arch_x86_64 {

// 실제 판단/복사 로직 — 어느 주소공간(pml4_phys)에서 벌어진 폴트인지를
// 명시적으로 받는다(스케줄러 상태를 전혀 건드리지 않는다). idt.cpp가
// 부르는 아래의 try_handle_cow_write_fault()가 "현재 스레드의
// 주소공간"을 골라 넘겨주는 겉껍질일 뿐, 진짜 로직은 이 함수 하나뿐 —
// kernel_main.cpp의 데모가 실제 유저 스레드/CR3 전환 없이도 이 함수를
// 직접 불러 진짜 프로덕션 경로(프레임 재사용 vs 복사 분기)를 QEMU에서
// 검증할 수 있게 하기 위해서다.
//
// fault_addr: CR2(폴트가 난 가상주소). error_code: 하드웨어가 스택에
// 남긴 그대로(Intel SDM Vol.3 §4.7 표 4-16 — bit0 present, bit1 write,
// bit2 user, ...). 이 폴트를 COW로 처리했으면 true(그 즉시 같은
// 명령을 재실행해도 되는 상태)를 반환하고, 처리 대상이 아니면(COW
// 표시가 없거나 애초에 존재하지 않던 매핑 등) false를 반환해 호출자가
// 진짜 예외로 취급하게 한다.
bool try_handle_cow_write_fault_for(uint64_t pml4_phys, uint64_t fault_addr, uint64_t error_code);

// idt.cpp가 실제로 부르는 진입점 — sched::current()의 owner_space에서
// pml4_phys를 얻어 위 함수로 넘긴다. 커널 스레드(owner_space==nullptr)
// 라면 애초에 COW 대상 주소공간이 없으므로 false.
bool try_handle_cow_write_fault(uint64_t fault_addr, uint64_t error_code);

}  // namespace arch_x86_64
