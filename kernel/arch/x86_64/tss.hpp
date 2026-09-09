// TSS(Task State Segment) 최소 설정 — RSP0만 채운다 (docs/plan/
// system-servers-bringup.md §M12, ADR-143). usermode.S가 M8 시점에
// 이미 "TSS(RSP0)는 ring3→ring0 방향(인터럽트로 되돌아올 때)에만
// 필요하다"고 정확히 지적해 뒀지만, 그 방향(유저모드에서 실제로
// 예외/인터럽트가 발생하는 경우)이 M8~M11엔 한 번도 일어나지 않아
// (모든 예외 테스트가 커널 스레드=ring0에서만 있었다) 미뤄 둘 수
// 있었다. M12의 COW 쓰기 폴트가 실제 유저 스레드(ring3)에서 처음으로
// 발생하면서 더 이상 미룰 수 없게 됐다 — TSS.RSP0가 없으면(또는
// GDT에 TSS 디스크립터 자체가 없으면) CPU가 ring3→ring0 전환 시
// 어느 커널 스택으로 갈지 알 수 없어 그 자체로 결함(#GP/#DF, 최악의
// 경우 트리플 폴트)이 난다.
#pragma once

namespace arch_x86_64 {

// GDT를 TSS 디스크립터(셀렉터 0x38)를 포함한 확장판으로 다시 구성하고
// LTR로 적재한다. mm::init() 이후, 첫 유저 스레드가 뜨기 전에 호출해야
// 한다(install_syscall_entry()와 같은 시점 — kernel_main.cpp 참고).
void init_tss();

}  // namespace arch_x86_64
