#ifndef MINICORE_LIBS_LIBKENV_MAPLE_TREE_H
#define MINICORE_LIBS_LIBKENV_MAPLE_TREE_H

#include "libkenv/types.h"

// libkenv: Maple Tree(SP-2AAD7C8D §3) - VMA(가상 메모리 영역) 관리용
// 키-값 범위 자료구조. Linux 6.1의 Maple Tree를 이 프로젝트의
// freestanding 제약(RCU 없음, §3.2)에 맞게 이식한다. 다른 libkenv
// 컨테이너(chunked_list.h)와 같은 이유로 GenericSlabAllocator
// (minicore/libs/libkmm)보다 아래 계층이라 그 존재를 몰라야 한다 -
// 할당자는 함수 포인터로 주입받는다.

namespace kernel {

constexpr uint32_t kMapleRangeSlotCount = 16;   // maple_range_64/maple_leaf_64와 동일(SP-2AAD7C8D §3.1)
constexpr uint32_t kMapleArangeSlotCount = 10;  // maple_arange_64와 동일(gap 배열 자리 확보)
constexpr uint32_t kMapleDenseSlotCount = 31;   // pivot 없이 8B 슬롯만 채운 최대 개수(256B/8B)

// (1) 밀집 노드 - 아주 작은/희소 구간 전용(SP-2AAD7C8D §3.1-A: 트리
// 전체가 루트=리프 하나에 들어갈 때). v1 MapleTree 구현은 이 타입을
// 아직 만들지 않는다(§6-5 - 노드 타입 전환 휴리스틱은 구현 착수 시
// 실측하며 다듬는 것으로 열려 있음) - 태깅/구조체만 먼저 갖춰 둔다.
struct MapleDenseNode {
    void* parent;
    void* slot[kMapleDenseSlotCount];  // NULL=gap
};
static_assert(sizeof(MapleDenseNode) <= 256, "slab 256B 버킷 초과");

// (2) 리프 전용 64비트 범위 노드 - gap 배열 없음. v1 MapleTree는 아직
// 만들지 않는다(위와 같은 이유 - Arange64를 리프에도 그대로 쓰는 게
// v1의 기본 정책, §3.1-A).
struct MapleLeaf64Node {
    void* parent;
    uint64_t pivot[kMapleRangeSlotCount - 1];  // 15개 경계값
    void* slot[kMapleRangeSlotCount];          // 값 포인터(Vma* 등), NULL=gap
};
static_assert(sizeof(MapleLeaf64Node) <= 256, "slab 256B 버킷 초과");

// (3) 내부 전용 64비트 범위 노드 - gap 배열 없음, 자식 포인터만 보관.
// v1 MapleTree는 아직 만들지 않는다(내부 노드도 전부 Arange64,
// §3.1-A "루트를 포함한 모든 내부 노드는 Arange64로 시작").
struct MapleRange64Node {
    void* parent;
    uint64_t pivot[kMapleRangeSlotCount - 1];  // 15개 경계값
    void* slot[kMapleRangeSlotCount];          // 자식 노드 포인터(태깅된 포인터, 아래 참고)
};
static_assert(sizeof(MapleRange64Node) <= 256, "slab 256B 버킷 초과");

// (4) gap 탐색용 확장 노드 - v1 MapleTree가 실제로 쓰는 유일한 타입
// (루트=리프, §3.1-A 기본 정책). `usedSlotCount`는 SP-2AAD7C8D §3.1의
// 구조체 목록엔 없던 필드다 - store()/erase()가 "지금 몇 개의 슬롯이
// 실제 경계를 갖는지"를 알아야 하는데(비어 있는 트리는 슬롯 1개짜리
// 큰 gap 하나로 시작), 나머지 필드만으로는 그 개수를 안전하게
// 유추할 방법이 없어(경계값 0이 "안 씀"과 "진짜 경계가 0"을 구분 못함)
// 구현상 반드시 필요해 추가했다(RM-23F4B687 §4 취지 - 알고리즘의
// 구현 세부, 256B 예산에도 여유가 있어 문제없음).
struct MapleArangeNode {
    void* parent;
    uint64_t pivot[kMapleArangeSlotCount - 1];  // 9개 경계값 - slot[i]의 끝(포함), i<usedSlotCount-1일 때만 유효
    void* slot[kMapleArangeSlotCount];          // 10개 - 값 포인터(v1은 항상 리프라 자식이 아니라 값), NULL=gap
    uint64_t gap[kMapleArangeSlotCount];        // 각 슬롯의 gap 크기(슬롯이 gap이면 그 범위 크기, 값이면 0)
    uint16_t maxGapSlot;                        // gap[]이 가장 큰 슬롯 인덱스(findGap 힌트)
    uint16_t usedSlotCount;                     // 지금 의미 있는 슬롯 개수(1..kMapleArangeSlotCount)
};
static_assert(sizeof(MapleArangeNode) <= 256, "slab 256B 버킷 초과");

// SP-2AAD7C8D §3.1-B - 256B 정렬(하위 8비트가 항상 0)을 이용해
// 포인터 자체에 노드 타입을 인코딩한다. v1 MapleTree는 항상
// MapleArangeNode만 만들지만, 태깅 자체는 문서 그대로 4종 전부
// 정의해 둔다(다른 노드 타입이 실제로 쓰이기 시작해도 태깅 스킴이
// 안 바뀌게).
// libkenv/types.h엔 <cstdint>의 uintptr_t가 없다(freestanding,
// QU-148B7262) - 이 프로젝트가 포인터<->정수 변환에 이미 일관되게
// 쓰는 `uint64_t`(x86_64에서 포인터와 동일한 64비트 폭)로 대신한다.
enum class MapleNodeType : uint64_t { Dense = 0, Leaf64 = 1, Range64 = 2, Arange64 = 3 };

inline MapleNodeType kMapleNodeType(void* tagged) {
    return static_cast<MapleNodeType>(reinterpret_cast<uint64_t>(tagged) & 0x3);
}
inline void* kMapleNodePtr(void* tagged) {
    return reinterpret_cast<void*>(reinterpret_cast<uint64_t>(tagged) & ~uint64_t(0x3));
}
inline void* kMapleTagNode(void* raw, MapleNodeType type) {
    return reinterpret_cast<void*>(reinterpret_cast<uint64_t>(raw) | static_cast<uint64_t>(type));
}

// SP-2AAD7C8D §3.3 - [0, UINT64_MAX] 전체를 다루는 범위 키-값 자료구조.
// 값은 전부 void*(리프에서는 사용자 데이터 - §4의 Vma* 등, 태깅 없음).
//
// **v1 구현 범위(중요)**: 이 문서 §6-5가 "노드 타입 전환 휴리스틱은
// 구현 착수 시 실측하며 다듬는다"고 명시적으로 열어 둔 것을 그대로
// 받아, v1은 **루트=리프인 MapleArangeNode 딱 하나**만으로 동작한다
// (§3.1-A 기본 정책 - 루트/리프 전부 Arange64). 따라서 **동시에
// 저장 가능한 서로 겹치지 않는 범위(gap으로 분리된 값 슬롯)는 최대
// kMapleArangeSlotCount(10)개**로 제한된다 - 그 이상 필요해지면
// 여러 레벨로 분할하는 알고리즘(Linux 원본의 `maple_big_node` 기반
// 분할/재분배)이 필요한데, 이번 증분 범위 밖이라 `store()`가 그
// 상황에서 false를 반환한다(후속 계획으로 별도 추적 - 이 클래스의
// 공개 계약(§3.3의 시그니처/의미)은 향후 멀티레벨 구현으로 바뀌지
// 않는다, 지금은 그 계약을 만족하는 범위가 10개 엔트리로 좁을 뿐).
class MapleTree {
public:
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    // 할당자 콜백을 등록한다 - store()보다 먼저 준비돼 있어야 한다
    // (ChunkedList::ensureAllocator와 동일한 관례 - 이미 등록돼 있으면
    // 재호출은 무시).
    void ensureAllocator(AllocFn allocFn, FreeFn freeFn);

    // 트리를 빈 상태로 되돌린다(루트 해제) - 재사용 전 반드시 호출.
    void init();

    // [start, end](양 끝 포함) 범위에 value를 등록한다. 이미 값이
    // 있는 범위와 조금이라도 겹치면 실패(false) - 호출부가 먼저
    // findGap 등으로 빈 공간을 확인했다는 전제(§3.3). value는
    // nullptr일 수 없다(gap과 구분이 안 되므로).
    bool store(uint64_t start, uint64_t end, void* value);

    // addr을 포함하는 범위를 찾는다 - gap 안이면(등록된 값이 없으면)
    // nullptr. 찾으면 outRangeStart/outRangeEnd(둘 다 null 허용)에
    // 그 범위 전체(호출 시 넘긴 start/end 그대로)를 채운다.
    void* find(uint64_t addr, uint64_t* outRangeStart, uint64_t* outRangeEnd) const;

    // [searchFloor, searchCeil] 범위 안에서 size 이상인 첫 연속 gap을
    // 찾는다(first-fit, 문서 §3.3 - "서브트리 단위로 건너뛸 수 있게"는
    // 멀티레벨 트리가 생긴 뒤에나 의미가 생긴다 - v1은 단일 노드라
    // 그냥 왼쪽부터 훑는다). searchFloor > searchCeil이거나 size==0이면
    // false.
    bool findGap(uint64_t searchFloor, uint64_t searchCeil, uint64_t size, uint64_t* outStart) const;

    // [start, end] 범위를 지운다 - 부분 겹침도 지원한다(그 범위와
    // 겹치는 기존 엔트리가 있으면, 겹치지 않는 나머지 조각은 **같은
    // value 포인터를 유지한 채** gap 옆에 남는다 - 한 엔트리 중간에
    // 구멍을 내면 조각이 둘로 남을 수 있다). 실제로 무언가 지웠으면
    // true, [start,end]가 처음부터 전부 gap이었으면 false.
    //
    // **주의**: 이 함수는 순수하게 트리의 키 구조만 다룬다 - 값
    // 포인터가 가리키는 객체(예: Vma) 자신이 캐싱해 둔 start/end
    // 필드까지 갱신하거나, 조각난 값에 대해 새 객체를 할당하는 일은
    // 하지 않는다(그럴 방법 자체가 없다 - value는 opaque `void*`).
    // 한 엔트리가 정말로 둘로 쪼개져야 하는 경우(가운데를 punch-out),
    // 두 조각 모두 원래와 같은 value 포인터를 그대로 가리키게 되므로
    // - 그 값 객체를 두 개로 실제로 나누는 것은 호출부(향후
    // `ProcessAddressSpaceManager`, §5)의 책임이다.
    bool erase(uint64_t start, uint64_t end);

    // 트리 안의 모든 실제 값(비-gap) 엔트리를 방문한다 - fn(start, end,
    // value). ProcessAddressSpaceManager::unmapAll()(SP-2AAD7C8D §2)처럼
    // "지금 등록된 모든 범위를 알아야" 하는 소비자를 위해 추가했다(§3.3
    // 원 설계엔 없던 구현 세부 - RM-23F4B687 §4 취지, decompose()를
    // 그대로 재사용). v1(단일 루트=리프)이므로 항상 최대
    // kMapleArangeSlotCount(10)개까지만 순회한다(멀티레벨 트리가 생기면
    // 재귀 순회로 확장 필요 - PN-38D17292).
    template <typename Fn>
    void forEach(Fn&& fn) const {
        Span spans[kMapleArangeSlotCount];
        const uint32_t count = decompose(spans);
        for (uint32_t i = 0; i < count; ++i) {
            if (spans[i].value != nullptr) {
                fn(spans[i].lower, spans[i].upper, spans[i].value);
            }
        }
    }

private:
    struct Span {
        uint64_t lower;
        uint64_t upper;
        void* value;  // nullptr = gap
    };

    // 현재 루트 노드를 Span 배열로 풀어낸다 - count는 항상
    // usedSlotCount(루트가 없으면 0).
    uint32_t decompose(Span* outSpans) const;

    // spans(정렬된, 서로 인접/비중첩)로 루트 노드를 다시 구성한다 -
    // 인접한 gap끼리는 미리 합쳐져 있어야 한다(호출부 책임). count가
    // kMapleArangeSlotCount를 넘으면 false(트리 안 바꿈, v1 한계).
    bool rebuild(const Span* spans, uint32_t count);

    MapleArangeNode* _root = nullptr;
    AllocFn _alloc = nullptr;
    FreeFn _free = nullptr;
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_MAPLE_TREE_H
