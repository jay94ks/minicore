#ifndef MINICORE_KERNEL_DIAG_RING_H
#define MINICORE_KERNEL_DIAG_RING_H

#include "libkenv/types.h"

// [신규, 2026-09-23, PN-E4C6AF72, 설계자 지시(QU-136B918F 답변)]
// PN-7030D201/PN-3DDF2797 두 계획이 각자 독립적으로 발견한 "이
// 근처를 건드리는 어떤 개입이든 서로 다른 새 크래시 서명으로
// 회귀한다"는 패턴의 공통 원인을 추적하기 위한 비관측적(gdb 없이도
// 동작하는) 진단 도구 - PN-3DDF2797가 이미 확정한 "gdb 부착 자체가
// 타이밍을 바꿔 경쟁을 숨긴다"(heisenbug)는 교훈을 그대로 적용해,
// 순수 메모리 쓰기만 하는 코어별 링 버퍼에 의심 지점(일반 인터럽트
// 디스패치 스택 진입/이탈, `AsyncReactor::drainOnce()`의 스택풀
// AsyncTask 디스패치 구간)을 기록해 두고, 패닉이 실제로 발생한
// 순간에만 그 기록을 시리얼로 덤프한다 - gdb 브레이크포인트처럼
// 실행을 멈추거나 통신 프로토콜을 거치지 않으므로 이 자체가 타이밍을
// 왜곡할 가능성이 극히 낮다(단순 배열 인덱싱+대입 몇 개, 인터럽트
// 안팎 어디서 불려도 안전 - 락 없음, 코어별로 자기 슬롯만 쓴다).
namespace kernel {

enum class DiagRingEvent : uint8_t {
    EnterInterruptStack = 1,    // 일반 벡터 isr_common_stub이 디스패치 스택으로 스왑하기 직전
    LeaveInterruptStack = 2,    // 일반 벡터가 원래 스택으로 자연 복귀하기 직전
    StackfulDispatchBegin = 3,  // AsyncReactor::drainOnce()가 스택풀 AsyncTask로 kContextSwitch하기 직전
    StackfulDispatchEnd = 4,    // 그 kContextSwitch가 되돌아온 직후

    // [신규, 2026-09-23, PN-E4C6AF72 3차 실측의 "남은 것" 1번] EnterIsr~
    // StackfulDispatchBegin 사이(kIsrHandler 동적 벡터 디스패치 +
    // drainOnce() 초입)를 더 좁히기 위한 계측 - vector는 Dynamic 계열만
    // 의미 있고(frame->vector 그대로), 나머지는 0으로 채운다.
    DynamicDispatchEnter = 5,    // kIsrHandler가 gDynamicHandlers[vector]를 부르기 직전
    DynamicDispatchExit = 6,     // 그 핸들러가 정상 반환한 직후(EOI 전)
    DrainOnceTaskFound = 7,      // drainOnce()가 큐에서 task를 뽑은 직후(vector=task->subjectCode) - Cancelled 분기 이전
    DrainOnceCoroBranch = 8,     // drainOnce()가 coroHandle(코루틴) 분기를 선택한 시점 - 이 분기는 StackfulDispatchBegin이 절대 안 찍힘(정상)
    DrainOnceStackfulBranch = 9, // drainOnce()가 스택풀 분기를 선택한 시점(CR3 동기화 이전) - StackfulDispatchBegin보다 한 단계 이른 지점

    // [신규, 2026-09-24, PN-61D908EB/PN-E4C6AF72] "첫 인터럽트 이전"
    // 구간의 계측 공백을 메우기 위한 부팅 이정표 3종 - CS 오염이
    // 이미 "첫 인터럽트 이전"임을 diag_ring으로 확정했지만, 그 구간
    // 안에서 정확히 어디인지는 여전히 미상이었다(정적 코드 리뷰로
    // gdt.cpp의 ltr 경로는 무죄로 확인됨, PN-61D908EB 참고). 이
    // 이정표들의 `extra` 필드에 그 순간의 실제 rsp를 실어, 크래시
    // 시점의 InterruptFrame 주소와 겹치거나 인접한지 직접 대조한다.
    BootGdtInitDone = 10,   // kmain()의 Gdt::init() 직후
    BootTssLoadDone = 11,   // kmain()의 Gdt::loadTssForThisCore() 직후
    BootBeforeSti = 12,     // kmain()의 asm("sti") 직전
};

// event가 일어난 시점의 rsp/vector를 기록한다 - vector는 Enter/Leave
// 계열에서만 의미 있고(StackfulDispatch 계열은 0으로 채움), 코어별로
// 독립된 슬롯에 기록하므로 락이 필요 없다(자기 코어 외 다른 코어의
// 슬롯을 쓰는 호출부는 없음).
// [신규, 2026-09-24, PN-61D908EB/PN-E4C6AF72] `extra` - Enter/Leave
// InterruptStack이 그 순간의 InterruptFrame::cs를 실어 보낸다(다른
// 이벤트는 기본값 0, 기존 호출부는 안 바뀜) - iretq에서 CS/SS가
// TSS 셀렉터(0x38)로 오염되는 걸 실측한 뒤, "정확히 어느 ISR
// 진입/이탈 시점에 오염이 이미 있었는지"를 다음 재현에서 바로
// 잡기 위한 것.
void kDiagRingLog(DiagRingEvent event, kernel::uint32_t coreIndex, kernel::uint32_t vector, kernel::uint64_t rsp,
                   kernel::uint64_t extra = 0);

// 패닉 시점에 호출 - coreIndex 하나의 최근 기록을 시리얼로 덤프한다.
void kDiagRingDump(kernel::uint32_t coreIndex);

}  // namespace kernel

#endif  // MINICORE_KERNEL_DIAG_RING_H
