#ifndef MINICORE_LIBS_LIBKMM_SLAB_H
#define MINICORE_LIBS_LIBKMM_SLAB_H

#include "libkenv/spinlock.h"
#include "libkenv/types.h"

namespace kernel {

// Bonwick 매거진/디포 모델(SP-D7013B26, 확정, QU-4F905C08 답변 반영) -
// 코어당 더블 매거진(loaded/previous, 각 16개 고정) + 전역 디포
// (lock-free) + 슬랩 페이지(PageFrameAllocator::allocPage(), Order 0
// 고정). 이 SlabCache 하나는 고정 크기 객체 하나만 다룬다 - 크기별
// 인스턴스 배열은 GenericSlabAllocator가 관리한다.
//
// 동시성: alloc()/free() 진입 즉시 PreemptionGuard(scheduler.h)로 이
// 코어의 선점을 비활성화해, 코어별 매거진 접근 도중 다른 Task로
// 전환되지 않음을 보장한다(§2.1) - 전역 디포(lock-free 스택)와 슬랩
// 페이지 free-list(Spinlock 폴백)는 여러 코어가 동시에 건드릴 수
// 있어 별도로 보호한다.
class SlabCache {
public:
    void init(unsigned long objectSize);

    // 실패 시(디포+PageFrameAllocator까지 완전히 고갈) nullptr - 절대
    // 블로킹하지 않는다(§2.4, 설계자 지시 - 비동기 프레임워크 내부에서
    // 메모리 고갈로 슬립에 들어가면 스케줄러 데드락을 유발할 수 있음).
    void* alloc();
    void free(void* ptr);

private:
    static constexpr unsigned int kMagazineCapacity = 16;   // 설계자 지시로 고정
    static constexpr unsigned int kMaxCores = 32;            // kAcpiMaxCpus와 맞춤(순환 include 회피용 중복 상수)

    // 디포 lock-free 스택(Treiber stack)의 노드이자 매거진 자체 -
    // 값 복사 없이 코어<->디포 사이를 포인터만 옮겨 다닌다(총 노드
    // 개수는 2*kMaxCores로 고정 - 런타임 중 새로 만들거나 없애지
    // 않음).
    struct MagazineNode {
        MagazineNode* next = nullptr;
        unsigned int count = 0;
        void* items[kMagazineCapacity];
    };

    struct CoreState {
        MagazineNode* loaded = nullptr;
        MagazineNode* previous = nullptr;
    };

    static MagazineNode* popDepot(AtomicPtr<MagazineNode>& stack);
    static void pushDepot(AtomicPtr<MagazineNode>& stack, MagazineNode* node);

    // 매거진 계층 아래의 원시 슬랩 페이지 계층 - 매거진과 디포가 모두
    // 바닥났을 때만 거친다. 페이지 하나(PageFrameAllocator::allocPage(),
    // Order 0 고정, §2.2)를 객체 크기로 등분해 침습적 free-list로
    // 관리한다(빈 객체의 첫 8바이트에 다음 포인터를 겹쳐 씀 - 그래서
    // 버킷 최소 크기가 32B로 고정돼 있다, §2.3).
    void* slabLayerAlloc();
    void slabLayerFree(void* ptr);
    void growRawFreeList();

    unsigned long _objectSize = 0;
    CoreState _perCore[kMaxCores];
    AtomicPtr<MagazineNode> _fullDepot;
    AtomicPtr<MagazineNode> _emptyDepot;
    MagazineNode _nodePool[2 * kMaxCores];

    Spinlock _rawLock;
    void* _rawFreeList = nullptr;
};

// 요청 크기를 7단계 버킷(32/64/128/256/512/1024/2048) 중 하나로
// 올림한다 - 32B 미만 요청은 32로 취급(빈 객체에 free-list 포인터가
// 겹쳐 써지므로 최소 크기가 필요, §2.3). 2048 초과면 0을 반환한다 -
// 슬랩을 거치지 않고 PageFrameAllocator로 직행해야 한다는 신호다.
unsigned long sizeToBucket(unsigned long size);

// 버킷 7단계 고정(설계자 지시) - `alloc`/`free` 둘 다 내부에서
// `sizeToBucket`을 다시 거치므로, 호출부가 정확한 버킷 크기가 아닌
// 원래 요청 크기를 그대로 넘겨도 항상 같은 버킷으로 되돌아간다.
class GenericSlabAllocator {
public:
    static void init();

    // 실패 시 nullptr(비블로킹, SlabCache::alloc()과 동일한 정책).
    static void* alloc(unsigned long size);
    static void free(void* ptr, unsigned long size);
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKMM_SLAB_H
