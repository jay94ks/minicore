// x86_64 IDT — 최소 기반 (docs/plan/smp-fpu-bringup.md §M10, ADR-055).
// 이 계획에서 IDT에 실제로 거는 벡터는 세 종류로 제한한다:
//   (a) IPI(TLB shootdown 전용, k_vector_ipi_tlb_shootdown)
//   (b) 부팅 중 원인 불명 정지를 진단하기 위한 catch-all 예외 핸들러
//       (0~31 전 벡터 + 아직 안 쓰는 나머지 전부, 기본값)
//   (c) #NM(벡터 7) — 자리만 마련해 둔다(M11b의 lazy FPU 전환,
//       ADR-133이 실제 핸들러를 건다). 지금은 (b)와 동일하게 catch-all로
//       처리한다.
// 그 외(LAPIC spurious 벡터)는 이 계획이 LAPIC을 켜는 부산물로 필요해
// 최소한으로 함께 걷다.
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

// #NM(Device Not Available) — M11b(ADR-133, lazy FPU)가 실제로 쓸
// 자리. M10은 슬롯만 마련해 catch-all로 라우팅한다.
inline constexpr uint8_t k_vector_nm = 7;

// IDT를 구성하고 lidt로 적재한다. kernel_main 극초기, 첫 IPI/예외보다
// 반드시 먼저 호출해야 한다. LAPIC/AP는 아직 필요 없다 — 순수 CPU
// 상태(IDT)만 다룬다.
void init_idt();

}  // namespace arch_x86_64
