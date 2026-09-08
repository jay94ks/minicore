// IDT 구성 + interrupt_dispatch (idt.hpp 상단 주석 참고).
#include "idt.hpp"

#include "gdt_selectors.hpp"

#include <cstdint>

#include <klog.hpp>

// isr_stubs.S가 채운 256개 진입점 주소 테이블.
extern "C" void* isr_stub_table[256];

// smp.cpp(M10, ADR-055)가 정의한다 — IPI 기반 TLB shootdown의 실제
// invlpg + ack 처리는 그쪽 소유다. idt.cpp는 벡터 라우팅만 담당한다
// (ADR-002와 같은 정신의 계층 분리 — 여기서는 헤더 include 없이 최소
// extern 선언만 둔다, scheduler.cpp의 arch_context_switch 선언과 같은
// 관례).
extern "C" void smp_handle_tlb_shootdown_ipi();

// lapic.cpp(M10)가 정의한다 — 스퓨리어스 벡터는 EOI가 필요 없지만
// (Intel SDM Vol.3 §11.9), IPI 벡터의 실제 EOI는 smp_handle_tlb_shootdown_ipi()
// 자신이 처리한다(lapic.hpp 참고).
extern "C" void lapic_eoi();

namespace arch_x86_64 {

namespace {

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

constexpr uint8_t k_gate_present_interrupt64 = 0x8E;  // present=1, DPL=0, type=0xE(64비트 인터럽트 게이트)

idt_entry g_idt[256] = {};

void set_gate(uint32_t vector, void* handler) {
    auto addr = reinterpret_cast<uint64_t>(handler);
    idt_entry& e = g_idt[vector];
    e.offset_low = static_cast<uint16_t>(addr & 0xFFFF);
    e.selector = k_sel_code64;
    e.ist = 0;
    e.type_attr = k_gate_present_interrupt64;
    e.offset_mid = static_cast<uint16_t>((addr >> 16) & 0xFFFF);
    e.offset_high = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFFu);
    e.reserved = 0;
}

// panic.cpp(libk_detail::print_backtrace, ADR-126)와 같은 프레임포인터
// 체인 방식이지만, 시작 fp가 "지금 이 함수를 호출한 스택"이 아니라
// interrupt_frame에 저장된, 인터럽트/예외가 실제로 발생한 지점의 rbp다
// — 그래야 백트레이스가 인터럽트 진입 스텁이 아니라 원인이 된 코드
// 지점부터 시작한다.
constexpr int k_max_backtrace_frames = 16;

void print_backtrace_from(uint64_t start_fp) {
    auto* fp = reinterpret_cast<uint64_t*>(start_fp);
    klog::printf("[idt] backtrace:\n");
    for (int i = 0; i < k_max_backtrace_frames; ++i) {
        if (fp == nullptr || (reinterpret_cast<uint64_t>(fp) & 0x7) != 0) {
            break;
        }
        uint64_t saved_fp = fp[0];
        uint64_t ret_addr = fp[1];
        if (ret_addr == 0) {
            break;
        }
        klog::printf("  #%d 0x%lx\n", i, static_cast<unsigned long>(ret_addr));
        if (saved_fp <= reinterpret_cast<uint64_t>(fp)) {
            break;
        }
        fp = reinterpret_cast<uint64_t*>(saved_fp);
    }
}

// M10의 catch-all — "부팅 중 원인 불명 정지를 진단"(계획 §M10)이
// 목적이다. 일반 예외 처리 정책(페이지 폴트 등)은 이 계획의 범위 밖 —
// 여기서는 무조건 레지스터를 klog로 찍고 백트레이스 후 정지한다.
[[noreturn]] void diagnose_and_halt(const interrupt_frame& f) {
    klog::printf(
        "[idt] unexpected exception vector=%lu error_code=0x%lx rip=0x%lx cs=0x%lx "
        "rflags=0x%lx rsp=0x%lx ss=0x%lx\n",
        static_cast<unsigned long>(f.vector), static_cast<unsigned long>(f.error_code),
        static_cast<unsigned long>(f.rip), static_cast<unsigned long>(f.cs),
        static_cast<unsigned long>(f.rflags), static_cast<unsigned long>(f.user_rsp),
        static_cast<unsigned long>(f.ss));
    klog::printf(
        "[idt] rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx rsi=0x%lx rdi=0x%lx rbp=0x%lx\n",
        static_cast<unsigned long>(f.rax), static_cast<unsigned long>(f.rbx),
        static_cast<unsigned long>(f.rcx), static_cast<unsigned long>(f.rdx),
        static_cast<unsigned long>(f.rsi), static_cast<unsigned long>(f.rdi),
        static_cast<unsigned long>(f.rbp));
    klog::printf(
        "[idt] r8=0x%lx r9=0x%lx r10=0x%lx r11=0x%lx r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
        static_cast<unsigned long>(f.r8), static_cast<unsigned long>(f.r9),
        static_cast<unsigned long>(f.r10), static_cast<unsigned long>(f.r11),
        static_cast<unsigned long>(f.r12), static_cast<unsigned long>(f.r13),
        static_cast<unsigned long>(f.r14), static_cast<unsigned long>(f.r15));
    print_backtrace_from(f.rbp);

    for (;;) {
        asm volatile("cli; hlt");
    }
}

}  // namespace

}  // namespace arch_x86_64

// isr_common(isr_stubs.S)이 부르는 C++ 쪽 — extern "C"라 arch_x86_64
// 네임스페이스 밖에 둔다(idt.cpp 자신만 쓰는 내부 심벌이라 헤더에는
// 선언하지 않는다).
extern "C" void interrupt_dispatch(arch_x86_64::interrupt_frame* frame) {
    if (frame->vector == arch_x86_64::k_vector_ipi_tlb_shootdown) {
        smp_handle_tlb_shootdown_ipi();
        return;
    }
    if (frame->vector == arch_x86_64::k_vector_spurious) {
        // Intel SDM Vol.3 §11.9 — 스퓨리어스 벡터는 EOI를 보내지 않는다
        // (진짜 인터럽트가 아니었다는 신호이므로 큐에 남길 것이 없다).
        klog::printf("[idt] spurious interrupt (vector 0xFF) — ignored\n");
        return;
    }

    // 나머지 전부(0~31 예외 + #NM 자리 + 아직 안 쓰는 벡터) — catch-all.
    arch_x86_64::diagnose_and_halt(*frame);
}

namespace arch_x86_64 {

void init_idt() {
    for (uint32_t v = 0; v < 256; ++v) {
        set_gate(v, isr_stub_table[v]);
    }

    idt_pointer ptr{};
    ptr.limit = static_cast<uint16_t>(sizeof(g_idt) - 1);
    ptr.base = reinterpret_cast<uint64_t>(g_idt);
    asm volatile("lidt %0" : : "m"(ptr));
}

}  // namespace arch_x86_64
