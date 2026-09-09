// IDT 구성 + interrupt_dispatch (idt.hpp 상단 주석 참고).
#include "idt.hpp"

#include "gdt_selectors.hpp"
#include "page_fault.hpp"

#include <cstdint>

#include <klog.hpp>
#include <sched/scheduler.hpp>

// isr_stubs.S가 채운 256개 진입점 주소 테이블.
extern "C" void* isr_stub_table[256];

// smp.cpp(M10, ADR-055)가 정의한다 — IPI 기반 TLB shootdown의 실제
// invlpg + ack 처리는 그쪽 소유다. idt.cpp는 벡터 라우팅만 담당한다
// (ADR-002와 같은 정신의 계층 분리 — 여기서는 헤더 include 없이 최소
// extern 선언만 둔다, scheduler.cpp의 arch_context_switch 선언과 같은
// 관례).
extern "C" void smp_handle_tlb_shootdown_ipi();

// fpu.cpp(M11b, ADR-133)가 정의한다 — lazy FPU 소유권 전환의 실제
// XSAVE/FXSAVE 저장·복원은 그쪽 소유다.
extern "C" void arch_x86_64_handle_nm_trap();

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

void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

// 레거시 8259 PIC(마스터 0x20/0x21, 슬레이브 0xA0/0xA1)를 IRQ 마스크
// 레지스터(OCW1)에 0xFF를 써서 전부 막는다 — 이 커널은 M10부터
// LAPIC/IOAPIC 경로(lapic.cpp, MADT 파싱)만 쓰고 8259는 애초에 리맵도
// 하지 않는다. 리맵을 안 한 8259는 기본 벡터 오프셋(마스터=8~15)을
// 그대로 쓰므로, IRQ0(타이머)가 **CPU 예외 벡터 8(#DF, 더블폴트)과
// 그대로 충돌**한다 — QEMU의 PVH/qboot.rom 개발 경로(ADR-114)는
// 레거시 PIT를 사실상 건드리지 않아 이 문제가 드러나지 않았지만,
// 실제 BIOS(SeaBIOS)+GRUB(Multiboot2, ADR-017) 경로로 처음 부팅해
// 보니 SeaBIOS가 남겨 둔 8259/PIT가 여전히 살아 있어, 유저모드
// 최초 진입(enter_usermode, usermode.S — RFLAGS.IF=1로 인터럽트가
// 처음 켜지는 지점) 직후 IRQ0이 그대로 벡터 8로 들어와 "가짜
// 더블폴트"처럼 보였다(실제 인터럽트 프레임에 CPU가 채워 넣는
// error_code가 없어 idt.hpp::interrupt_frame이 한 칸씩 밀려 읽힌
// 것도 그 결과다). 리맵 대신 마스킹만 하는 이유는 이 커널이 8259를
// 쓸 계획이 전혀 없어서다(리맵은 "쓰되 벡터를 옮긴다"는 뜻인데,
// 그럴 필요조차 없다).
void disable_legacy_pic() {
    outb(0xA1, 0xFF);  // 슬레이브부터 — 마스터가 슬레이브의 캐스케이드
                        // 라인(IRQ2)을 통해 슬레이브 IRQ를 여전히
                        // 전달할 수 있으므로 순서상 안전한 쪽부터 막는다.
    outb(0x21, 0xFF);
}

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
    if (frame->vector == arch_x86_64::k_vector_timer) {
        // M21(ADR-176) — EOI를 **먼저** 보낸다. sched::on_timer_tick()이
        // 내부적으로 sched::yield()를 호출하면 arch_context_switch가 이
        // 인터럽트의 스택 프레임을 통째로 "다른 스레드의 콜스택 아래"에
        // 묻어 버린다 — 이 스레드가 다시 스케줄돼 이 함수까지 되돌아올
        // 때에야 비로소 (isr_common의 에필로그가) iretq를 실행한다. 그
        // 사이(다른 스레드가 얼마든지 오래 실행될 수 있는 구간) 동안
        // EOI가 안 나가 있으면, 이 코어의 LAPIC은 이 벡터의 ISR 비트를
        // 계속 세운 채로 남아 있어 이후 인터럽트를 받지 못한다. 그래서
        // "일 다 하고 마지막에 EOI"(smp_handle_tlb_shootdown_ipi의
        // 관례)가 아니라 여기서는 반드시 순서를 뒤집는다.
        lapic_eoi();
        sched::on_timer_tick();
        return;
    }
    if (frame->vector == arch_x86_64::k_vector_nm) {
        // M11b(ADR-133) — M10이 마련해 둔 자리를 이제 실제로 채운다.
        arch_x86_64_handle_nm_trap();
        return;
    }
    if (frame->vector == arch_x86_64::k_vector_page_fault) {
        // M12(ADR-016) — CR2(폴트 가상주소)는 #PF 진입 시점 값을 그대로
        // 읽어야 한다(뒤이은 다른 코드가 CR2를 건드리기 전에 이 함수가
        // 즉시 호출되므로 안전하다). COW로 처리됐으면(true) 그대로
        // 리턴 — iretq가 같은 명령을 재실행한다.
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        if (arch_x86_64::try_handle_cow_write_fault(cr2, frame->error_code)) {
            return;
        }
        // COW 대상이 아니었다 — 진짜 폴트. M10의 catch-all로 떨어진다.
        arch_x86_64::diagnose_and_halt(*frame);
    }

    // 나머지 전부(0~31의 다른 예외 + 아직 안 쓰는 벡터) — catch-all.
    arch_x86_64::diagnose_and_halt(*frame);
}

namespace arch_x86_64 {

void init_idt() {
    disable_legacy_pic();

    for (uint32_t v = 0; v < 256; ++v) {
        set_gate(v, isr_stub_table[v]);
    }

    idt_pointer ptr{};
    ptr.limit = static_cast<uint16_t>(sizeof(g_idt) - 1);
    ptr.base = reinterpret_cast<uint64_t>(g_idt);
    asm volatile("lidt %0" : : "m"(ptr));
}

}  // namespace arch_x86_64
