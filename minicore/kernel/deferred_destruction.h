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

// isr.S/context_switch.S가 직접 호출하는 C 링키지 리프 함수 - 반드시
// 아주 가볍고, 자기 자신의 balanced call/ret 외에는 스택을 건드리지
// 않아야 한다(InterruptFrame 구성 도중/직후, 또는 TaskTcb 블록을 막
// 새 스택으로 삼기 직전처럼 rsp가 민감한 지점에서 불리므로 - 정확한
// 삽입 위치의 근거는 각 .S 파일의 주석 참고).
//
// [갱신, 2026-09-21, PN-D7B66FE4, DC-53B93BFF (B) "IST 기반 전용
// 인터럽트 스택" 확정 반영] 단순 카운터 증감만 하지 않는다 - 이
// 진입이 가장 바깥쪽(중첩 아님, 증가 후 값==1)이면 이 코어 전용의
// 넉넉한 인터럽트 디스패치 스택 top 주소를 반환해 isr.S가 실제로
// rsp를 그 값으로 바꾸게 한다(중첩이면 0 반환 - 이미 그 전용 스택
// 위에서 실행 중이므로 다시 top으로 되감지 않는다). **이 방식은
// SP-677210E6의 하드웨어 IST(게이트를 탈 때마다 무조건 고정 top으로
// 되감음 - 진짜 재진입에는 안전하지 않다, 그래서 #DF/NMI/#MC/#DB
// 처럼 재진입 시나리오 자체가 없거나 무의미한 벡터에만 적합)와
// 다르다 - Linux 등 성숙한 커널의 percpu IRQ 스택과 동일한 원칙으로,
// "가장 바깥쪽일 때만 top으로 스왑, 중첩이면 현재 위치에서 그대로
// 이어 쓴다"를 소프트웨어(이 카운터)로 직접 구현해 진짜 재진입에도
// 안전하다. PN-584DB994이 gdb로 확정한 "중첩 인터럽트 3-4단계가
// Task의 고정 8KiB 커널 스택(kTaskDefaultKernelStackSize, task.h)을
// 실제로 넘긴다"는 근본 원인을 없앤다 - kIsrHandler(및 그 안의 전체
// C++ 호출 사슬: onTick/onForcedMigration/pickNext/enqueue/Logger 등)가
// 이제 Task 자신의 스택이 아니라 이 전용 버퍼 위에서 실행된다.
//
// currentRsp는 이 진입 시점의(아직 스왑 전) rsp - 가장 바깥쪽일 때만
// kLeaveInterruptDepth()가 나중에 되돌려줄 수 있게 코어별로 잠깐
// 맡아 둔다(그 코어에서 대응하는 이탈이 원래 스택으로 돌아갈 때만
// 읽음 - 중첩된 안쪽 진입/이탈은 이 저장소를 전혀 건드리지 않는다).
extern "C" kernel::uint64_t kEnterInterruptDepth(kernel::uint64_t currentRsp);

// 깊이 카운터를 내리고, 이 이탈이 가장 바깥쪽(내린 후 값==0)이면 위
// 진입 때 맡아 둔 원래 rsp를 반환한다(중첩 이탈이면 0). isr.S의
// "같은 Task로 자연 복귀" 경로만 이 반환값을 실제로 적용(0이 아니면
// rsp를 그 값으로 되돌려 이후 pop들이 이 인터럽트가 실제로 push된
// 그 자리에서 정확히 읽게 한다) - context_switch.S의
// kContextSwitchFromISR 경로는 이 반환값을 그대로 무시해도 안전하다
// (어차피 이 호출 직후 곧바로 rsp=newTcb로 덮어쓰므로, 기존 코드
// 변경 없음).
extern "C" kernel::uint64_t kLeaveInterruptDepth();

// [신규, 2026-09-21, PN-584DB994, 설계자 지시] `kEnterInterruptDepth()`가
// 이미 이 인터럽트분을 반영해 증가시킨 뒤의 값 - `kIsrHandler`(idt.cpp)
// 가 호출 시점에 이 값이 정확히 1이면 "지금 이 인터럽트가 어떤
// Task를 직접 인터럽트했다(중첩 아님)"는 뜻이라, 그 경우에만
// `Scheduler::captureCurrentFrame()`을 불러 그 Task의 tcb를 즉시
// 갱신한다(중첩이면 `frame`이 원래 Task의 진짜 재개 지점이 아니라
// "바깥쪽 인터럽트 처리 도중 어딘가"라 오히려 tcb를 오염시킨다).
extern "C" unsigned int kCurrentInterruptDepth();

#endif  // MINICORE_KERNEL_DEFERRED_DESTRUCTION_H
