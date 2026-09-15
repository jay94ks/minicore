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
    virtual bool cancel(Task* task, WaitCancelReason reason) = 0;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_WAITABLE_H
