#ifndef MINICORE_KERNEL_GDT_H
#define MINICORE_KERNEL_GDT_H

#include "libkenv/types.h"

namespace kernel {

constexpr uint16_t kGdtKernelCodeSelector = 0x08;
constexpr uint16_t kGdtKernelDataSelector = 0x10;

// ring3 유저 코드/데이터 세그먼트(PN-124C105B, SP-04EE2A18 "유저랜드
// ABI" 절) - RPL=3을 이미 OR해 둔 완성 셀렉터 값이다(그대로 CS/SS에
// 적재). 배치가 0x18(데이터)/0x20(코드) 순서인 이유는 x86_64
// `sysretq`의 하드웨어 규약 때문 - STAR[63:48]=0x10으로 두면
// SYSRET이 CS=STAR[63:48]+16=0x20, SS=STAR[63:48]+8=0x18을 자동으로
// 골라 쓴다(Intel SDM Vol.2 SYSRET 설명, 32비트 호환 코드 세그먼트
// 자리는 이 프로젝트가 32비트 유저 코드를 지원하지 않아 생략 -
// STAR[63:48]+0 자리가 비게 되지만 sysretq는 그 자리를 쓰지 않는다).
constexpr uint16_t kGdtUserDataSelector = 0x18 | 3;
constexpr uint16_t kGdtUserCodeSelector = 0x20 | 3;

// 런타임 GDT + 코어별 TSS(DC-3D3212A4/QU-4E00C118, 설계자 지시,
// 2026-09-14 - "TSS+IST를 정식 구현하여 가드 페이지가 실제 진단
// 로그를 남기게 만들고, TSS+IST가 다른 곳에서도 활용 할 수 있도록
// 설계하여 설계안을 제출하라"). x86 하드웨어 태스크 스위칭(레지스터
// 상태 전체를 하드웨어가 통째로 갈아 끼우는 방식)은 여전히 안 쓴다
// (PL-2D3184BC 3번 결정 그대로 유효) - 이 TSS는 오직 두 가지 용도로만
// 쓴다:
//   1. IST(Interrupt Stack Table) - 지금 스택이 고장나 있어도(예:
//      Task 커널 스택 오버플로우가 가드 페이지에 부딪힌 경우) CPU가
//      특정 예외 벡터에 한해 항상 유효한 별도 스택으로 강제 전환하게
//      한다. 지금은 IST1(#DF 전용)만 쓴다 - 나머지 IST2-7은 향후
//      다른 벡터(NMI/#MC 등)가 필요해지면 같은 방식으로 확장한다.
//   2. RSP0 - 향후 유저랜드가 생기면 ring3->ring0 전환(syscall 진입
//      등) 시 커널 스택 포인터로 재사용할 자리(아직 안 씀, 필드만
//      존재).
//
// 부팅 시 boot.S가 이미 최소 GDT(null/code64/data, gdt64)를 실어
// long mode에 들어왔지만, 그 GDT엔 TSS 디스크립터를 넣을 자리가
// 없다 - Gdt::init()이 코드/데이터 디스크립터는 boot.S와 완전히
// 같은 내용으로 유지한 채(그래서 CS 재적재/far jump가 불필요 -
// GDTR을 바꿔도 CPU가 CS 히든 캐시를 스스로 재적재하진 않으므로,
// 같은 셀렉터 위치에 같은 내용만 있으면 안전하다), 코어마다 하나씩
// TSS 디스크립터 슬롯(kAcpiMaxCpus개, acpi.h)을 추가한 새 GDT로
// 교체한다.
//
// IDT/TSS 모두 "테이블 내용은 전역 하나, 포인터 레지스터(GDTR/IDTR/
// TR)는 코어마다 별도"라는 같은 원칙을 따른다(idt.cpp의 Idt::init()/
// reloadOnThisCore()와 정확히 같은 패턴) - init()은 BSP가 테이블을
// 만들고 자기 몫의 GDTR도 적재하고, reloadOnThisCore()/
// loadTssForThisCore()는 이미 만들어진 테이블에 각 코어(AP 포함)가
// 자기 GDTR/TR을 개별적으로 맞추는 역할만 한다.
class Gdt {
public:
    // BSP가 부팅 중 한 번만 호출한다(Idt::init() 근처). kAcpiMaxCpus개의
    // TSS 디스크립터 슬롯을 미리 다 만들어 두므로 이 시점에 실제
    // 코어 수를 몰라도 된다. 자기 자신(BSP)의 GDTR도 이 안에서 적재한다.
    static void init();

    // 이미 만들어진 공용 GDT로 이 코어의 GDTR을 다시 적재한다(AP
    // 전용 - AP는 ap_trampoline.S가 boot.S의 예전 gdt64를 그대로 쓴
    // 채로 kApMain에 진입하므로, TSS 디스크립터가 있는 새 GDT로
    // 자기 GDTR을 바꿔 끼워야 loadTssForThisCore가 가리킬 TSS
    // 디스크립터를 찾을 수 있다). Idt::reloadOnThisCore()와 같은
    // 타이밍에 호출한다.
    static void reloadOnThisCore();

    // 코어마다 자신의 TSS를 채우고 ltr로 적재해야 한다(TR도 코어별
    // 레지스터라 공유 불가 - BSP는 물론 각 AP도 자기 자신의
    // reloadOnThisCore()/Lapic::init() 직후 반드시 호출해야 한다).
    // 지금은 IST1(#DF 전용)만 채운다. Acpi::init()/Lapic::init()
    // 이후에만 호출 가능하다(자기 코어 인덱스를 Lapic::id()로 찾음).
    static void loadTssForThisCore();

    // 코어 인덱스(Acpi::cpuApicId와 같은 배열 인덱스)에 대응하는 TSS
    // 셀렉터 - 진단/장래 재사용 목적으로 공개해 둔다.
    static uint16_t tssSelectorForCore(uint32_t coreIndex);

    // ring3로 처음 진입하기 직전에, 그 UserThread 자신의 커널 스택
    // top을 이 코어의 TSS.RSP0에 심어 둔다(PN-124C105B/PN-16CA347D
    // 6번 - RSP0의 첫 실사용처). 이후 이 코어에서 ring3->ring0 전환
    // (인터럽트/트랩)이 일어나면 하드웨어가 자동으로 이 값을 RSP로
    // 쓴다. **v1 한계**: 코어당 한 번에 하나의 UserThread만 이 방식
    // 으로 실행됨을 전제한다 - 멀티 UserThread 스케줄링이 생기면 매
    // 디스패치마다 갱신하는 일반화가 필요하다(후속 과제,
    // PN-16CA347D 진행하며 실측).
    static void setRsp0ForThisCore(uint64_t rsp0);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_GDT_H
