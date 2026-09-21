#include "idt.h"

#include "acpi.h"
#include "deferred_destruction.h"
#include "gdt.h"
#include "interrupt_frame.h"
#include "lapic.h"
#include "libkenv/types.h"
#include "logger.h"
#include "nmi.h"
#include "paging.h"
#include "panic.h"
#include "process.h"
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
//   - #PF(14, Page Fault, SP-A252E82F "인터럽트 컨텍스트 재설계",
//     2026-09-21): 온디맨드 페이징 구현 이후 유일하게 남아 있던,
//     "회피 불가능한데도 아직 IST가 없던" 예외 - 일반 인터럽트
//     핸들러 실행 도중에도(예: 그 핸들러가 새 페이지를 만지다 폴트를
//     내면) 발생할 수 있어 다른 넷과 같은 성격이다. 전체 저장소
//     `sti` 전수 조사로 확인한 대로 일반 인터럽트는 절대 서로 중첩될
//     수 없으므로(IF=0 유지), 회피 불가능한 예외/NMI 다섯 개만
//     IST로 격리하면 나머지 모든 벡터는 카운터 없이 단일 스택을
//     공유해도 안전하다(deferred_destruction.h/.cpp의 새 설계).
// IST1-7 중 5개를 쓰고 나머지(IST6-7)는 향후 다른 벡터가 필요해지면
// 같은 패턴으로 배정한다(SP-677210E6 참고).
constexpr kernel::uint32_t kDebugVector = 1;
constexpr kernel::uint32_t kNmiVector = 2;
constexpr kernel::uint32_t kDoubleFaultVector = 8;
constexpr kernel::uint32_t kPageFaultVector = 14;
constexpr kernel::uint32_t kMachineCheckVector = 18;

constexpr kernel::uint8_t kDoubleFaultIst = 1;
constexpr kernel::uint8_t kNmiIst = 2;
constexpr kernel::uint8_t kMachineCheckIst = 3;
constexpr kernel::uint8_t kDebugIst = 4;
constexpr kernel::uint8_t kPageFaultIst = 5;

// [신규, 2026-09-21, SP-A252E82F §2] 코어별 "지금 이 코어에서 #PF
// 핸들러가 실행 중인가" 플래그 - #PF 핸들러(Paging::handlePageFault())
// 실행 도중 또 #PF가 발생하면(재진입), IST5가 하드웨어적으로 같은
// 고정 스택 top으로 rsp를 되돌려 버려 진행 중이던 첫 #PF 프레임을
// 덮어쓴다(IST는 그 자체로 재진입 안전하지 않음 - gdt.h 문서 참고).
// 설계자 지시(QU-65761CB0 확정)에 따라 이는 "막아야 할 위험"이 아니라
// "#PF 핸들러 코드 자체의 버그"로 취급한다 - 이 핸들러는 항상
// 영구 매핑된 커널 구조체만 건드리도록 설계돼 있으므로, 정상 동작
// 중에는 절대 재귀적으로 폴트를 내지 않아야 한다. 그래서 리커버리를
// 시도하지 않고 즉시 kPanic()으로 rip/cr2를 로그에 남기고 멈춘다.
kernel::uint32_t gInPageFaultHandler[kernel::kAcpiMaxCpus] = {};

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
    gIdt[kPageFaultVector].ist = kPageFaultIst;
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

// [신규, 2026-09-18, PN-F7EBD6F5 근본 원인 수정] `Serial::write()`는
// 자기 자신 한 번의 호출만 락으로 감싼다(serial.cpp의 `gWriteLock`
// 주석 참고) - 그런데 이 진단 덤프는 예전엔 `Serial::write`/
// `writeHex`를 20회 넘게 연달아 호출했다. 그 호출 사이사이(락이
// 풀려 있는 틈)에 다른 코어가 끼어들면 두 코어의 출력이 호출 단위로
// 뒤섞인다 - 실제로 관측된 "vector=  vector=0x...002 error_code=..."
// 류의 깨진 로그(PN-907C5289/PN-F7EBD6F5)가 정확히 이 패턴이다.
// `SMP4`에서 하나의 stop-the-world NMI가 여러 코어에 동시에 도착하면
// 그 코어들 전부가 이 함수를 거의 동시에 실행하므로 실측 재현율이
// 매우 높았다(PN-F7EBD6F5, 90%대). 해법은 logger.cpp의
// `SerialLoggingDriver::writeLine()`이 이미 쓰는 것과 같은 원칙 -
// **전체 메시지를 로컬 버퍼 하나에 다 이어붙인 뒤 `Serial::write()`를
// 정확히 한 번만 호출**한다(그러면 `gWriteLock` 한 번의 획득 구간이
// 메시지 전체를 덮는다). `Logger`의 256바이트 버퍼는 이 전체 레지스터
// 덤프(20개 이상 필드)에 비해 너무 작아 재사용하지 않고, 여기 전용
// 버퍼+헬퍼를 따로 둔다.
kernel::uint32_t kAppendDiagStr(char* buf, kernel::uint32_t bufSize, kernel::uint32_t pos, const char* s) {
    while (*s && pos + 1 < bufSize) {
        buf[pos++] = *s++;
    }
    return pos;
}

// Serial::writeHex()와 정확히 같은 포맷("0x"+16자리 16진수)을 버퍼에
// 이어붙인다.
kernel::uint32_t kAppendDiagHex(char* buf, kernel::uint32_t bufSize, kernel::uint32_t pos, kernel::uint64_t value) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    char tmp[19] = "0x0000000000000000";
    for (kernel::uint32_t i = 0; i < 16; ++i) {
        tmp[17 - i] = kHexDigits[(value >> (i * 4)) & 0xF];
    }
    return kAppendDiagStr(buf, bufSize, pos, tmp);
}

// [PN-F443FE73, SP-677210E6 "NMI 활용"] kPanic(InterruptFrame*)와
// NMI WatchdogTrap 분기 양쪽이 공유하는 레지스터 덤프 - 원래
// kPanic() 본문 그대로, 재사용을 위해 이름만 붙여 뺐다(로직 변경
// 없음). `header`는 호출부가 이미 완성해 둔 안내 문구(예: "\nminicore:
// NMI - debug forced halt\n") - 위 SMP 안전성 이유로 이 함수 안에서
// 별도로 Serial::write하지 않고 반드시 이 함수의 버퍼 안에 함께
// 담아 단 한 번의 Serial::write()로 내보낸다(호출부가 헤더와 덤프를
// 각각 따로 write하면 그 사이 틈에서 여전히 다른 코어가 끼어들 수
// 있다 - PN-F7EBD6F5).
void kPrintFrameDiagnostics(kernel::InterruptFrame* frame, const char* header) {
    constexpr kernel::uint32_t kBufSize = 1024;  // 헤더+23개 필드 전체를 넉넉히 담음
    char buf[kBufSize];
    kernel::uint32_t pos = 0;

    pos = kAppendDiagStr(buf, kBufSize, pos, header);

    pos = kAppendDiagStr(buf, kBufSize, pos, "  vector=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->vector);
    pos = kAppendDiagStr(buf, kBufSize, pos, " error_code=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->errorCode);
    pos = kAppendDiagStr(buf, kBufSize, pos, "\n  rip=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rip);
    pos = kAppendDiagStr(buf, kBufSize, pos, " cs=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->cs);
    pos = kAppendDiagStr(buf, kBufSize, pos, " rflags=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rflags);
    pos = kAppendDiagStr(buf, kBufSize, pos, "\n");

    if (frame->vector == 14) {  // Page Fault
        pos = kAppendDiagStr(buf, kBufSize, pos, "  cr2(fault addr)=");
        pos = kAppendDiagHex(buf, kBufSize, pos, kReadCr2());
        pos = kAppendDiagStr(buf, kBufSize, pos, "\n");
    }

    // PN-63BCFE45 진단 강화 - rip/cs/rflags/cr2만으로는 이번 멀티
    // 프로세스 크래시(특히 #DB/TF처럼 보이는 증상)가 진짜 레지스터
    // 상태인지 스택/프레임 손상의 2차 증상인지 구분이 안 돼서, 이미
    // InterruptFrame에 있는 전체 GPR + rsp/rbp + 그 순간의 CR3까지
    // 함께 덤프하도록 넓혔다 - 앞으로의 크래시 진단에도 일반적으로
    // 유용하다.
    pos = kAppendDiagStr(buf, kBufSize, pos, "  rax=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rax);
    pos = kAppendDiagStr(buf, kBufSize, pos, " rbx=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rbx);
    pos = kAppendDiagStr(buf, kBufSize, pos, " rcx=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rcx);
    pos = kAppendDiagStr(buf, kBufSize, pos, " rdx=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rdx);
    pos = kAppendDiagStr(buf, kBufSize, pos, "\n  rsi=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rsi);
    pos = kAppendDiagStr(buf, kBufSize, pos, " rdi=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rdi);
    pos = kAppendDiagStr(buf, kBufSize, pos, " rbp=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rbp);
    pos = kAppendDiagStr(buf, kBufSize, pos, " rspOld=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->rspOld);
    pos = kAppendDiagStr(buf, kBufSize, pos, "\n  ssOld=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->ssOld);
    pos = kAppendDiagStr(buf, kBufSize, pos, " r8=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r8);
    pos = kAppendDiagStr(buf, kBufSize, pos, " r9=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r9);
    pos = kAppendDiagStr(buf, kBufSize, pos, " r10=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r10);
    pos = kAppendDiagStr(buf, kBufSize, pos, "\n  r11=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r11);
    pos = kAppendDiagStr(buf, kBufSize, pos, " r12=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r12);
    pos = kAppendDiagStr(buf, kBufSize, pos, " r13=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r13);
    pos = kAppendDiagStr(buf, kBufSize, pos, " r14=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r14);
    pos = kAppendDiagStr(buf, kBufSize, pos, "\n  r15=");
    pos = kAppendDiagHex(buf, kBufSize, pos, frame->r15);
    {
        kernel::uint64_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        pos = kAppendDiagStr(buf, kBufSize, pos, " cr3=");
        pos = kAppendDiagHex(buf, kBufSize, pos, cr3);
    }
    pos = kAppendDiagStr(buf, kBufSize, pos, "\n");

    buf[pos] = '\0';
    kernel::Serial::write(buf);
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

    // [순서 변경, 2026-09-18, PN-49C2F890] DR6는 CPU가 자동으로
    // 클리어하지 않는다 - 핸들러가 명시적으로 비워야 한다(SDM Vol.3
    // §17.2 요구사항, 안 비우면 다음 #DB에서도 낡은 상태 비트가 그대로
    // 남는다). **반드시 콜백 호출 전에** 비워야 한다 - 콜백
    // (kHandleUserBreakpointHit)이 이제 Scheduler::parkCurrent()로
    // 실제 파킹할 수 있고, 재개는 나중에 로드밸런싱이 고른 **다른
    // 코어**에서 일어날 수 있다(§3.2 갈래② - 반드시 원래 코어로
    // 되돌아온다는 보장 없음). 예전처럼 "콜백 반환 후" 비우면, 파킹된
    // 콜백이 재개되는 시점에 실행 중인 코어(원래와 다를 수 있음)의
    // DR6를 잘못 지우고 원래(#DB가 실제로 발생한) 코어의 DR6는
    // 영원히 안 지워진 채로 남는다 - 콜백이 항상 그 자리에서 곧장
    // 반환하던 예전에는 이 순서 문제 자체가 드러날 수 없었다. 콜백의
    // 로직은 이미 읽어 둔 `dr6` 지역변수 값만 쓰므로 순서를 바꿔도
    // 동작에 차이가 없다.
    asm volatile("mov %0, %%dr6" : : "r"(kernel::uint64_t{0}));

    bool handled = false;
    if (gDebugCallback != nullptr) {
        handled = gDebugCallback(frame, dr6);  // true면 콜백이 처리 완료
    }

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
            kPrintFrameDiagnostics(frame, "\nminicore: NMI - debug forced halt\n");
            for (;;) {
                asm volatile("cli; hlt");
            }
        case kernel::NmiReason::WatchdogTrap:
            kPrintFrameDiagnostics(frame, "\nminicore: NMI - watchdog: this core unresponsive\n");
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

    // [PN-3081704A] "최초 1회만" 래치 - panic.cpp의 kPanic(const char*)
    // 와 공유한다. 서로 다른 코어가 거의 동시에 각자 진짜 패닉을
    // 발견하면(예: 서로 다른 essential 서비스가 비슷한 시점에 죽음)
    // 가장 먼저 도달한 쪽만 실제로 진행하고, 나머지는 즉시 cli 후
    // 조용히 멈춘다 - 먼저 패닉한 코어가 보낼 stop-the-world NMI에
    // 자신이 한창 진행 중이던 로그 출력/NMI 발신 도중 끼어드는 경합을
    // 막는다(실측 확인, PN-907C5289 - 두 PANIC 메시지가 문자 단위로
    // 뒤섞인 로그).
    if (!kernel::kTryClaimFirstPanic()) {
        asm volatile("cli");
        for (;;) {
            asm volatile("hlt");
        }
    }

    // [SP-677210E6 "디버그 강제 정지(stop the world)"] 이 코어가 진짜로
    // 패닉하는 중이다 - 다른 로그를 찍기 전에 최대한 빨리 나머지 온라인
    // 코어부터 멈춰 공유 상태 오염/로그 뒤섞임을 막는다. Lapic::isReady()
    // 확인은 극초반(ACPI/LAPIC 준비 전) 패닉을 위한 방어.
    if (kernel::Lapic::isReady()) {
        kernel::Nmi::stopAllOtherCores();
    }

    // [수정, 2026-09-18, PN-F7EBD6F5] 헤더 문구도 kPrintFrameDiagnostics
    // 안의 단일 Serial::write()에 함께 담아야 하므로(위 함수 주석
    // 참고), 여기서 먼저 로컬 버퍼에 조립해 둔다 - 예전처럼 여기서
    // 바로 Serial::write하지 않는다.
    char headerBuf[96];
    kernel::uint32_t headerPos = 0;
    headerPos = kAppendDiagStr(headerBuf, sizeof(headerBuf), headerPos, "\nminicore: PANIC - unhandled exception: ");
    if (frame->vector < 32) {
        headerPos = kAppendDiagStr(headerBuf, sizeof(headerBuf), headerPos, kExceptionNames[frame->vector]);
    } else {
        // 33-254 대역인데 registerHandler로 등록된 콜백이 없는 채
        // 인터럽트가 들어온 경우 - kExceptionNames는 CPU 예외(0-31)
        // 전용이라 그대로 인덱싱하면 엉녡한 이름이 찍힌다(예전에는
        // 이 경로 자체가 없어서 문제가 없었다 - PL-2D149D8F에서 범용
        // 벡터 디스패치를 추가하며 같이 고침).
        headerPos = kAppendDiagStr(headerBuf, sizeof(headerBuf), headerPos, "Unrouted hardware interrupt");
    }
    headerPos = kAppendDiagStr(headerBuf, sizeof(headerBuf), headerPos, "\n");
    headerBuf[headerPos] = '\0';
    kPrintFrameDiagnostics(frame, headerBuf);

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
// [신규, 2026-09-18, PN-10EE096A] 위 "필요해지면 그때"가 실사용처
// (pubreg 항목4의 다중 연결 accept 루프)가 생겨 지금이다:
//   2(waitAnyOf) - RDI=WaitAnyOfSyscallArgs*(syscall.h) - tokens/count
//                  in, resultToken/resultOutcome out. `submit()`/
//                  `wait()`와 달리 입출력 필드가 여러 개라 구조체
//                  포인터 하나로 묶는다. waitForMultipleSyscall()도
//                  이 verb 하나를 재사용(둘 다 커널 내부에서 완전히
//                  같은 구현을 공유 - QU-31402585/QU-F475C6C2/
//                  QU-C06793C2, verb를 2개로 나눌 이유가 없다).
//
// PN-16CA347D(프로세스 모델)/PN-55D24891(ring3 진입) 완료로 이 벡터는
// 이제 실제 UserThread 컨텍스트에서 실행 가능하다.
constexpr kernel::uint64_t kSyscallVerbSubmit = 0;
constexpr kernel::uint64_t kSyscallVerbWait = 1;
constexpr kernel::uint64_t kSyscallVerbWaitAnyOf = 2;
// [신규, 2026-09-18, PN-44C91D6E] `fork()` 전용 verb - Submit/Wait/
// WaitAnyOf와 달리 이 verb는 `kDispatchSyscallVerb`(위 셋의 공용
// 디스패치)를 절대 타지 않는다(아래 `kHandleSyscallTrap`이 그
// 이전에 직접 가로챔) - `syscall` 명령 경로(`kDispatchSyscallVerb`를
// 공유하는 `syscall_fastpath.cpp`)는 fork()가 필요로 하는 전체
// `InterruptFrame`을 애초에 갖고 있지 않아 이 verb 자체가 도달할
// 수 없다(process.h의 `kHandleForkSyscall` 문서 주석 참고).
constexpr kernel::uint64_t kSyscallVerbFork = 3;

// context_switch.S가 entry 함수의 자연 반환 시 호출하는 것과 같은
// 함수(scheduler.cpp) - self-terminate 트랩 특별 취급(아래 참고)이
// 재사용한다. 헤더 없이 extern "C" 링크만으로 직접 선언(scheduler.h의
// 공개 API로 노출할 만큼 범용은 아니다 - 이 파일과 context_switch.S,
// 딜따 호출부만 존재).
extern "C" void kTaskOnFallingToEnd();

// [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF] `kTaskOnFallingToEnd`
// 바로 위와 동일한 이유로 헤더 없이 직접 선언(scheduler.cpp 정의) -
// `SelfTerminateThread` 트랩 특별 취급(아래 kDispatchSyscallVerbBody)이
// 부른다. `kTaskOnFallingToEnd`와 달리 exitCode를 받는다.
extern "C" void kThreadOnFallingToEnd(kernel::int32_t exitCode);

// [신규, 2026-09-17, PN-71E50394 항목3 나머지 - SP-0666DB3C §4.4
// 체크포인트, **2026-09-19 범용화, PN-59A60413, QU-4152C857
// 설계자 답변("별도 계획으로 분리해서 구현해. 소비되지 않더라도
// 확실하게 구현해둬야해")** - 실행 중(비대기)인 UserThread도 다음
// syscall 진입 시점에 이 지점에서 자신의 pendingSignals를 확인한다
// (§9.5의 Waitable::cancel() 강제 웨이크업 경로(대기 중)와 쌍을 이루는
// "실행 중" 경로).
//
// 예전엔 Kill/Terminate 두 신호만 하드코딩으로 찾아 dispositions[]를
// 전혀 참조하지 않고 무조건 종료시켰다 - PN-485132FF가 SIGCHLD를
// 배선하려다 이 하드코딩 때문에 dispositions[]에 뭘 넣어도(Chld를
// Ignore로 설정해도) 아무 효과가 없다는 진짜 설계 공백을 발견했다.
// 이제는 **먼저 쌓인 신호부터 하나씩** dispositions[]를 실제로
// 참조해 처리한다:
//   - `Ignore`면 그 신호 하나만 조용히 소비(erase)하고 다음 신호를
//     계속 찾는다(대기열 전체를 비울 때까지, 또는 처리해야 할 신호를
//     만날 때까지).
//   - 그 외(`Default`, 그리고 `Handler`도 - 아래 참고)면 발견 즉시
//     종료로 처리한다.
// `Kill`/`Stop`은 `SignalActionHandler`(process.cpp)가 애초에
// `Ignore`로 바꾸는 걸 거부하므로(POSIX "마스킹 불가 원칙",
// signal.h `SignalDisposition` 문서 주석) 이 dispositions[] 조회
// 자체가 자연히 항상 `Default`를 돌려준다 - 예전처럼 Kill을
// 특별히 하드코딩할 필요가 없다. 부수 효과로 `Terminate`(SIGTERM,
// "핸들러로 가로챌 수 있음")도 이제 실제로 `Ignore` disposition을
// 존중한다 - 예전엔 Kill과 똑같이 무조건 종료돼 이 신호 고유의
// 마스킹 가능 성질이 지켜지지 않고 있었다(이것도 이번에 같이
// 고쳐진 버그).
//
// `Handler`는 아직 실제 ring3 디스패치 인프라(핸들러 주소를 어디에
// 저장할지, sigreturn 규약을 어떻게 둘지)가 설계되지 않아
// `SignalActionHandler`가 애초에 `NotSupported`로 거부한다(signal.h
// 참고) - 그래서 이 분기는 지금은 도달 불가능한 방어적 코드일
// 뿐이다. 도달한다면 안전한 기본값(Default와 동일하게 종료)으로
// 폴백한다 - "말없이 무시"보다 "일단 안전하게 종료"가 이 프로젝트의
// 일관된 선택(SpawnProcess flags 검증 등과 같은 원칙).
//
// [수정, 2026-09-21, PN-1DFCB337; 갱신, SP-A252E82F] 이 함수는
// `int 0x80`(진짜 InterruptFrame이 있음)과 `syscall` 명령 빠른 경로
// (syscall_fastpath.cpp, InterruptFrame 자체가 없음 - isr_common_stub을
// 아예 안 거쳐 일반 인터럽트 디스패치 스택으로도 안 옮겨감) 양쪽에서
// 불린다. 예전엔 두 경로 모두 `sti`+`for(;;) hlt`로 반환하지
// 않았는데, `int 0x80` 쪽만 `kTerminateFaultingUserTask`와 똑같이
// 이 인터럽트 호출 프레임을 그냥 버려 원래 스택으로 못 돌아가는
// 결함이 있었다(자세한 근거는 그 함수의 문서 주석/PN-1DFCB337 참고) -
// `syscall` 빠른 경로는 애초에 isr_common_stub의 스택 스왑 자체를
// 거치지 않아 이 결함과 무관하다. `frame`이 있으면(=`int 0x80`)
// `Scheduler::parkFromISR()`로 원래 스택 복귀까지 함께 마치며 idle로
// 전환하고, `frame`이 null이면(=`syscall` 빠른 경로) 예전 `sti`+
// `hlt` 그대로 유지한다(스택 스왑이 애초에 없었으므로 안전 - 다음
// 스케줄러 틱이 다른 Task로 전환할 때까지 대기하는 원래 방식 그대로).
bool kCheckSignalCheckpoint(kernel::InterruptFrame* frame) {
    auto* thread = static_cast<kernel::UserThread*>(kernel::Scheduler::currentTask());
    if (!thread) {
        return false;
    }
    kernel::SharedPtr<kernel::Process> process = thread->process.lock();
    if (!process) {
        return false;
    }
    for (;;) {
        auto* slot = process->pendingSignals.find([](const kernel::PendingSignal&) { return true; });
        if (!slot) {
            return false;  // 대기 중인 신호 없음(또는 전부 Ignore로 소비함)
        }
        const kernel::SignalNumber number = slot->value.number;
        const kernel::uint32_t index = static_cast<kernel::uint32_t>(number);
        const kernel::SignalDisposition disposition =
            (index < kernel::kSignalCount) ? process->dispositions[index] : kernel::SignalDisposition::Default;
        // find()가 준 슬롯 참조로 disposition을 다 읽은 뒤에 소비한다 -
        // Ignore든 종료든 이 신호 하나는 "확인됨"이므로 항상 지운다.
        process->pendingSignals.erase(slot);
        if (disposition == kernel::SignalDisposition::Ignore) {
            continue;  // 이 신호는 조용히 소비 - 다음 신호를 계속 찾는다.
        }
        // Default 또는 (아직 도달 불가능한) Handler 폴백 - 종료.
        kernel::Logger::info("minicore: signal checkpoint - terminating UserThread (pending signal, non-Ignore disposition)");
        kTaskOnFallingToEnd();
        if (frame) {
            kernel::Scheduler::parkFromISR(thread, frame);  // 반환하지 않음
        }
        asm volatile("sti");
        for (;;) {
            asm volatile("hlt");
        }
    }
}

}  // namespace

namespace kernel {

// syscall.h 선언 참고 - int 0x80(아래 kHandleSyscallTrap)과 `syscall`
// 명령(syscall_fastpath.cpp의 kHandleSyscallFast) 양쪽이 공유하는 공용
// 디스패치. **PN-124C105B("syscall 명령 경로") 신설 시 int 0x80
// 핸들러 하나에만 있던 이 로직을 그대로 옥겨 온 것뿐** - 동작 자체는
// 전혀 바뀌지 않았다.
// [신규, 2026-09-18, PN-22E5E9E7 항목7] kDispatchSyscallVerb의 실제
// 스위치 로직 - 원래 이름 그대로였던 것을 FS_BASE 진입/반환 스왑을
// 감싸기 위해 내부 헬퍼로 뺐다(아래 kDispatchSyscallVerb 참고). 여러
// return 지점(각 verb 분기)이 있어 "돌아가기 직전 매번 되돌린다"는
// 방식은 하나만 빠뜨려도 조용히 깨지는 함정이라, 단일 호출 지점을
// 감싸는 래퍼 하나로 통일했다 - self-terminate(kSyscallVerbSubmit
// 분기의 sti+hlt 루프)만 유일하게 이 함수 밖으로 반환하지 않는다.
namespace {
uint64_t kDispatchSyscallVerbBody(uint64_t verb, uint64_t arg0, uint64_t arg1, kernel::InterruptFrame* frame) {
    // [PN-71E50394 항목3 나머지] 어떤 verb든 실제로 처리하기 전에
    // 먼저 체크포인트를 통과해야 한다 - Kill/Terminate가 걸려 있으면
    // 이 호출에서 반환하지 않는다(아래 kCheckSignalCheckpoint 참고).
    // frame은 kCheckSignalCheckpoint 문서 주석 참고(int 0x80이면
    // 실제 값, syscall 빠른 경로면 null).
    kCheckSignalCheckpoint(frame);
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
                // [수정, 2026-09-21, PN-1DFCB337] kCheckSignalCheckpoint와
                // 동일한 이유 - frame이 있으면(int 0x80) parkFromISR로
                // 이 인터럽트분의 카운터까지 정확히 닫는다.
                if (frame) {
                    auto* self = kernel::Scheduler::currentTask();
                    if (self) {
                        kernel::Scheduler::parkFromISR(self, frame);  // 반환하지 않음
                    }
                }
                asm volatile("sti");
                for (;;) {
                    asm volatile("hlt");
                }
            }
            // [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF]
            // `SelfTerminateThread`도 self-terminate와 완전히 같은 이유로
            // 절대 ring3에 복귀하면 안 된다(syscall.h의
            // kSyscallEndpointSelfTerminateThread 문서 주석 참고) - 다만
            // exitCode(SelfTerminateThreadArgs 하나뿐인 필드)를 유저
            // 포인터(arg1)에서 읽어야 한다는 점만 다르다. 포인터가
            // 유효하지 않으면(잘못된 유저 프로그램) 방어적으로 0으로
            // 취급한다 - 이 syscall 자체가 반환하지 않으므로 별도
            // 에러 코드를 돌려줄 방법이 없다.
            if (endpointId == kSyscallEndpointSelfTerminateThread) {
                kernel::int32_t exitCode = 0;
                auto* current = kernel::Scheduler::currentTask();
                if (arg1 != 0 && current && current->isUserLevel) {
                    auto* callerThread = static_cast<kernel::UserThread*>(current);
                    if (kernel::Paging::isUserRangeValid(arg1, sizeof(kernel::SelfTerminateThreadArgs),
                                                          callerThread->userPml4Phys)) {
                        exitCode = reinterpret_cast<kernel::SelfTerminateThreadArgs*>(arg1)->exitCode;
                    }
                }
                kThreadOnFallingToEnd(exitCode);
                // [수정, 2026-09-21, PN-1DFCB337] 위 kSyscallEndpointSelfTerminate
                // 분기와 동일한 이유 - frame이 있으면(int 0x80)
                // parkFromISR로 이 인터럽트분의 카운터까지 정확히 닫는다.
                if (frame && current) {
                    kernel::Scheduler::parkFromISR(current, frame);  // 반환하지 않음
                }
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
        case kSyscallVerbWaitAnyOf: {
            // [신규, 2026-09-18, PN-10EE096A] wait()/submit()과 마찬가지로
            // 이 verb 자체가 트랩을 건 UserThread의 실행 흐름에서
            // **동기적으로** 처리된다(AsyncTaskHandler::onExec()처럼
            // 나중에 리액터 컨텍스트에서 실행되는 게 아님) - 그래서
            // channel.cpp의 kValidateUserBuffer(submitterTask 체이닝,
            // 비동기 컨텍스트가 CR3 재동기화 문제를 겪는 것에 대한
            // 대응)를 재사용할 필요 없이, 지금 이 순간의
            // Scheduler::currentTask()가 곧 그 포인터의 실제 소유자다.
            auto* thread = static_cast<UserThread*>(Scheduler::currentTask());
            auto* args = reinterpret_cast<WaitAnyOfSyscallArgs*>(arg0);
            if (!thread || !args) {
                return 0;
            }
            if (!Paging::isUserRangeValid(reinterpret_cast<uint64_t>(args),
                                           sizeof(WaitAnyOfSyscallArgs), thread->userPml4Phys)) {
                return 0;
            }
            if (args->count == 0 || args->tokens == nullptr ||
                !Paging::isUserRangeValid(reinterpret_cast<uint64_t>(args->tokens),
                                           sizeof(AsyncTaskManageCode) * static_cast<uint64_t>(args->count),
                                           thread->userPml4Phys)) {
                return 0;
            }
            const Syscall::MultiWaitResult result = Syscall::waitAnyForMultipleSyscall(args->tokens, args->count);
            args->resultToken = result.token;
            args->resultOutcome = result.outcome;
            return 1;
        }
        default:
            return 0;  // 알 수 없는 verb - 실패로 취급(v1, 새 DC 불필요 수준)
    }
}

}  // namespace

// [신규, 2026-09-18, PN-22E5E9E7 항목7, SP-29D652AA §5.3] syscall.h
// 선언 그대로의 공개 진입점 - int 0x80(아래 kHandleSyscallTrap)과
// `syscall` 명령(syscall_fastpath.cpp) 양쪽이 여전히 이 이름으로
// 부른다. 실제 진입 시점 FS_BASE는 아직 이 UserThread의 ring3 값
// (`UserThread::userFsBase`)이다 - swapgs/syscall 진입도 int 게이트
// 진입도 FS_BASE를 안 건드리기 때문(syscall_entry.S 문서 주석 참고) -
// 그래서 위 kDispatchSyscallVerbBody가 실행되기 전에 먼저 이 Task
// 자신의 커널 TCB(`kSyncFsBase`, 항목3에서 Task 디스패치용으로 만든
// 그 함수를 여기서도 재사용)로 되돌리고, 처리가 끝나 ring3로 돌아가기
// 직전에 다시 유저 값으로 되돌린다(`kSyncFsBaseToUser`, 항목6이 만든
// `userFsBase`를 소비하는 첫 지점). self-terminate(kSyscallVerbSubmit
// 분기)만 이 함수 밖으로 반환하지 않아 유저 복원이 실행되지 않는데,
// 그 Task는 어차피 다시는 ring3로 안 돌아가므로 정확히 의도한 동작이다.
uint64_t kDispatchSyscallVerb(uint64_t verb, uint64_t arg0, uint64_t arg1, InterruptFrame* frame) {
    Task* current = Scheduler::currentTask();
    if (current) {
        kSyncFsBase(current);
    }
    const uint64_t result = kDispatchSyscallVerbBody(verb, arg0, arg1, frame);
    if (current && current->isUserLevel) {
        kSyncFsBaseToUser(static_cast<UserThread*>(current));
    }
    return result;
}

}  // namespace kernel

namespace {

void kHandleSyscallTrap(kernel::InterruptFrame* frame) {
    // [신규, 2026-09-18, PN-44C91D6E] fork()는 `kDispatchSyscallVerb`
    // 공용 디스패치(verb/arg0/arg1 세 값만 받음)를 타지 않는다 - 자식
    // 재개에 필요한 전체 `InterruptFrame`을 그대로 넘겨야 해서 여기서
    // 직접 가로챈다. `kDispatchSyscallVerb`가 body 호출 앞뒤로 하는
    // FS_BASE 스왑(진입 시 커널 TCB로, 반환 직전 유저 값으로)과
    // 신호 체크포인트를 여기서도 똑같이 반복한다 - 이 verb만 그 공용
    // 래퍼를 우회하므로 직접 하지 않으면 빠진다.
    if (frame->rax == kSyscallVerbFork) {
        kernel::Task* current = kernel::Scheduler::currentTask();
        if (current) {
            kernel::kSyncFsBase(current);
        }
        kCheckSignalCheckpoint(frame);
        kernel::kHandleForkSyscall(frame);
        if (current && current->isUserLevel) {
            kernel::kSyncFsBaseToUser(static_cast<kernel::UserThread*>(current));
        }
        return;
    }
    frame->rax = kernel::kDispatchSyscallVerb(frame->rax, frame->rdi, frame->rsi, frame);
}

// [QU-04C420BF, SP-0666DB3C, PN-71E50394 항목3] ring3(유저) 코드가
// 커널이 해결할 수 없는 #PF/#UD를 냈을 때 커널 전체를 kPanic으로
// 정지시키는 대신 그 프로세스 하나만 죽인다. 폴트난 그 ring3
// 명령어를 안전하게 재개할 방법이 없어 §4.4의 체크포인트 방식(다음
// syscall 진입/ring3 재진입 시점에 pendingSignals 확인)이 적용될 수
// 없다 - kTaskOnFallingToEnd()(Zombie 표시 + Syscall::submitDetached로
// 리액터에 정리 위임)를 부른 뒤 이 함수에서 반환하지 않는다는 큰
// 그림은 설계자 확인(2026-09-16, "그래 이렇게 해")에서 확정된 그대로다.
//
// [수정, 2026-09-21, PN-1DFCB337; 갱신, SP-A252E82F] 다만 "반환하지
// 않는" 구체적인 방법은 그 확인 이후(2026-09-20) 이 프로젝트가 더
// 나은 패턴을 이미 확립했다 - `Scheduler::parkFromISR()`(#DB ISR의
// `kHandleUserBreakpointHit`, debug_session.cpp 참고). 원래 이
// 함수는 `sti` + `for(;;) hlt`로 "다음 스케줄러 틱이 다른 Task로
// 전환할 때까지 대기"했는데, 이 방식은 이 #PF/#UD 진입이 `isr_common_stub`
// 에서 스왑해 둔 일반 디스패치 스택(당시 gInterruptDepth 카운터 기반
// 설계, 지금은 SP-A252E82F로 카운터 없는 무조건 스왑 설계로
// 교체됨)을 원래 Task 스택으로 **영원히** 되돌리지 못한다 - 이 hlt
// 루프의 C 콜스택(`kIsrHandler`←`isr_common_stub`) 자체가, 나중에
// 다른 인터럽트가 이 코어를 다른 Task로 전환하는 순간 통째로 버려지기
// 때문이다(그 전환은 그 "다른 인터럽트" 자신의 진입/이탈만 짝을
// 맞출 뿐, 이 hlt 루프에 갇혀 있던 원래 #PF/#UD 진입의 이탈은 코드상
// 존재해도 실행될 기회를 영원히 잃는다). PN-D7B66FE4(코어별 전용
// 인터럽트 디스패치 스택)가 바로 이 스택 전환의 정확성에 의존하는데,
// gdb 실측으로 이 함수가 그 짝을 실제로 못 맞추고 있음을 확인했다
// (자세한 근거는 PN-1DFCB337). `parkFromISR()`은 이 hlt 루프 대신
// `kContextSwitchFromISR()`로 **곧장** idle로 전환해 그 호출 하나로
// 이 진입의 마무리까지 함께 마친다 - "절대 ring3로 안 돌아간다"는
// 원래 확정된 요구사항은 그대로 지키면서(idle 전환도 iretq로 ring3가
// 아니라 idle의 재개 지점으로 감), 스택 스왑 불일치 문제만 없앤다.
// 게다가 idle이 곧 `AsyncReactor::drainOnce()`를 도는 자리라, 방금
// 제출한 self-terminate 비동기 작업도 다음 스케줄러 틱을 기다리지
// 않고 더 빨리 처리된다.
void kTerminateFaultingUserTask(kernel::SignalNumber signal, kernel::InterruptFrame* frame) {
    auto* thread = static_cast<kernel::UserThread*>(kernel::Scheduler::currentTask());
    // [수정, 2026-09-17, PN-E2A114C1] `thread->process`가 이제
    // `WeakPtr<Process>`라 `.lock()`으로 유효성을 확인해야 한다.
    if (thread) {
        if (kernel::SharedPtr<kernel::Process> proc = thread->process.lock()) {
            proc->raiseSignal(signal);
        }
    }
    kTaskOnFallingToEnd();
    if (thread) {
        kernel::Scheduler::parkFromISR(thread, frame);  // 반환하지 않음
    }
    // 이론상 도달 불가 - 호출부(kIsrHandler)가 이미
    // `frame->cs == kGdtUserCodeSelector`(ring3)를 확인한 뒤에만 이
    // 함수를 부르므로 `Scheduler::currentTask()`가 null일 수 없다.
    // 그래도 방어적으로 예전 방식(다음 스케줄러 틱까지 안전 대기)을
    // 최후의 안전망으로 남겨 둔다.
    asm volatile("sti");
    for (;;) {
        asm volatile("hlt");
    }
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
    // [신규, 2026-09-21, PN-584DB994, 설계자 지시; 갱신, SP-A252E82F]
    // "인터럽트가 발생하면 가장 먼저 해야 할 일은 현재 Task의 tcb에
    // 인터럽트 프레임을 캡처해 진행 상황을 보존하는 것" - 벡터별
    // 분기(EOI, 스케줄링 결정 등)보다 먼저, 무조건 이 자리에서 한다.
    // 단 이 인터럽트가 중첩(다른 인터럽트 처리 도중)이 아닐 때만 -
    // 중첩이면 `frame`이 원래 Task의 진짜 재개 지점이 아니라 "바깥쪽
    // 인터럽트 처리 도중 어딘가"를 가리켜, 그걸 tcb에 담으면 오히려
    // 커널 내부 지점으로 오염시킨다(scheduler.h의 `captureCurrentFrame()`
    // 문서 주석 참고).
    //
    // 일반(마스커블) 벡터는 전체 저장소 `sti` 전수 조사로 확인한 대로
    // 절대 서로 중첩되지 않으므로(IF=0 유지) 항상 outermost다 - 판단할
    // 것도 없이 무조건 캡처한다. 회피 불가능한 예외/NMI(#DB/NMI/#DF/
    // #PF/#MC, 전부 하드웨어 IST로 격리됨)만 진짜 중첩 가능성이 있어,
    // 이 인터럽트가 트랩한 시점의 rsp(`frame->rspOld`)가 이미 어느
    // 인터럽트 스택 위였는지를 직접 확인해야 한다 - 그 위였다면 다른
    // 인터럽트 처리 도중 끼어든 것(중첩), 아니라면 Task를 직접
    // 인터럽트한 것(outermost)이다.
    const bool isIstVector = (frame->vector == kDebugVector) || (frame->vector == kNmiVector) ||
                              (frame->vector == kDoubleFaultVector) || (frame->vector == kPageFaultVector) ||
                              (frame->vector == kMachineCheckVector);
    const bool isOutermost =
        !isIstVector || !kIsAddressOnAnyInterruptStack(frame->rspOld, kernel::Scheduler::currentCoreIndex());
    if (isOutermost) {
        kernel::Scheduler::captureCurrentFrame(frame);
    }
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
    if (frame->vector == kPageFaultVector) {
        // [신규, 2026-09-21, SP-A252E82F §2, QU-65761CB0 확정] #PF는
        // IST5라 항상 유효한 전용 스택에서 실행되지만, IST 자체는
        // 재진입 안전하지 않다(재진입하면 같은 고정 top으로 rsp가
        // 되돌아가 진행 중이던 첫 #PF 프레임을 덮어씀 - gdt.h 참고).
        // handlePageFault()는 항상 영구 매핑된 커널 구조체만 건드리게
        // 설계돼 있으므로, 이 재진입은 "막아야 할 위험"이 아니라
        // "그 설계 불변식이 깨졌다는 확정적 버그 신호"다(설계자 지시) -
        // 복구를 시도하지 않고 즉시 rip/cr2를 남기고 멈춘다.
        const kernel::uint32_t coreIndex = kernel::Scheduler::currentCoreIndex();
        if (gInPageFaultHandler[coreIndex]) {
            kPanic(frame);  // 반환하지 않음 - #PF 핸들러 도중 #PF 재진입은 그 핸들러 코드의 버그
        }
        gInPageFaultHandler[coreIndex] = 1;
        const kernel::uint64_t faultAddr = kReadCr2();
        const bool handled = kernel::Paging::handlePageFault(faultAddr, frame->errorCode);
        gInPageFaultHandler[coreIndex] = 0;  // 아래 두 미반환 경로 전에 반드시 먼저 내린다
        if (handled) {
            return;
        }
        // [QU-04C420BF, PN-71E50394 항목3] 온디맨드 매핑으로도 못 고친
        // 진짜 세그폴트 - ring3(유저 코드)에서 난 것이면 커널 전체를
        // 패닉시키지 않고 그 프로세스만 죽인다. ring0(커널 자신)
        // 폴트는 진짜 커널 버그이므로 그대로 아래 kPanic(frame)으로
        // 떨어진다(동작 변경 없음).
        if (frame->cs == kernel::kGdtUserCodeSelector) {
            kTerminateFaultingUserTask(kernel::SignalNumber::Segv, frame);
            // kTerminateFaultingUserTask는 절대 반환하지 않는다(위 주석).
        }
    }
    if (frame->vector == 6) {  // #UD(Invalid Opcode) - [QU-04C420BF, PN-71E50394 항목3]
        if (frame->cs == kernel::kGdtUserCodeSelector) {
            kTerminateFaultingUserTask(kernel::SignalNumber::IllegalInstruction, frame);
            // 반환하지 않음 - ring0의 #UD(진짜 커널 버그)는 이 분기에
            // 안 걸리고 그대로 아래 kPanic(frame)으로 떨어진다.
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
