#include "idt.h"

#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/types.h"
#include "logger.h"
#include "nmi.h"
#include "paging.h"
#include "scheduler.h"
#include "serial.h"
#include "syscall.h"
#include "timer.h"
#include "x86_64/msr.h"

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

// 레거시 syscall 트랩 진입 벡터(PN-124C105B, SP-04EE2A18 "유저랜드
// ABI" 절 - "int 0x80류 소프트웨어 인터럽트 게이트") - 33-254 범위
// 안에 있어(kDynamicVectorBase/kDynamicVectorEnd 참고) Init()의 일반
// 루프가 먼저 여느 하드웨어 인터럽트 벡터처럼 채우지만, 이 벡터
// 하나만 등록 직후 DPL=3으로 다시 덮어써 ring3의 `int 0x80`을 허용
// 한다(위 kInterruptGateTypeAttr은 모든 게이트에 DPL=0을 고정하므로
// 이 벡터만 예외). registerHandler/unregisterHandler 양쪽에서
// 명시적으로 배제해 device 인터럽트 라우팅이 실수로 이 번호를
// 재사용하지 못하게 막는다.
constexpr kernel::uint32_t kSyscallVector = 0x80;
constexpr kernel::uint8_t kInterruptGateTypeAttrDpl3 = 0xEE;  // present, DPL3, 64비트 interrupt gate

// IST(Interrupt Stack Table) 배정 - gdt.cpp의 Gdt::loadTssForThisCore()
// 가 채우는 TSS.ISTn을 가리킨다(DC-3D3212A4/QU-4E00C118, 설계자 후속
// 지시 2026-09-14 - "#DF 외 다른 벡터(NMI/#MC/#DB)의 IST 배정도
// 고려하라"). 이 네 벡터는 현재 RSP가 뭐든(설령 Task 커널 스택
// 오버플로우로 고장 나 있어도, 또는 애초에 임의 시점에 비동기로
// 들어와도) 항상 유효한 별도 스택에서 실행되게 강제한다:
//   - #DF(8): #PF 등 다른 예외 전달 중 재폴트 시 격상된다 - IST 없이는
//     그 재폴트가 다시 실패해 트리플 폴트(조용한 리셋)로 이어진다
//     (실측으로 확인된 문제, gdt.h 참고).
//   - NMI(2): 마스크 불가능(cli로도 못 막음) - 커널이 스핀락을 쥐거나
//     컨텍스트 전환 도중처럼 RSP가 일시적으로 불안정한 어느
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

// [PN-F443FE73, SP-677210E6 "#DB 상세 설계"] registerHandler와 별도
// 슬롯인 이유는 헤더 문서 주석 참고 - #DB는 고정 CPU 예외라
// registerHandler(33-254 전용)를 못 쓴다.
kernel::Idt::DebugCallback gDebugCallback = nullptr;

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

void kSetGate(kernel::uint32_t vector, kernel::uint64_t handlerAddr, kernel::uint8_t typeAttr = kInterruptGateTypeAttr) {
    gIdt[vector].offsetLow = static_cast<kernel::uint16_t>(handlerAddr & 0xFFFF);
    gIdt[vector].selector = kKernelCodeSelector;
    gIdt[vector].ist = 0;
    gIdt[vector].typeAttr = typeAttr;
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
    // 위 루프가 kSyscallVector도 여느 하드웨어 인터럽트 벡터처럼
    // DPL=0으로 채웠으니, 여기서 그 한 슬롯만 DPL=3으로 다시 덮어써
    // ring3의 `int 0x80`을 허용한다(핸들러 주소는 그대로 재사용).
    kSetGate(kSyscallVector, kIsrDynamicStubTable[kSyscallVector - kDynamicVectorBase], kInterruptGateTypeAttrDpl3);

    gIdtPointer.limit = static_cast<uint16_t>(sizeof(gIdt) - 1);
    gIdtPointer.base = reinterpret_cast<uint64_t>(&gIdt[0]);
    asm volatile("lidt %0" : : "m"(gIdtPointer));
}

void Idt::reloadOnThisCore() {
    asm volatile("lidt %0" : : "m"(gIdtPointer));
}

void Idt::registerHandler(uint32_t vector, InterruptHandler handler) {
    if (vector < kDynamicVectorBase || vector > kDynamicVectorEnd || vector == kSyscallVector) {
        return;  // kSyscallVector(0x80)은 device 라우팅 대상이 아니다 - 위 kSyscallVector 주석 참고
    }
    gDynamicHandlers[vector] = handler;
}

void Idt::unregisterHandler(uint32_t vector) {
    if (vector < kDynamicVectorBase || vector > kDynamicVectorEnd || vector == kSyscallVector) {
        return;
    }
    gDynamicHandlers[vector] = nullptr;
}

void Idt::registerDebugCallback(DebugCallback callback) {
    gDebugCallback = callback;
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

// [PN-F443FE73, SP-677210E6 "NMI 활용"] kPanic(InterruptFrame*)와
// NMI WatchdogTrap 분기 양쪽이 공유하는 레지스터 덤프 - 원래
// kPanic() 본문 그대로, 재사용을 위해 이름만 붙여 뺐다(로직 변경
// 없음).
void kPrintFrameDiagnostics(kernel::InterruptFrame* frame) {
    kernel::Serial::write("  vector=");
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

    // PN-63BCFE45 진단 강화 - rip/cs/rflags/cr2만으로는 이번 멀티
    // 프로세스 크래시(특히 #DB/TF처럼 보이는 증상)가 진짜 레지스터
    // 상태인지 스택/프레임 손상의 2차 증상인지 구분이 안 돼서, 이미
    // InterruptFrame에 있는 전체 GPR + rsp/rbp + 그 순간의 CR3까지
    // 함께 덤프하도록 넓혔다 - 앞으로의 크래시 진단에도 일반적으로
    // 유용하다.
    kernel::Serial::write("  rax=");
    kernel::Serial::writeHex(frame->rax);
    kernel::Serial::write(" rbx=");
    kernel::Serial::writeHex(frame->rbx);
    kernel::Serial::write(" rcx=");
    kernel::Serial::writeHex(frame->rcx);
    kernel::Serial::write(" rdx=");
    kernel::Serial::writeHex(frame->rdx);
    kernel::Serial::write("\n  rsi=");
    kernel::Serial::writeHex(frame->rsi);
    kernel::Serial::write(" rdi=");
    kernel::Serial::writeHex(frame->rdi);
    kernel::Serial::write(" rbp=");
    kernel::Serial::writeHex(frame->rbp);
    kernel::Serial::write(" rspOld=");
    kernel::Serial::writeHex(frame->rspOld);
    kernel::Serial::write("\n  ssOld=");
    kernel::Serial::writeHex(frame->ssOld);
    kernel::Serial::write(" r8=");
    kernel::Serial::writeHex(frame->r8);
    kernel::Serial::write(" r9=");
    kernel::Serial::writeHex(frame->r9);
    kernel::Serial::write(" r10=");
    kernel::Serial::writeHex(frame->r10);
    kernel::Serial::write("\n  r11=");
    kernel::Serial::writeHex(frame->r11);
    kernel::Serial::write(" r12=");
    kernel::Serial::writeHex(frame->r12);
    kernel::Serial::write(" r13=");
    kernel::Serial::writeHex(frame->r13);
    kernel::Serial::write(" r14=");
    kernel::Serial::writeHex(frame->r14);
    kernel::Serial::write("\n  r15=");
    kernel::Serial::writeHex(frame->r15);
    {
        kernel::uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        kernel::Serial::write(" cr3=");
        kernel::Serial::writeHex(cr3);
    }
    kernel::Serial::write("\n");
}

// [PN-F443FE73, SP-677210E6 "#DB(Debug) 상세 설계"] #DB는 NMI/#MC와
// 성격이 다르다 - 하드웨어 오류나 마스크 불가 통지가 아니라 "누군가
// 의도적으로 건 브레이크포인트/싱글스텝"이라 기본 동작이 panic이면
// 안 된다(콜백 미등록 = 아직 아무도 안 쓴다는 뜻이지 오류가 아님).
// 그래서 미처리 시에도 로그만 남기고 계속 실행한다(#MC/NMI와 다름 -
// 이 함수는 항상 kIsrHandler가 EOI 없이 바로 반환하게 만든다, 트랩
// 이지 하드웨어 IRQ가 아니므로 EOI 자체가 불필요 - kSyscallVector/
// #NM과 동일한 관례).
constexpr kernel::uint64_t kDr6BsBit = 1ULL << 14;  // Single-step

void kHandleDebugException(kernel::InterruptFrame* frame) {
    kernel::uint64_t dr6;
    asm volatile("mov %%dr6, %0" : "=r"(dr6));

    bool handled = false;
    if (gDebugCallback != nullptr) {
        handled = gDebugCallback(frame, dr6);  // true면 콜백이 처리 완료
    }

    // DR6는 CPU가 자동으로 클리어하지 않는다 - 핸들러가 명시적으로
    // 비워야 한다(SDM Vol.3 §17.2 요구사항, 안 비우면 다음 #DB에서도
    // 낡은 상태 비트가 그대로 남는다).
    asm volatile("mov %0, %%dr6" : : "r"(kernel::uint64_t{0}));

    if (!handled) {
        // 등록된 소비자가 없거나 콜백이 "내 것 아님"이라고 반환 -
        // 지금은 소비자가 실제로 없으므로 이 경로가 기본값이다.
        kernel::Logger::warn("minicore: #DB unhandled (dr6=%llx, single-step=%x)", dr6,
                              (dr6 & kDr6BsBit) != 0 ? 1 : 0);
    }
    // handled == true면 콜백이 필요한 상태 조작을 이미 끝냈다는 전제로
    // 그냥 iretq(추가로 할 일 없음).
}

// [PN-F443FE73, SP-677210E6 §"#MC(Machine Check) 실제 처리"] MCA
// (Machine Check Architecture, Intel SDM Vol.3 15.3) MSR을 읽어
// 정정 가능(corrected)/정정 불가(uncorrected) 에러를 구분한다.
// true를 반환하면(전부 정정 가능하거나 기록된 에러가 없음) 계속
// 실행해도 안전하다는 뜻 - kIsrHandler가 그대로 반환한다. false면
// (정정 불가 에러가 하나라도 있으면) 더 이상 안전하지 않으므로
// 호출부가 일반 kPanic(frame) 경로로 떨어지게 둔다.
constexpr kernel::uint32_t kMsrMcgCap = 0x179;
constexpr kernel::uint32_t kMsrMcgStatus = 0x17A;
constexpr kernel::uint32_t kMsrMc0StatusBase = 0x401;  // MCi_STATUS = base + 4*i

bool kHandleMachineCheck() {
    using kernel::arch::kReadMsr64;
    using kernel::arch::kWriteMsr64;
    const kernel::uint64_t mcgCap = kReadMsr64(kMsrMcgCap);
    const auto bankCount = static_cast<kernel::uint32_t>(mcgCap & 0xFF);
    bool uncorrectedFound = false;
    for (kernel::uint32_t i = 0; i < bankCount; ++i) {
        const kernel::uint32_t statusMsr = kMsrMc0StatusBase + 4 * i;
        const kernel::uint64_t status = kReadMsr64(statusMsr);
        constexpr kernel::uint64_t kValBit = 1ULL << 63;   // 이 뱅크에 유효한 기록이 있음
        constexpr kernel::uint64_t kUcBit = 1ULL << 61;    // Uncorrected
        constexpr kernel::uint64_t kPccBit = 1ULL << 57;   // Processor Context Corrupt
        if (!(status & kValBit)) {
            continue;  // 이 뱅크엔 기록된 에러 없음
        }
        const bool uncorrected = (status & kUcBit) != 0 || (status & kPccBit) != 0;
        kernel::Logger::warn("minicore: #MC bank=%x status=%llx (%s)", i, status,
                              uncorrected ? "UNCORRECTED" : "corrected");
        if (uncorrected) {
            uncorrectedFound = true;
        }
        // 로그로 남긴 뒤 뱅크를 비운다(SDM 15.3.1.2 권장 - 다음 에러
        // 탐지를 위해 소프트웨어가 클리어해야 한다).
        kWriteMsr64(statusMsr, 0);
    }
    // MCG_STATUS의 MCIP(bit2, Machine Check In Progress)를 반드시
    // 클리어해야 한다 - 안 하면 이후 또 다른 #MC 진입 시 프로세서가
    // 복구 불가능하다고 판단해 즉시 셧다운한다(SDM 15.3.1.1).
    kWriteMsr64(kMsrMcgStatus, 0);
    return !uncorrectedFound;
}

// [PN-F443FE73, SP-677210E6 "NMI 활용 - 워치독 및 디버그 강제 정지
// IPI"] NMI(벡터 2)는 하드웨어가 직접 발생시키는 경우도 있지만, 이
// 프로젝트에선 대부분 다른 코어가 kernel::Nmi::send()로 의도적으로
// 보낸 것이다 - kernel::Nmi::reasonForThisCore()로 그 사유를
// 구분한다. DebugHalt/WatchdogTrap 둘 다 이 코어를 안전하게 재개할
// 수 있다는 보장이 없어(전자는 다른 코어의 진짜 패닉, 후자는 이
// 코어 자신이 최근 스케줄러 틱조차 못 돈 상태) kPanic과 동일하게
// 영구 정지한다 - 다만 새로 stop-the-world를 또 보내지는 않는다
// (모든 대상 코어가 이미 같은 이유로 정지 중이므로 무의미한 IPI
// 폭주를 피한다).
void kHandleNmi(kernel::InterruptFrame* frame) {
    switch (kernel::Nmi::reasonForThisCore()) {
        case kernel::NmiReason::DebugHalt:
            kernel::Serial::write("\nminicore: NMI - debug forced halt\n");
            kPrintFrameDiagnostics(frame);
            for (;;) {
                asm volatile("cli; hlt");
            }
        case kernel::NmiReason::WatchdogTrap:
            kernel::Serial::write("\nminicore: NMI - watchdog: this core unresponsive\n");
            kPrintFrameDiagnostics(frame);
            for (;;) {
                asm volatile("cli; hlt");
            }
        case kernel::NmiReason::None:
        default:
            // 설명 안 되는 NMI(진짜 하드웨어 NMI 등 극히 드문 경우) -
            // 로그만 남기고 계속(과잉 대응 방지, RM-23F4B687 §4 원칙 -
            // 실제로 겪어본 뒤 재검토).
            kernel::Serial::write("\nminicore: NMI - unexplained, continuing\n");
            return;
    }
}

void kPanic(kernel::InterruptFrame* frame) {
    if (frame->vector == kNmiVector) {
        // 다른 미등록 벡터와 같은 일반 패닉 경로로 떨어지지 않는다 -
        // 위 kHandleNmi()가 자체적으로 처리(정지든 계속이든)를 끝낸다.
        kHandleNmi(frame);
        return;
    }

    // [SP-677210E6 "디버그 강제 정지(stop the world)"] 이 코어가 진짜로
    // 패닉하는 중이다 - 다른 로그를 찍기 전에 최대한 빨리 나머지 온라인
    // 코어부터 멈춰 공유 상태 오염/로그 뒤섞임을 막는다. Lapic::isReady()
    // 확인은 극초반(ACPI/LAPIC 준비 전) 패닉을 위한 방어.
    if (kernel::Lapic::isReady()) {
        kernel::Nmi::stopAllOtherCores();
    }

    kernel::Serial::write("\nminicore: PANIC - unhandled exception: ");
    if (frame->vector < 32) {
        kernel::Serial::write(kExceptionNames[frame->vector]);
    } else {
        // 33-254 대역인데 registerHandler로 등록된 콜백이 없는 채
        // 인터럽트가 들어온 경우 - kExceptionNames는 CPU 예외(0-31)
        // 전용이라 그대로 인덱싱하면 엉녡한 이름이 찍힌다(예전에는
        // 이 경로 자체가 없어서 문제가 없었다 - PL-2D149D8F에서 범용
        // 벡터 디스패치를 추가하며 같이 고침).
        kernel::Serial::write("Unrouted hardware interrupt");
    }
    kernel::Serial::write("\n");
    kPrintFrameDiagnostics(frame);

    for (;;) {
        asm volatile("cli; hlt");
    }
}

// 레거시 syscall 트랩(벡터 0x80) 진입점 - PN-124C105B. SP-04EE2A18
// "유저랜드 ABI" 절이 "정확한 구현은 PL에서"로 남겨 둔 레지스터
// ABI가 QU-E7E51931/QU-CD6F68B7(설계자 답변, 2026-09-15)로 확정됐다 -
// RAX(진입 시)="verb" 코드로 제출/대기를 구분한다(System V/Linux
// syscall 관례 그대로 - RAX가 최초엔 syscall 번호, 반환 시 결과값
// 으로 재사용되는 패턴을 그대로 적용한 것이지 SyscallEndpointId/
// RM-48E1E610 표와는 별개의 작은 내부 상수다):
//   0(submit) - RDI=endpointId(SyscallEndpointId), RSI=args(void*)
//               반환: RAX=token(AsyncTaskManageCode, 실패 시 0)
//   1(wait)   - RDI=token(AsyncTaskManageCode)
//               반환: RAX=result(1=성공/Completed, 0=실패·무효)
// waitForMultipleSyscall/waitAnyForMultipleSyscall용 verb 번호와 그쪽
// 레지스터 배치는 이 답변 범위 밖(QU-CD6F68B7 본문 참고) - 필요해지면
// 별도로 확정한다.
//
// PN-16CA347D(프로세스 모델)/PN-55D24891(ring3 진입) 완료로 이 벡터는
// 이제 실제 UserThread 컨텍스트에서 실행 가능하다.
constexpr kernel::uint64_t kSyscallVerbSubmit = 0;
constexpr kernel::uint64_t kSyscallVerbWait = 1;

// context_switch.S가 entry 함수의 자연 반환 시 호출하는 것과 같은
// 함수(scheduler.cpp) - self-terminate 트랩 특별 취급(아래 참고)이
// 재사용한다. 헤더 없이 extern "C" 링크만으로 직접 선언(scheduler.h의
// 공개 API로 노출할 만큼 범용은 아니다 - 이 파일과 context_switch.S,
// 딜따 호출부만 존재).
extern "C" void kTaskOnFallingToEnd();

}  // namespace

namespace kernel {

// syscall.h 선언 참고 - int 0x80(아래 kHandleSyscallTrap)과 `syscall`
// 명령(syscall_fastpath.cpp의 kHandleSyscallFast) 양쪽이 공유하는 공용
// 디스패치. **PN-124C105B("syscall 명령 경로") 신설 시 int 0x80
// 핸들러 하나에만 있던 이 로직을 그대로 옥겨 온 것뿐** - 동작 자체는
// 전혀 바뀌지 않았다.
uint64_t kDispatchSyscallVerb(uint64_t verb, uint64_t arg0, uint64_t arg1) {
    switch (verb) {
        case kSyscallVerbSubmit: {
            const auto endpointId = static_cast<SyscallEndpointId>(arg0);
            // **self-terminate는 다른 모든 syscall과 근본적으로 다르다**
            // (PN-71C3D483, QU-D96B1DCE 설계자 답변 - "syscall 디스패치가
            // self-terminate를 특별 취급") - 이 UserThread는 이제
            // 끝났으므로 절대 ring3로(=이 트랩을 건 지점으로) 복귀하면
            // 안 된다. 정상적인 submit-and-return(아래 default 경로)
            // 대신, entry 함수가 자연 반환했을 때와 완전히 동일한 처리
            // (Zombie 표시 + Syscall::submitDetached - kTaskOnFallingToEnd
            // 재사용, scheduler.cpp 참고)를 한 뒤 **이 함수에서 반환하지
            // 않고** sti+hlt 루프로 들어간다 - 호출부(int 0x80의
            // isr_common_stub 에필로그든 `syscall`의 sysretq 이전
            // 코드든)가 전혀 실행되지 않으므로 ring3로 절대 안 돌아간다.
            // 리액터가 나중에 비동기로 Scheduler::retireTask()를 불러
            // 이 커널 스택을 회수할 때까지, 이 hlt 루프가 그 자리를
            // 지킨다(kTaskFallingToEndHalt와 동일한 역할).
            if (endpointId == kSyscallEndpointSelfTerminate) {
                kTaskOnFallingToEnd();
                asm volatile("sti");
                for (;;) {
                    asm volatile("hlt");
                }
            }
            void* args = reinterpret_cast<void*>(arg1);
            return static_cast<uint64_t>(Syscall::submit(endpointId, args));
        }
        case kSyscallVerbWait: {
            const auto token = static_cast<AsyncTaskManageCode>(arg0);
            return Syscall::wait(token) ? 1 : 0;
        }
        default:
            return 0;  // 알 수 없는 verb - 실패로 취급(v1, 새 DC 불필요 수준)
    }
}

}  // namespace kernel

namespace {

void kHandleSyscallTrap(kernel::InterruptFrame* frame) {
    frame->rax = kernel::kDispatchSyscallVerb(frame->rax, frame->rdi, frame->rsi);
}

}  // namespace

// isr_common_stub(isr.S)이 호출한다.
// - 타이머(kTimerVector)/spurious(0xFF): 하드웨어 인터럽트라 반드시
//   EOI를 보내야 다음 인터럽트가 들어온다. 이 벡터는 전역 시각
//   (Timer::tickCount())만 담당한다.
// - 스케줄러 틱(kSchedulerTickVector, PL-2D3184BC 7/8단계): Timer와는
//   독립된 코어별 LAPIC 타이머 - Scheduler::onTick이 Task 전환을
//   할 수도 있어(kContextSwitch가 이 함수 호출 자체를 오래 "매달아
//   둘" 수 있음) EOI를 일반 동적 핸들러 경로(핸들러 반환 후 EOI)에
//   맡지 않고 Scheduler::onTick 안에서 가장 먼저 직접 보낸다 -
//   kTimerVector와 같은 이유의 특례.
// - 페이지 폴트(벡터 14): 먼저 Paging::kHandlePageFault로 "온디맨드
//   매핑으로 해결 가능한 폴트인지" 확인한다 - 처리됐으면 그냥 반환해
//   iretq가 폴트난 명령어를 재실행하게 둔다.
// - 범용 하드웨어 인터럽트(33-254, PL-2D149D8F): Idt::registerHandler로
//   등록된 콜백이 있으면 호출한 뒤 EOI를 보낸다 - 콜백이 없으면(라우팅
//   설정은 됐는데 핸들러 등록을 깤박은 버그) 조용히 무시하지 않고
//   진단 로그를 남기고 멈추다.
// - 그 외(진짜 잘못된 접근, 다른 예외 전부)는 진단 로그를 남기고
//   멈추다.
extern "C" void kIsrHandler(kernel::InterruptFrame* frame) {
    if (frame->vector == kernel::kTimerVector) {
        kernel::Timer::onTick();
        kernel::Lapic::sendEoi();
        return;
    }
    if (frame->vector == kernel::kSchedulerTickVector) {
        kernel::Scheduler::onTick(frame);  // EOI는 이 함수가 직접 가장 먼저 보낸다
        return;
    }
    if (frame->vector == kernel::kForcedMigrationVector) {
        kernel::Scheduler::onForcedMigration(frame);  // EOI는 이 함수가 직접 가장 먼저 보낸다 - onTick()과 동일한 이유
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
    if (frame->vector == 7) {  // #NM(Device Not Available) - lazy FPU/SSE 소유권 전환(SP-83A07867 §8, PN-F258698E)
        kernel::Scheduler::handleFpuTrap();
        return;
    }
    if (frame->vector == kDebugVector) {  // #DB - PN-F443FE73, 항상 계속 실행(panic 아님)
        kHandleDebugException(frame);
        return;
    }
    if (frame->vector == kMachineCheckVector) {  // #MC - PN-F443FE73
        if (kHandleMachineCheck()) {
            return;  // 전부 정정 가능(또는 기록된 에러 없음) - 계속 실행
        }
        // 정정 불가 에러 있음 - 아래 kPanic(frame)으로 떨어진다.
    }
    if (frame->vector == kSyscallVector) {
        // 소프트웨어 트랩(ring3의 `int 0x80`)이라 EOI 불필요 - 하드웨어
        // 인터럽트가 아니다.
        kHandleSyscallTrap(frame);
        return;
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
