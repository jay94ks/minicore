#include "idt.h"

#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/types.h"
#include "paging.h"
#include "serial.h"
#include "timer.h"

namespace {

struct IdtEntry {
    kernel::uint16_t offsetLow;
    kernel::uint16_t selector;
    kernel::uint8_t ist;
    kernel::uint8_t typeAttr;
    kernel::uint16_t offsetMid;
    kernel::uint32_t offsetHigh;
    kernel::uint32_t reserved;
} __attribute__((packed));

struct IdtPointer {
    kernel::uint16_t limit;
    kernel::uint64_t base;
} __attribute__((packed));

constexpr kernel::uint32_t kVectorCount = 32;
constexpr kernel::uint32_t kDynamicVectorBase = 33;   // isr.S의 kIsrDynamicStubTable[0]에 대응
constexpr kernel::uint32_t kDynamicVectorEnd = 254;   // 포함(inclusive)
constexpr kernel::uint16_t kKernelCodeSelector = 0x08;
constexpr kernel::uint8_t kInterruptGateTypeAttr = 0x8E;  // present, DPL0, 64비트 interrupt gate

// IST(Interrupt Stack Table) 배정 - gdt.cpp의 Gdt::loadTssForThisCore()
// 가 채우는 TSS.ISTn을 가리킨다(DC-3D3212A4/QU-4E00C118, 설계자 후속
// 지시 2026-09-14 - "#DF 외 다른 벡터(NMI/#MC/#DB)의 IST 배정도
// 고려하라"). 이 네 벡터는 현재 RSP가 뭐든(설령 Task 커널 스택
// 오버플로우로 고장나 있어도, 또는 애초에 임의 시점에 비동기로
// 들어와도) 항상 유효한 별도 스택에서 실행되게 강제한다:
//   - #DF(8): #PF 등 다른 예외 전달 중 재폴트 시 격상된다 - IST 없이는
//     그 재폴트가 다시 실패해 트리플 폴트(조용한 리셋)로 이어진다
//     (실측으로 확인된 문제, gdt.h 참고).
//   - NMI(2): 마스크 불가능(cli로도 못 막음) - 커널이 스핀락을 쥔
//     채거나 컨텍스트 전환 도중처럼 RSP가 일시적으로 불안정한 어떤
//     순간에도 끼어들 수 있다.
//   - #MC(18, Machine Check): 하드웨어 오류 - NMI와 같은 이유로 임의
//     시점에 들어올 수 있다.
//   - #DB(1, Debug): 디버그 예외(브레이크포인트/싱글스텝) - 향후
//     디버깅 지원 시 같은 이유로 안전한 스택이 필요해질 것을 대비해
//     지금 같이 배정해 둔다.
// IST1-7 중 4개만 쓰고 나머지(IST5-7)는 향후 다른 벡터가 필요해지면
// 같은 패턴으로 배정한다(SP-677210E6 참고).
constexpr kernel::uint32_t kDebugVector = 1;
constexpr kernel::uint32_t kNmiVector = 2;
constexpr kernel::uint32_t kDoubleFaultVector = 8;
constexpr kernel::uint32_t kMachineCheckVector = 18;

constexpr kernel::uint8_t kDoubleFaultIst = 1;
constexpr kernel::uint8_t kNmiIst = 2;
constexpr kernel::uint8_t kMachineCheckIst = 3;
constexpr kernel::uint8_t kDebugIst = 4;

IdtEntry gIdt[256];
IdtPointer gIdtPointer;

// registerHandler/unregisterHandler가 채우는 벡터->콜백 표(33-254만
// 유효, 나머지 인덱스는 항상 nullptr로 남는다 - 커널이 직접 처리).
kernel::Idt::InterruptHandler gDynamicHandlers[256] = {};

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

// 하드웨어 인터럽트(CPU 예외 0-31 밖) - 타이머/spurious는 이름 그대로,
// 나머지(33-254)는 isr.S가 만든 주소 표(kIsrDynamicStubTable)로 받는다.
extern "C" void isr32();
extern "C" void isr255();
extern "C" const kernel::uint64_t kIsrDynamicStubTable[kDynamicVectorEnd - kDynamicVectorBase + 1];

using IsrStub = void (*)();

const IsrStub kIsrStubs[kVectorCount] = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

void kSetGate(kernel::uint32_t vector, kernel::uint64_t handlerAddr) {
    gIdt[vector].offsetLow = static_cast<kernel::uint16_t>(handlerAddr & 0xFFFF);
    gIdt[vector].selector = kKernelCodeSelector;
    gIdt[vector].ist = 0;
    gIdt[vector].typeAttr = kInterruptGateTypeAttr;
    gIdt[vector].offsetMid = static_cast<kernel::uint16_t>((handlerAddr >> 16) & 0xFFFF);
    gIdt[vector].offsetHigh = static_cast<kernel::uint32_t>((handlerAddr >> 32) & 0xFFFFFFFF);
    gIdt[vector].reserved = 0;
}

void kSetGate(kernel::uint32_t vector, IsrStub handler) {
    kSetGate(vector, reinterpret_cast<kernel::uint64_t>(handler));
}

}  // namespace

namespace kernel {

void Idt::init() {
    for (uint32_t vector = 0; vector < kVectorCount; ++vector) {
        kSetGate(vector, kIsrStubs[vector]);
    }
    gIdt[kDebugVector].ist = kDebugIst;
    gIdt[kNmiVector].ist = kNmiIst;
    gIdt[kDoubleFaultVector].ist = kDoubleFaultIst;
    gIdt[kMachineCheckVector].ist = kMachineCheckIst;
    kSetGate(kTimerVector, isr32);
    kSetGate(0xFF, isr255);
    for (uint32_t vector = kDynamicVectorBase; vector <= kDynamicVectorEnd; ++vector) {
        kSetGate(vector, kIsrDynamicStubTable[vector - kDynamicVectorBase]);
    }

    gIdtPointer.limit = static_cast<uint16_t>(sizeof(gIdt) - 1);
    gIdtPointer.base = reinterpret_cast<uint64_t>(&gIdt[0]);
    asm volatile("lidt %0" : : "m"(gIdtPointer));
}

void Idt::reloadOnThisCore() {
    asm volatile("lidt %0" : : "m"(gIdtPointer));
}

void Idt::registerHandler(uint32_t vector, InterruptHandler handler) {
    if (vector < kDynamicVectorBase || vector > kDynamicVectorEnd) {
        return;
    }
    gDynamicHandlers[vector] = handler;
}

void Idt::unregisterHandler(uint32_t vector) {
    if (vector < kDynamicVectorBase || vector > kDynamicVectorEnd) {
        return;
    }
    gDynamicHandlers[vector] = nullptr;
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

kernel::uint64_t kReadCr2() {
    kernel::uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

void kPanic(kernel::InterruptFrame* frame) {
    kernel::Serial::write("\nminicore: PANIC - unhandled exception: ");
    if (frame->vector < 32) {
        kernel::Serial::write(kExceptionNames[frame->vector]);
    } else {
        // 33-254 대역인데 registerHandler로 등록된 콜백이 없는 채로
        // 인터럽트가 들어온 경우 - kExceptionNames는 CPU 예외(0-31)
        // 전용이라 그대로 인덱싱하면 엉뚱한 이름이 찍힌다(예전에는
        // 이 경로 자체가 없어서 문제가 없었다 - PL-2D149D8F에서 범용
        // 벡터 디스패치를 추가하며 같이 고침).
        kernel::Serial::write("Unrouted hardware interrupt");
    }
    kernel::Serial::write("\n  vector=");
    kernel::Serial::writeHex(frame->vector);
    kernel::Serial::write(" error_code=");
    kernel::Serial::writeHex(frame->errorCode);
    kernel::Serial::write("\n  rip=");
    kernel::Serial::writeHex(frame->rip);
    kernel::Serial::write(" cs=");
    kernel::Serial::writeHex(frame->cs);
    kernel::Serial::write(" rflags=");
    kernel::Serial::writeHex(frame->rflags);
    kernel::Serial::write("\n");

    if (frame->vector == 14) {  // Page Fault
        kernel::Serial::write("  cr2(fault addr)=");
        kernel::Serial::writeHex(kReadCr2());
        kernel::Serial::write("\n");
    }

    for (;;) {
        asm volatile("cli; hlt");
    }
}

}  // namespace

// isr_common_stub(isr.S)이 호출한다.
// - 타이머(kTimerVector)/spurious(0xFF): 하드웨어 인터럽트라 반드시
//   EOI를 보내야 다음 인터럽트가 들어온다. 스케줄러가 생기기 전까지
//   타이머는 그냥 틱만 센다.
// - 페이지 폴트(벡터 14): 먼저 Paging::kHandlePageFault로 "온디맨드
//   매핑으로 해결 가능한 폴트인지" 확인한다 - 처리됐으면 그냥 반환해
//   iretq가 폴트난 명령어를 재실행하게 둔다.
// - 범용 하드웨어 인터럽트(33-254, PL-2D149D8F): Idt::registerHandler로
//   등록된 콜백이 있으면 호출한 뒤 EOI를 보낸다 - 콜백이 없으면(라우팅
//   설정은 됐는데 핸들러 등록을 깜빡한 버그) 조용히 무시하지 않고
//   진단 로그를 남기고 멈춘다.
// - 그 외(진짜 잘못된 접근, 다른 예외 전부)는 진단 로그를 남기고
//   멈춘다.
extern "C" void kIsrHandler(kernel::InterruptFrame* frame) {
    if (frame->vector == kernel::kTimerVector) {
        kernel::Timer::onTick();
        kernel::Lapic::sendEoi();
        return;
    }
    if (frame->vector == 0xFF) {
        return;  // spurious - EOI 불필요(스펙상 안 보내도 됨)
    }
    if (frame->vector == 14) {
        const kernel::uint64_t faultAddr = kReadCr2();
        if (kernel::Paging::handlePageFault(faultAddr, frame->errorCode)) {
            return;
        }
    }
    if (frame->vector >= kDynamicVectorBase && frame->vector <= kDynamicVectorEnd) {
        auto* handler = gDynamicHandlers[frame->vector];
        if (handler) {
            handler(frame);
            kernel::Lapic::sendEoi();
            return;
        }
    }
    kPanic(frame);
}
