#include "tls.h"

#include "libkenv/spinlock.h"

namespace {

kernel::Spinlock gRegistryLock;
kernel::TlsSlotIndex gNextSlot = 0;

}  // namespace

namespace kernel {

TlsSlotIndex TlsRegistry::allocateSlot() {
    SpinlockGuard guard(gRegistryLock);
    if (gNextSlot >= kMaxTlsSlots) {
        return kMaxTlsSlots;  // 상한 초과 - 호출부가 확인하면 항상 유효하지 않은 값
    }
    return gNextSlot++;
}

}  // namespace kernel
