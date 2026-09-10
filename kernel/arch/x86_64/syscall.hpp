// SYSCALL/SYSRET 진입 설정 (docs/plan/kernel-bootstrap.md M8). 유저모드
// 진입을 시작하기 전에 한 번 호출해야 한다 — syscall_entry.S/syscall.cpp
// 참고.
//
// M34(real-libc-syscall-layer.md §M34, ADR-185) — 이제 코어마다 호출
// 해야 한다(BSP+각 AP, init_idt()/init_fpu()/lapic_enable_this_core()
// 와 같은 "코어별 MSR·레지스터 상태" 패턴). kernel_gs_base_slot은 이
// 코어 전용 g_syscall_kernel_rsp[] 슬롯의 주소 — IA32_KERNEL_GS_BASE
// MSR에 그대로 심어 syscall_entry.S가 swapgs+`%gs:0`으로 그 슬롯을
// 코어별로 정확히 찾아가게 한다(다른 코어의 슬롯을 건드릴 방법이
// 원천적으로 없다 — 매핑이 아니라 MSR 값 자체가 코어별이기 때문).
#pragma once

#include <cstdint>

namespace kern::arch::x86_64 {

void install_syscall_entry(uint64_t kernel_gs_base_slot);

}  // namespace kern::arch::x86_64
