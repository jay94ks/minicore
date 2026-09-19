#ifndef MINICORE_KERNEL_WAITABLE_H
#define MINICORE_KERNEL_WAITABLE_H

#include "libkenv/types.h"

namespace kernel {

struct Task;

// SP-0666DB3C §9.2 - 임의의 대기 상태에서 강제로 끄집어내는 범용 훅.
// 이 프로젝트엔 서로 다른 모양의 "대기 중" 상태가 여러 개 있다(§1의
// WaitQueue, SP-04EE2A18의 UserThread::pendingSyscalls 등) - 각각을
// 개별 취소 코드로 구현하지 않도록, "이 Task가 지금 무엇에 막혀
// 있는지"를 추상 인터페이스로 표현한다. Task::blockedOn이 이 포인터를
// 들고 있다.
enum class WaitCancelReason : uint32_t {
    None,     // 강제 취소된 적 없음(정상 웨이크업, 또는 아직 파킹 중) -
              // Task::lastCancelReason의 기본값(§9.6-3 참고)
    Signal,   // 강제 시그널(Kill 등)에 의한 취소
    Timeout,  // 향후 타임아웃 기능이 생기면 재사용(v1 범위 밖)
};

class Waitable {
public:
    virtual ~Waitable() = default;

    // task를 이 대기 구조체에서 강제로 제거하고 Ready로 되돌린다. 이미
    // 정상적으로 깨어나 대기 구조체를 떠난 뒤라면(경쟁 상황) 아무 일도
    // 하지 않고 false를 반환한다 - 호출부는 이 반환값으로 "취소가
    // 실제로 적용됐는지"를 판단한다.
    //
    // [갱신, 2026-09-19, PN-0AC554C2, QU-25E1C297 답변] "강제로 즉시
    // 제거"가 아니라 **협조적 취소**로 재해석된다 - 각 Waitable 구현체
    // (특히 큰 단위로 처리되는 비동기 작업을 감싸는 것들)는 내부적으로
    // 취소 토큰을 유통해 취소됐으면 스스로 다음 확인 지점에서 탈출해야
    // 한다. 정확한 토큰 API 형태는 PN-0AC554C2 5단계에서 설계 제안 후
    // 확정한다 - 이 시그니처 자체는 아직 안 바뀐다(walk-back 아님,
    // 단계적 마이그레이션).
    virtual bool cancel(Task* task, WaitCancelReason reason) = 0;

    // [신규, 2026-09-19, PN-0AC554C2, QU-25E1C297 답변("Waitable
    // 클래스에 isCompleted를 추가하고 스케쥴러가 이걸 확인하면서
    // 리스트를 자동으로 비우게 만들어")] `Task::blockedOn` 리스트에
    // 들어간 이 Waitable 엔트리가 이미 해소됐는지 - `Scheduler::
    // onTick()`의 재스케줄 결정 지점이 이 값을 확인해 true인 엔트리를
    // 리스트에서 제거하고, 리스트가 완전히 비면 그 Task를 다시 실행
    // 가능한 상태로 되돌린다(kDrainAndCheckBlockedOn, wait_queue.h
    // 참고). **1단계(현재)에서는 아무도 이 값을 실제로 소비하는 경로가
    // 없다** - 기존 WaitQueue/pendingSyscalls/디버그 정지 세 경로는
    // 전부 여전히 자기 자신의 기존 메커니즘(Scheduler::parkCurrent()
    // 직접 호출 등)으로 블로킹하고, blockedOn은 그 경로들이 실행
    // 중인 동안(=Task가 이미 파킹돼 onTick의 "current" 후보가 아닌
    // 동안) 채워져 있으므로 이 확인 자체가 호출될 기회가 없다 - 순수
    // 추가 단계(PN-0AC554C2 1번)이며, 실제 통합(2-6단계)이 진행되면서
    // 비로소 의미를 갖는다.
    virtual bool isCompleted() const = 0;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_WAITABLE_H
