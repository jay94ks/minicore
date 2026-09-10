// x86_64 FPU 활성화 + lazy 전환 (M9 ADR-127 → M11b ADR-133).
//
// M9는 eager 정책(매 컨텍스트 스위치마다 무조건 FXSAVE/FXRSTOR)이었다.
// M11b부터는 lazy 정책이다 — arch_context_switch(context_switch.S)는
// 스위치마다 CR0.TS만 세우고, 실제 레지스터 상태 저장/복원은 그
// 소유자가 바뀔 때만 `#NM` 트랩(이 파일의 arch_x86_64_handle_nm_trap,
// idt.cpp가 벡터 7로 라우팅)에서 일어난다.
#pragma once

namespace arch_x86_64 {

// 부팅 극초기(BSP)와 각 AP 진입 시(ap_main) 반드시 호출해야 한다 —
// CR0/CR4/XCR0은 코어별 상태라 코어마다 각자 설정해야 한다. CPUID로
// XSAVE(bit26)·AVX(bit28) 지원 여부를 검사해:
//   - 둘 다 있으면: CR4.OSXSAVE 설정 → XSETBV로 XCR0에 x87/SSE/AVX
//     활성화 → CPUID leaf 0xD로 필요한 저장 영역 크기를 얻어 이후
//     XSAVE/XRSTOR 경로를 쓴다(1024바이트 상한 내인지 assert).
//   - 하나라도 없으면: 기존 FXSAVE/FXRSTOR·512바이트 경로로 폴백한다.
// 여러 코어에서 반복 호출해도 안전하다(멱등 — 매번 같은 CPUID 결과를
// 다시 계산할 뿐이다). 첫 arch_context_switch(=첫 CR0.TS 설정)보다
// 반드시 먼저 호출해야 한다 — 안 그러면 CR4.OSFXSR 없이 FXSAVE/XSAVE를
// 실행해 `#UD`가 난다.
void init_fpu();

}  // namespace arch_x86_64

// idt.cpp(interrupt_dispatch)가 `#NM`(벡터 7) 발생 시 부르는 진입점 —
// extern "C"라 네임스페이스 밖에 둔다(idt.cpp의 다른 벡터 핸들러
// 선언과 같은 관례).
extern "C" void arch_x86_64_handle_nm_trap();

namespace kern::object {
struct thread;
}  // namespace kern::object

// kernel/core/sched/scheduler.cpp(M11b, ADR-133 §결정3)가 kern::sched::exit()
// 에서 부르는 HAL 훅 — 영구 종료하는 스레드가 어느 코어의 FPU
// 소유자였다면 그 기록을 지운다(끊어진 스레드를 계속 "소유자"로
// 가리키는 채로 남지 않도록). ADR-002와 같은 최소 결합 관례 —
// scheduler.cpp는 g_fpu_owner의 존재 자체를 몰라도 된다.
extern "C" void arch_fpu_thread_exiting(kern::object::thread* t);
