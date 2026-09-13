#include "idt.h"

#include "interrupt_frame.h"
#include "paging.h"
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

}  // namespace

namespace {

unsigned long kReadCr2() {
    unsigned long cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

void kPanic(kernel::InterruptFrame* frame) {
    kernel::Serial::kWrite("\nminicore: PANIC - unhandled exception: ");
    kernel::Serial::kWrite(kExceptionNames[frame->vector & 0x1F]);
    kernel::Serial::kWrite("\n  vector=");
    kernel::Serial::kWriteHex(frame->vector);
    kernel::Serial::kWrite(" error_code=");
    kernel::Serial::kWriteHex(frame->errorCode);
    kernel::Serial::kWrite("\n  rip=");
    kernel::Serial::kWriteHex(frame->rip);
    kernel::Serial::kWrite(" cs=");
    kernel::Serial::kWriteHex(frame->cs);
    kernel::Serial::kWrite(" rflags=");
    kernel::Serial::kWriteHex(frame->rflags);
    kernel::Serial::kWrite("\n");

    if (frame->vector == 14) {  // Page Fault
        kernel::Serial::kWrite("  cr2(fault addr)=");
        kernel::Serial::kWriteHex(kReadCr2());
        kernel::Serial::kWrite("\n");
    }

    for (;;) {
        asm volatile("cli; hlt");
    }
}

}  // namespace

// isr_common_stub(isr.S)이 호출한다. 페이지 폴트(벡터 14)는 먼저
// Paging::kHandlePageFault로 "온디맨드 매핑으로 해결 가능한 폴트인지"
// 확인한다 - 처리됐으면 그냥 반환해 iretq가 폴트난 명령어를 재실행
// 하게 둔다. 그 외(진짜 잘못된 접근, 다른 예외 전부)는 진단 로그를
// 남기고 멈춘다.
extern "C" void kIsrHandler(kernel::InterruptFrame* frame) {
    if (frame->vector == 14) {
        const unsigned long faultAddr = kReadCr2();
        if (kernel::Paging::kHandlePageFault(faultAddr, frame->errorCode)) {
            return;
        }
    }
    kPanic(frame);
}
