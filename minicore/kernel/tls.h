#ifndef MINICORE_KERNEL_TLS_H
#define MINICORE_KERNEL_TLS_H

#include "libkenv/types.h"
#include "scheduler.h"
#include "task.h"

namespace kernel {

// SP-0666DB3C §11 - 진짜 컴파일러 thread_local(FS_BASE 스왑 +
// .tdata/.tbss)에 필요한 인프라가 아직 없어(§11.1), 이 프로젝트가
// 이미 쓰는 "부팅 시 슬롯/코드를 한 번 발급받아 재사용" 관례
// (SyscallEndpointId/AsyncTaskSubjectCode, AsyncCallbackRegistry와
// 동일 패턴)를 그대로 재사용하는 명시적(explicit) TLS다.
using TlsSlotIndex = uint32_t;
// kMaxTlsSlots 자체는 task.h(Task::tlsSlots 배열 크기와 공유하기 위해)에 있다.

// 기능별로 부팅/초기화 시 한 번만 슬롯을 발급받는다 - 요청마다
// 등록/해제하지 않는다(AsyncCallbackRegistry와 동일 관례).
class TlsRegistry {
public:
    // kMaxTlsSlots을 넘으면 kMaxTlsSlots을 그대로 반환한다
    // (AsyncCallbackRegistry::registerHandler와 동일 관례 - 이 값은
    // ThreadLocal<T>가 그대로 인덱스로 써버리면 배열 밖 접근이 되므로
    // 호출부(부팅 시 슬롯을 발급받는 소수의 서브시스템)가 반환값을
    // kMaxTlsSlots과 비교해 확인해야 한다).
    static TlsSlotIndex allocateSlot();
};

// onExec/코루틴 본문이나 syscall 처리 코드가 쓰는 타입 안전 뷰 - 실제
// T 인스턴스의 생성/해제는 그 슬롯을 발급받은 서브시스템 책임이다
// (AsyncTaskHandler의 args와 동일 원칙) - 이 클래스 자신은 슬롯
// 접근만 제공한다. Task/UserThread(완전한 스케줄링 단위) 전용 -
// AsyncTask는 대상이 아니다(§11.3).
template <typename T>
class ThreadLocal {
public:
    explicit ThreadLocal(TlsSlotIndex slot) : _slot(slot) {}

    T* get() const {
        return static_cast<T*>(Scheduler::currentTask()->tlsSlots[_slot]);
    }
    void set(T* value) {
        Scheduler::currentTask()->tlsSlots[_slot] = value;
    }

private:
    TlsSlotIndex _slot;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TLS_H
