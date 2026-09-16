#ifndef MINICORE_KERNEL_DELAYED_EXEC_H
#define MINICORE_KERNEL_DELAYED_EXEC_H

#include "libkenv/types.h"

namespace kernel {

// 지연 실행(타이머/알람) 인프라(SP-F15B4A63, PN-C46DF296) - 임의
// 시간 뒤 콜백을 실행하는 커널 내부 전용 메커니즘(유저 syscall 아님,
// v1 범위). 첫 실사용처는 SP-EAB162FC §6.4 Resurrect 백오프(1~10분
// 간격) - PN-645CF608 항목 5가 이 계획 완료 후 이어서 배선한다.
//
// **자료구조(§2, QU-A8C0CC2C 설계자 답변)**: 고정 배열이 아니라
// 연결 리스트다 - "초과 시 또 별도의 비용이 발생하니 linked list로
// 구현해야 해"(설계자 지시). 상한 자체가 없으므로 `schedule()`의
// 실패 가능성은 순수 노드 할당 실패(OOM)로 좁혀진다. 동시 대기
// 항목 수가 여전히 적을 것으로 예상돼(실사용처가 Resurrect 백오프
// 하나뿐) `pump()`는 정렬 리스트/힙 없이 매번 선형 순회한다
// (RM-23F4B687 §4 - 실제로 항목 수가 늘어나는 게 확인되면 재검토).
using DelayedCallback = void (*)(void* arg);

// 슬랩(SP-D7013B26)에서 노드 하나씩 할당/해제하는 연결 리스트 항목 -
// 정렬되지 않은 채 삽입 순서 그대로 매달리고, pump()가 매번 선형
// 순회하며 만료분을 찾는다(§2 참고, 헤더 서두 주석과 동일 근거).
struct DelayedTimerEntry {
    uint64_t deadlineTick = 0;   // Timer::tickCount() 기준 절대 시각
    DelayedCallback callback = nullptr;
    void* arg = nullptr;
    uint64_t token = 0;          // 발급 시 유일값(취소용)
    DelayedTimerEntry* next = nullptr;
};

class DelayedExecutionQueue {
public:
    // 부팅 시 한 번(BSP) - 리스트를 빈 상태로 리셋한다.
    static void init();

    // delayTicks(Timer::tickCount() 단위) 이후 callback(arg)를
    // 실행하도록 예약한다 - 노드는 GenericSlabAllocator에서 확보.
    // 성공 시 0이 아닌 토큰(cancel()에 넘길 값), 노드 할당 실패
    // 시 0.
    static uint64_t schedule(uint64_t delayTicks, DelayedCallback callback, void* arg);

    // 아직 실행 전이면 리스트에서 제거하고 노드를 해제한다(취소) -
    // 이미 실행됐거나(pump()가 이미 제거) 없는 토큰이면 false.
    static bool cancel(uint64_t token);

    // 만료된(deadlineTick <= Timer::tickCount()) 항목을 전부 찾아
    // callback(arg)을 호출하고 리스트에서 제거(노드 해제)한다 -
    // AsyncReactor(SP-F682B889)가 자기 실행 큐가 비어 idle로
    // 돌아가기 직전마다 호출한다(§3, QU-A8C0CC2C 설계자 답변 -
    // 전용 커널 Task를 새로 만들지 않고 기존 리액터를 재사용).
    // 콜백은 반드시 짧게 끝나야 한다(리액터 자신의 실행 슬롯 위에서
    // 직접 호출되므로 - onExec과 동일한 제약).
    static void pump();

    // 리스트가 비어 있지 않은지(만료 여부와 무관) - 리액터가 이
    // 값이 true인 동안은 완전히 파킹하지 않고 짧은 주기로 다시
    // 확인하도록(§3 구현, async_task.cpp의 reactorTaskEntry) 판단
    // 하는 데 쓴다. 리스트가 비어 있으면(가장 흔한 정상 상태) 리액터가
    // 평소처럼 그대로 파킹해 CPU를 낭비하지 않는다.
    static bool hasPending();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_DELAYED_EXEC_H
