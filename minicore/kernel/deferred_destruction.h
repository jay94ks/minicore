#ifndef MINICORE_KERNEL_DEFERRED_DESTRUCTION_H
#define MINICORE_KERNEL_DEFERRED_DESTRUCTION_H

#include "libkenv/types.h"

// [신규, 2026-09-20, SP-5130284C, PN-4137C88C/QU-A07B9019] SharedPtr<T>가
// 인터럽트 컨텍스트(onTick()/onForcedMigration() 등)에서 마지막 강한
// 참조를 잃으면 커스텀 삭제자(예: Process::destroy())가 그 자리에서
// 동기 실행돼, 그 삭제자가 잡는 Spinlock(PageFrameAllocator/
// ResourceGroup::lock/주소공간 락 등, 전부 재진입 불가)을 이 코어의
// 다른 코드가 이미 쥔 채였다면 자기 자신을 영원히 멈출 수 있었다
// (PN-4137C88C가 실측 확인). 이 파일은 그 소멸을 인터럽트 컨텍스트
// 밖(AsyncReactor::drainOnce())으로 미루는 공용 매커니즘의 커널 레이어
// 구현이다 - libkenv/shared_ptr.h는 이 파일을 모른다(순환 include
// 방지 - task.h가 이미 shared_ptr.h를 include하고 scheduler.h가
// task.h를 include하므로, shared_ptr.h가 scheduler.h/이 파일을 다시
// include하면 순환이 된다). 대신 shared_ptr.h가 선언해 둔
// gShouldDeferHeavyDestruction/gPushDeferredDestructionHook 함수포인터
// 전역을 kInitDeferredDestruction()이 부팅 시 이 파일의 실제 구현으로
// 채운다(ControlBlockBase::_destroyOwned/_freeSelf와 동일한 타입 소거
// 트램폴린 패턴).

namespace kernel {

// 부팅 초기(kMain) 1회 - libkenv의 함수포인터 훅을 이 파일의 실제
// 구현으로 등록한다.
void kInitDeferredDestruction();

// AsyncReactor::drainOnce()(안전한 비인터럽트 컨텍스트)가 매 호출마다
// 먼저 확인한다 - Rcu::drainCallbacksOnThisCore()와 동일한 관례.
// 인터럽트 컨텍스트에서 지연됐던 SharedPtr 소멸(원래 releaseStrong()이
// 했을 일 전체 - _destroyOwned + releaseWeak, SP-5130284C §3.2-a)을
// 지금 이 안전한 자리에서 대신 수행한다.
void kDrainDeferredDestructions();

}  // namespace kernel

// isr.S/context_switch.S가 직접 호출하는 C 링키지 리프 함수들 - 반드시
// 아주 가볍고, 자기 자신의 balanced call/ret 외에는 스택을 건드리지
// 않아야 한다(InterruptFrame 구성 도중/직후, 또는 TaskTcb 블록을 막
// 새 스택으로 삼기 직전처럼 rsp가 민감한 지점에서 불리므로 - 정확한
// 삽입 위치의 근거는 각 .S 파일의 주석 참고).
//
// [갱신, 2026-09-21, SP-A252E82F "인터럽트 컨텍스트 재설계"] 예전
// `gInterruptDepth` 카운터 기반 설계(임의 깊이 중첩을 소프트웨어로
// 지원)를 완전히 폐기했다 - 전체 저장소 `sti` 전수 조사로 확인한
// 대로, 일반(마스커블) 인터럽트끼리는 애초에 절대 중첩되지 않는다
// (`kIsrHandler` 실행 구간 내내 IF=0으로 유지됨). 유일하게 실재하는
// "중첩"은 IF와 무관하게 강제로 발생하는 회피 불가능한 예외(#PF/
// NMI/#DF/#MC/#DB)뿐이고, 이들은 전부 하드웨어 IST로 격리된다
// (gdt.cpp, `#PF`는 이번 갱신으로 IST5 추가) - 그래서 **일반 벡터의
// isr_common_stub만** 이 두 함수를 쓴다(IST 벡터는 하드웨어가 이미
// 전용 스택으로 전환해 뒀으므로 이 소프트웨어 스왑 자체가 필요
// 없다 - isr.S가 벡터 번호로 분기해 IST 벡터는 아예 이 호출을
// 건너뛴다). 일반 벡터끼리는 절대 중첩되지 않으므로 카운터 없이
// "매번 무조건 스왑"하면 충분하다 - `PN-9326B06F`가 추적해 온 카운터
// 누수/오작동 계열 버그 전체가 이 설계에서는 애초에 존재할 수 없다.
//
// currentRsp는 이 진입 시점의(아직 스왑 전) rsp - 대응하는
// `kLeaveInterruptStack()`이 나중에 돌려줄 수 있게 코어별로 잠깐
// 맡아 둔다. 일반 벡터끼리는 절대 중첩되지 않으므로 이 저장소를
// 서로 다른 두 진입이 동시에 쓸 위험이 없다.
extern "C" kernel::uint64_t kEnterInterruptStack(kernel::uint64_t currentRsp);

// 위에서 맡아 둔 원래 rsp를 그대로 돌려준다. isr.S의 일반 벡터
// "자연 복귀" 경로만 이 반환값을 실제로 적용한다(원래 rsp로 되돌려
// 이후 pop들이 이 인터럽트가 실제로 push된 자리에서 정확히 읽게
// 함) - `context_switch.S`의 `kContextSwitchFromISR` 경로는 이제
// 이 함수를 아예 부르지 않는다(다른 Task로 영구히 전환하며 이
// 인터럽트를 끝내므로, 이 코어의 다음 일반 인터럽트 진입이
// `kEnterInterruptStack()`으로 스스로 새 값을 저장할 뿐이지 이전
// 값을 "짝 맞춰 돌려받을" 필요 자체가 없다 - 카운터가 없으므로
// 균형을 맞출 것도 없다).
extern "C" kernel::uint64_t kLeaveInterruptStack();

// [신규, 2026-09-21, SP-A252E82F] 주어진 주소(`addr`)가 이 코어
// (`coreIndex`)의 일반 디스패치 스택 또는 IST 스택(#DF/NMI/#MC/#DB/
// #PF) 중 어느 하나의 범위 안에 있는지 확인한다. 두 가지 용도로
// 쓰인다:
//   1. `kIsInInterruptContext()`(이 파일, SharedPtr 소멸 지연 판단) -
//      현재 rsp를 넘겨 "지금 이 코드가 인터럽트 스택 위에서 실행
//      중인가"를 직접 확인한다(카운터 없이, 있는 그대로의 사실을
//      본다 - 어떤 경로가 이 값을 "깜빡 안 내려도" 절대 어긋날 수
//      없다).
//   2. `kIsrHandler`(idt.cpp)가 IST 벡터(#PF 등)에 대해 "이게 정말
//      Task를 직접 인터럽트했는지, 아니면 다른 인터럽트 처리 도중
//      끼어든 것인지"를 판단할 때 - 그 인터럽트가 트랩한 시점의
//      rsp(`InterruptFrame::rspOld`)를 넘긴다. 일반 벡터는 이 확인이
//      필요 없다(절대 중첩되지 않으므로 항상 outermost).
extern "C" bool kIsAddressOnAnyInterruptStack(kernel::uint64_t addr, kernel::uint32_t coreIndex);

#endif  // MINICORE_KERNEL_DEFERRED_DESTRUCTION_H
