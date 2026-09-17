#ifndef MINICORE_KERNEL_TLS_H
#define MINICORE_KERNEL_TLS_H

#include "libkenv/types.h"
#include "task.h"

namespace kernel {

// SP-0666DB3C §11 - 원래는 진짜 컴파일러 thread_local에 필요한
// 인프라(FS_BASE 스왑 + .tdata/.tbss)가 없어(§11.1) 이 프로젝트가 이미
// 쓰는 "부팅 시 슬롯/코드를 한 번 발급받아 재사용" 관례(SyscallEndpointId/
// AsyncTaskSubjectCode, AsyncCallbackRegistry와 동일 패턴)를 흉내 낸
// 명시적(explicit) 슬롯 인덱스였다. [갱신, 2026-09-18, PN-22E5E9E7
// 항목2] 이제 그 인프라(항목1: 링커 .tdata/.tbss+PT_TLS, 항목2/3/4:
// 이 파일의 `gTlsSlots`+task.cpp의 TCB 생성+scheduler.cpp의
// `kSyncFsBase`)가 갖춰져, "슬롯 인덱스" 개념 자체는 API 호환을 위해
// 그대로 남기되 그 실제 저장소만 진짜 `thread_local` 배열로 옮겼다 -
// 아래 ThreadLocal<T>의 인덱스 발급/타입 안전 뷰 관례는 그대로다.
using TlsSlotIndex = uint32_t;
// kMaxTlsSlots 자체는 task.h(gTlsSlots 배열 크기와 공유하기 위해)에 있다.

// [신규, 2026-09-18, PN-22E5E9E7 항목2] SP-29D652AA가 요구하는 진짜
// 컴파일러 thread_local 배열 - Task::tlsSlots(예전 명시적 슬롯 배열
// 필드)를 대체한다. 실제 저장소는 이제 각 Task의 TCB(`Task::kernelFsBase`
// 가 가리키는 블록, task.cpp가 부팅마다 .tdata/.tbss 템플릿을 복사해
// 만든다)에 있다 - 디스패치 시점마다 FS_BASE가 그 Task 자신의 TCB로
// 스왑되므로(`kSyncFsBase`, scheduler.cpp) 컴파일러가 생성하는 %fs-상대
// 접근이 자동으로 "지금 실행 중인 Task 것"을 가리킨다 - `Scheduler::
// currentTask()`를 거칠 필요가 아예 없어졌다.
extern thread_local void* gTlsSlots[kMaxTlsSlots];

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
        return static_cast<T*>(gTlsSlots[_slot]);
    }
    void set(T* value) {
        gTlsSlots[_slot] = value;
    }

private:
    TlsSlotIndex _slot;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TLS_H
