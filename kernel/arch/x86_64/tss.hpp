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

#include <cstdint>

namespace arch_x86_64 {

// GDT를 TSS 디스크립터(셀렉터 0x38)를 포함한 확장판으로 다시 구성하고
// LTR로 적재한다. mm::init() 이후, 첫 유저 스레드가 뜨기 전에 호출해야
// 한다(install_syscall_entry()와 같은 시점 — kernel_main.cpp 참고).
void init_tss();

// M12(system-servers-bringup.md §M12, ADR-147) — [io_base, io_base+count)
// 포트 범위를 ring3에서 inb/outb 등으로 직접 접근 가능하게 IOPB
// 비트를 0(허용)으로 지운다. init_tss() 이후에만 호출 가능하다.
// **알려진 단순화**: IOPB는 TSS 하나에 전역으로 공유된다(코어당 TSS
// 하나, 스레드별로 나누지 않는다) — 지금은 유일한 트러스트 프로세스
// (initrun)만 존재해 문제가 없지만, 신뢰하지 않는 유저 프로세스가
// 생기는 시점에는 이 접근이 그 프로세스에게도 그대로 열려 있다는
// 뜻이다(재검토 필요, docs/design/open-items.md에 등록).
void grant_io_port_range(uint16_t io_base, uint16_t count);

}  // namespace arch_x86_64
