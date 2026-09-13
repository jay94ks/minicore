#include "idt.h"

#include "interrupt_frame.h"
#include "serial.h"

namespace {

struct IdtEntry {
    unsigned short offsetLow;
    unsigned short selector;
    unsigned char ist;
    unsigned char typeAttr;
    unsigned short offsetMid;
    unsigned int offsetHigh;
    unsigned int reserved;
} __attribute__((packed));

struct IdtPointer {
    unsigned short limit;
    unsigned long base;
} __attribute__((packed));

constexpr int kVectorCount = 32;
constexpr unsigned short kKernelCodeSelector = 0x08;
constexpr unsigned char kInterruptGateTypeAttr = 0x8E;  // present, DPL0, 64비트 interrupt gate

IdtEntry gIdt[256];
IdtPointer gIdtPointer;

// isr.S가 벡터 0~31 각각에 대해 정의한 스텁 - 이름은 그대로 isrN.
extern "C" void isr0();
extern "C" void isr1();
extern "C" void isr2();
extern "C" void isr3();
extern "C" void isr4();
extern "C" void isr5();
extern "C" void isr6();
extern "C" void isr7();
extern "C" void isr8();
extern "C" void isr9();
extern "C" void isr10();
extern "C" void isr11();
extern "C" void isr12();
extern "C" void isr13();
extern "C" void isr14();
extern "C" void isr15();
extern "C" void isr16();
extern "C" void isr17();
extern "C" void isr18();
extern "C" void isr19();
extern "C" void isr20();
extern "C" void isr21();
extern "C" void isr22();
extern "C" void isr23();
extern "C" void isr24();
extern "C" void isr25();
extern "C" void isr26();
extern "C" void isr27();
extern "C" void isr28();
extern "C" void isr29();
extern "C" void isr30();
extern "C" void isr31();

using IsrStub = void (*)();

const IsrStub kIsrStubs[kVectorCount] = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

void kSetGate(int vector, IsrStub handler) {
    const auto addr = reinterpret_cast<unsigned long>(handler);
    gIdt[vector].offsetLow = static_cast<unsigned short>(addr & 0xFFFF);
    gIdt[vector].selector = kKernelCodeSelector;
    gIdt[vector].ist = 0;
    gIdt[vector].typeAttr = kInterruptGateTypeAttr;
    gIdt[vector].offsetMid = static_cast<unsigned short>((addr >> 16) & 0xFFFF);
    gIdt[vector].offsetHigh = static_cast<unsigned int>((addr >> 32) & 0xFFFFFFFF);
    gIdt[vector].reserved = 0;
}

}  // namespace

namespace kernel {

void Idt::kInit() {
    for (int vector = 0; vector < kVectorCount; ++vector) {
        kSetGate(vector, kIsrStubs[vector]);
    }

    gIdtPointer.limit = static_cast<unsigned short>(sizeof(gIdt) - 1);
    gIdtPointer.base = reinterpret_cast<unsigned long>(&gIdt[0]);
    asm volatile("lidt %0" : : "m"(gIdtPointer));
}

}  // namespace kernel

namespace {

const char* const kExceptionNames[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point Exception", "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection Exception", "VMM Communication Exception", "Security Exception", "Reserved",
};

void kWriteHex64(unsigned long value) {
    char buf[19] = "0x0000000000000000";
    constexpr char kHexDigits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        buf[18 - i] = kHexDigits[(value >> (i * 4)) & 0xF];
    }
    kernel::Serial::kWrite(buf);
}

}  // namespace

// isr_common_stub(isr.S)이 호출한다 - 아직 복구 경로가 없으니 진단
// 로그만 남기고 멈춘다. rip/vector/error code는 항상, 페이지 폴트는
// CR2(폴트 주소)도 같이 찍는다.
extern "C" void kIsrHandler(kernel::InterruptFrame* frame) {
    kernel::Serial::kWrite("\nminicore: PANIC - unhandled exception: ");
    kernel::Serial::kWrite(kExceptionNames[frame->vector & 0x1F]);
    kernel::Serial::kWrite("\n  vector=");
    kWriteHex64(frame->vector);
    kernel::Serial::kWrite(" error_code=");
    kWriteHex64(frame->errorCode);
    kernel::Serial::kWrite("\n  rip=");
    kWriteHex64(frame->rip);
    kernel::Serial::kWrite(" cs=");
    kWriteHex64(frame->cs);
    kernel::Serial::kWrite(" rflags=");
    kWriteHex64(frame->rflags);
    kernel::Serial::kWrite("\n");

    if (frame->vector == 14) {  // Page Fault
        unsigned long cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        kernel::Serial::kWrite("  cr2(fault addr)=");
        kWriteHex64(cr2);
        kernel::Serial::kWrite("\n");
    }

    for (;;) {
        asm volatile("cli; hlt");
    }
}
