// x86_64 IDT — 최소 기반 (docs/plan/smp-fpu-bringup.md §M10/M11b,
// ADR-055/133). M10 계획에서 IDT에 실제로 거는 벡터는 세 종류로
// 제한했다:
//   (a) IPI(TLB shootdown 전용, k_vector_ipi_tlb_shootdown)
//   (b) 부팅 중 원인 불명 정지를 진단하기 위한 catch-all 예외 핸들러
//       (0~31의 나머지 예외 + 아직 안 쓰는 벡터, 기본값)
//   (c) #NM(벡터 7) — M11b(ADR-133)의 lazy FPU 전환 핸들러
//       (fpu.cpp::arch_x86_64_handle_nm_trap). M10은 이 자리를 마련만
//       하고 (b)로 라우팅했었다.
// 그 외(LAPIC spurious 벡터)는 이 계획이 LAPIC을 켜는 부산물로 필요해
// 최소한으로 함께 걷다.
//
// M21(general-purpose-completion.md §M21, ADR-176)이 네 번째 벡터를
// 추가한다: k_vector_timer — LAPIC 타이머의 주기적 인터럽트로,
// idt.cpp가 EOI 후 kern::sched::on_timer_tick()을 부른다(선점형 스케줄링의
// 유일한 강제 전환 지점).
#pragma once

#include <cstdint>

namespace arch_x86_64 {

// isr_stubs.S가 채우는 하드웨어 스택 프레임 그대로의 레이아웃 — 순서는
// isr_common(같은 파일)의 push 순서를 반대로 읆은 것과 정확히 같다.
// GPR 저장 이후 프레임(vector/error_code/iretq 5필드)은 하드웨어와
// isr_stub_N(둘 다 isr_stubs.S)이 채운다.
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, user_rsp, ss;
};

// IPI 기반 TLB shootdown 전용(M10, ADR-055) — kernel/arch/x86_64/smp.cpp가
// 이 벡터로 온 인터럽트에서 invlpg를 수행한다.
inline constexpr uint8_t k_vector_ipi_tlb_shootdown = 0xFC;

// LAPIC 스퓨리어스 벡터(Intel SDM Vol.3 §11.9) — EOI 불필요, 로그만
// 남기고 즉시 리턴한다.
inline constexpr uint8_t k_vector_spurious = 0xFF;

// #NM(Device Not Available) — M11b(ADR-133, lazy FPU 전환)가 실제로
// 쓴다. M10은 이 슬롯을 마련만 해 두고 catch-all로 라우팅했었다.
inline constexpr uint8_t k_vector_nm = 7;

// #PF(Page Fault) — M12(system-servers-bringup.md §M12, ADR-016)의
// COW 쓰기 폴트 처리(page_fault.cpp::try_handle_cow_write_fault)가
// 실제로 쓴다. 그 함수가 false를 반환하면(COW 대상이 아닌 진짜 폴트)
// M10의 catch-all(diagnose_and_halt)로 그대로 떨어진다.
inline constexpr uint8_t k_vector_page_fault = 14;

// M21(ADR-176) — LAPIC 타이머(선점형 스케줄링). 0~31(CPU 예외) +
// k_vector_ipi_tlb_shootdown(0xFC) + k_vector_spurious(0xFF)와 겹치지
// 않는 임의의 자리 — SDM이 정한 의미가 없는 순수 소프트웨어 벡터다.
inline constexpr uint8_t k_vector_timer = 0x40;

// IDT를 구성하고 lidt로 적재한다. kernel_main 극초기, 첫 IPI/예외보다
// 반드시 먼저 호출해야 한다. LAPIC/AP는 아직 필요 없다 — 순수 CPU
// 상태(IDT)만 다룬다.
void init_idt();

}  // namespace arch_x86_64
