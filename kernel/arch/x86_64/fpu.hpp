// x86_64 FPU 활성화(M9, ADR-127). FXSAVE/FXRSTOR를 쓰려면 CR4.OSFXSR가
// 설정돼 있어야 한다(안 그러면 #UD) — kernel_main이 스케줄러를 시작하기
// 전, 즉 첫 arch_context_switch(=첫 FXSAVE/FXRSTOR)보다 먼저 반드시
// 호출해야 한다.
#pragma once

namespace arch_x86_64 {

void init_fpu();

}  // namespace arch_x86_64
