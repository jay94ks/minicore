#include "tls.h"

#include "libkenv/spinlock.h"

namespace {

kernel::Spinlock gRegistryLock;
kernel::TlsSlotIndex gNextSlot = 0;

}  // namespace

namespace kernel {

// [신규, 2026-09-18, PN-22E5E9E7 항목2] 이 배열 자신이 .tbss에 놓인다
// (전부 0으로 초기화되므로 .tdata가 아니라 .tbss - linker.ld 참고) -
// 부팅마다 `Task::init()`(task.cpp)이 이 템플릿(.tdata~.tbss 경계
// 전체)을 복사해 각 Task 전용 TCB 인스턴스를 만든다.
thread_local void* gTlsSlots[kMaxTlsSlots] = {};

TlsSlotIndex TlsRegistry::allocateSlot() {
    SpinlockGuard guard(gRegistryLock);
    if (gNextSlot >= kMaxTlsSlots) {
        return kMaxTlsSlots;  // 상한 초과 - 호출부가 확인하면 항상 유효하지 않은 값
    }
    return gNextSlot++;
}

}  // namespace kernel
