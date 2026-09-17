#ifndef MINICORE_KERNEL_KERNEL_CONTAINER_POLICY_H
#define MINICORE_KERNEL_KERNEL_CONTAINER_POLICY_H

#include "libkcont/lockfree_vector.h"
#include "libkcont/vector.h"
#include "libkmm/slab.h"

// Vector<T, Policy>(libkcont/vector.h)의 "커널 최적화 버전" 첫
// 적용 사례(SP-FAF768AB §0-A, PN-633BF2D8 구현 범위 2번) -
// GenericSlabAllocator(libkmm)를 컴파일 타임에 고정한 정책 구현체를
// 커널 쪽에 둔다. libkcont 자신은 이 계층 구조(libkenv 밑, libkmm
// 위)를 몰라야 하므로(RM-23F4B687 디렉터리 배치 규칙 - libkenv/
// chunked_list.h가 이미 확립한 "할당자는 함수 포인터로 주입, 상위
// 계층을 직접 링크하지 않는다" 원칙과 동일) GenericSlabAllocator를
// 직접 링크하는 이 정책 구현체는 여기(minicore/kernel, 커널 전용)에
// 둔다. moveElement/destroyElement는 DefaultContainerPolicy와 동일한
// 트리비얼 동작을 유지한다(비trivial 타입 지원은 별도 소비자가
// 필요할 때 그 소비자가 직접 정책을 정의).

namespace kernel {

struct KernelVectorPolicy {
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    template <typename T>
    static void moveElement(T* dst, T* src) {
        *dst = *src;
    }
    template <typename T>
    static void destroyElement(T* /*ptr*/) {}

    static void* kAlloc(uint64_t size) { return GenericSlabAllocator::alloc(size); }
    static void kFree(void* ptr, uint64_t size) { GenericSlabAllocator::free(ptr, size); }
};

// 호출부 편의 별칭 - `Vector<Foo, KernelVectorPolicy> v; v.ensureAllocator(
// KernelVectorPolicy::kAlloc, KernelVectorPolicy::kFree);`를 매번
// 반복하지 않고 `KernelVector<Foo> v; v.ensureAllocator(kKernelAlloc,
// kKernelFree);`로 줄인다.
template <typename T>
using KernelVector = Vector<T, KernelVectorPolicy>;

inline void* kKernelVectorAlloc(uint64_t size) { return GenericSlabAllocator::alloc(size); }
inline void kKernelVectorFree(void* ptr, uint64_t size) { GenericSlabAllocator::free(ptr, size); }

// [신규, 2026-09-17, PN-DAE91888] LockFreeVector<T>(libkcont)도 같은
// "함수 포인터로 할당자 주입" 원칙을 그대로 따른다 - Policy 템플릿
// 파라미터 없이 alloc/free 시그니처만 받으므로(세그먼트 방식이라
// moveElement/destroyElement 훅 자체가 필요 없음 - 재할당이 없어 기존
// 원소를 옮기거나 정리할 일이 없다), 별도 정책 구조체 없이
// KernelVectorPolicy가 이미 노출한 kKernelVectorAlloc/kKernelVectorFree
// 를 그대로 ensureAllocator에 넘기면 된다 - `KernelLockFreeVector<Foo> v;
// v.ensureAllocator(kKernelVectorAlloc, kKernelVectorFree);`.
template <typename T>
using KernelLockFreeVector = LockFreeVector<T>;

}  // namespace kernel

#endif  // MINICORE_KERNEL_KERNEL_CONTAINER_POLICY_H
