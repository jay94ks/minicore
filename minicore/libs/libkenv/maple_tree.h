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
// 전체가 루트=리프 하나에 들어갈 때). v2 MapleTree 구현도 이 타입을
// 아직 만들지 않는다(§6-5 - 노드 타입 전환 휴리스틱은 구현 착수 시
// 실측하며 다듬는 것으로 열려 있음) - 태깅/구조체만 먼저 갖춰 둔다.
struct MapleDenseNode {
    void* parent;
    void* slot[kMapleDenseSlotCount];  // NULL=gap
};
static_assert(sizeof(MapleDenseNode) <= 256, "slab 256B 버킷 초과");

// (2) 리프 전용 64비트 범위 노드 - gap 배열 없음. v2 MapleTree는 아직
// 만들지 않는다(위와 같은 이유 - Arange64를 리프/내부 노드 양쪽에
// 그대로 쓰는 게 v2의 기본 정책, §3.1-A).
struct MapleLeaf64Node {
    void* parent;
    uint64_t pivot[kMapleRangeSlotCount - 1];  // 15개 경계값
    void* slot[kMapleRangeSlotCount];          // 값 포인터(Vma* 등), NULL=gap
};
static_assert(sizeof(MapleLeaf64Node) <= 256, "slab 256B 버킷 초과");

// (3) 내부 전용 64비트 범위 노드 - gap 배열 없음, 자식 포인터만 보관.
// v2 MapleTree는 아직 만들지 않는다(내부 노드도 전부 Arange64,
// §3.1-A "루트를 포함한 모든 내부 노드는 Arange64로 시작").
struct MapleRange64Node {
    void* parent;
    uint64_t pivot[kMapleRangeSlotCount - 1];  // 15개 경계값
    void* slot[kMapleRangeSlotCount];          // 자식 노드 포인터(태깅된 포인터, 아래 참고)
};
static_assert(sizeof(MapleRange64Node) <= 256, "slab 256B 버킷 초과");

// (4) gap 탐색용 확장 노드 - v2 MapleTree가 실제로 쓰는 유일한 타입
// (리프/내부 노드 전부 Arange64, §3.1-A 기본 정책). `usedSlotCount`는
// SP-2AAD7C8D §3.1의 구조체 목록엔 없던 필드다 - store()/erase()가
// "지금 몇 개의 슬롯이 실제 경계를 갖는지"를 알아야 하는데(비어 있는
// 트리는 슬롯 1개짜리 큰 gap 하나로 시작), 나머지 필드만으로는 그
// 개수를 안전하게 유추할 방법이 없어(경계값 0이 "안 씀"과 "진짜
// 경계가 0"을 구분 못함) 구현상 반드시 필요해 추가했다(RM-23F4B687
// §4 취지 - 알고리즘의 구현 세부, 256B 예산에도 여유가 있어 문제없음).
// `leaf`도 같은 이유로 추가한 v2 전용 필드 - 리프에서는 slot[i]가
// 값 포인터(또는 NULL=gap), 내부 노드에서는 slot[i]가 항상 태깅된
// 자식 포인터(gap 없음 - 내부 노드는 자기 범위를 자식들로 완전히
// 분할한다)라는 두 가지 완전히 다른 해석을 이 필드 하나로 구분한다.
struct MapleArangeNode {
    void* parent;
    uint64_t pivot[kMapleArangeSlotCount - 1];  // 9개 경계값 - slot[i]의 끝(포함), i<usedSlotCount-1일 때만 유효
    void* slot[kMapleArangeSlotCount];          // 10개 - leaf: 값 포인터(NULL=gap) / 내부: 태깅된 자식 포인터(항상 non-null)
    uint64_t gap[kMapleArangeSlotCount];        // leaf: 그 슬롯 자신의 gap 크기(값이면 0) / 내부: 그 자식 서브트리 안의 최대 gap(집계값)
    uint16_t maxGapSlot;                        // gap[]이 가장 큰 슬롯 인덱스(findGap 힌트)
    uint16_t usedSlotCount;                     // 지금 의미 있는 슬롯 개수(1..kMapleArangeSlotCount)
    bool leaf;                                  // true=리프(값 저장) / false=내부(자식 포인터 저장) - v2 전용
};
static_assert(sizeof(MapleArangeNode) <= 256, "slab 256B 버킷 초과");

// SP-2AAD7C8D §3.1-B - 256B 정렬(하위 8비트가 항상 0)을 이용해
// 포인터 자체에 노드 타입을 인코딩한다. v2 MapleTree는 항상
// MapleArangeNode만 만들지만(leaf/internal 구분은 위 `leaf` 필드가
// 담당), 태깅 자체는 문서 그대로 4종 전부 정의해 둔다(다른 노드
// 타입이 실제로 쓰이기 시작해도 태깅 스킴이 안 바뀌게).
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
// **v2 구현(PN-38D17292, 2026-09-15) - 멀티레벨 지원**: v1(PN-0782CAC4)
// 은 루트=리프인 MapleArangeNode 딱 하나로만 동작해 동시에 저장 가능한
// 서로 겹치지 않는 범위가 최대 kMapleArangeSlotCount(10)개(실측으로는
// 경계 밖 gap 2개가 항상 슬롯을 점유해 8개, QU-824C6B0A)로 제한됐다.
// v2는 Linux 원본의 `maple_big_node` 기반 분할 절차를 그대로 이식해
// 노드가 꽉 차면 두 개로 쪼개고 부모(없으면 새 루트)에 자식을
// 편입시키는 방식으로 트리 높이가 필요한 만큼 늘어난다 - 엔트리 개수
// 상한이 사실상 사라진다(메모리가 허용하는 한). **공개 계약(이 클래스의
// 시그니처/의미)은 v1과 완전히 동일** - SP-2AAD7C8D §3.3이 이미 "향후
// 멀티레벨 구현으로 바뀌지 않는다"고 명시해 둔 대로다.
//
// **v2가 아직 하지 않는 것(알려진 한계, 의도적 축소 - RM-23F4B687 §4)**:
// 1. **erase 시 노드 병합/트리 축소가 없다** - 노드가 성겨져도(엔트리
//    수가 적어져도) 형제와 합치거나 부모에서 제거하지 않는다. 트리
//    구조상 안전(끊긴 링크나 잘못된 값이 남지 않음)하지만, VMA를 많이
//    만들었다가 대부분 지우는 워크로드에서 메모리(슬랩 256B 버킷)를
//    필요 이상으로 오래 붙들 수 있다 - 후속 과제.
// 2. **분할 도중 새 노드 할당이 실패하면 트리가 부분적으로만 갱신된
//    채 남을 수 있다** - 원래 하나였던 노드가 이미 둘로 나뉜(자식/
//    형제 쪽은 이미 rebuild됨) 상태에서 그 위 레벨(부모 삽입 또는
//    새 루트 생성)의 할당만 실패하면, `store()`는 false를 반환하지만
//    내부적으로는 이미 일부 구조 변경이 반영돼 있다 - 진짜 트랜잭션
//    (사전에 필요한 노드를 전부 확보한 뒤에만 실제로 커밋)은 이번
//    범위 밖이다. 슬랩 할당자가 시스템 전역적으로 완전히 고갈되는
//    극단적 상황에서만 발현되는 경로라 v1/v2 전환 시점엔 실측된 적
//    없음 - 실제로 문제가 되면 재검토.
// 3. **하나의 store()/erase() 호출이 요구하는 범위가 자식 노드 하나의
//    경계를 넘어서면 실패한다**(`insertIntoInternal`/`erase`의 방어적
//    분기) - 이론상 서로 다른 리프에 걸친 "논리적으로 하나로 이어진
//    gap"이 있을 수 있는데(두 리프가 우연히 인접한 경계에서 각자
//    gap을 갖고 있는 경우), findGap()의 gap 집계는 서브트리 단위라 이
//    경우를 하나의 큰 gap으로 합쳐 보고하지 않는다(각 자식의 최대
//    gap만 본다) - 그래서 store()가 그 경계를 넘는 요청을 받을 일
//    자체가 findGap()이 먼저 걸러 주지만, 만에 하나 호출부가 직접
//    임의의 [start,end]를 넘기면 이 방어 분기가 안전하게 거부한다.
class MapleTree {
public:
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    // 할당자 콜백을 등록한다 - store()보다 먼저 준비돼 있어야 한다
    // (ChunkedList::ensureAllocator와 동일한 관례 - 이미 등록돼 있으면
    // 재호출은 무시).
    void ensureAllocator(AllocFn allocFn, FreeFn freeFn);

    // 트리를 빈 상태로 되돌린다(트리 전체 - 멀티레벨이면 모든 내부/
    // 리프 노드를 재귀적으로 반납 - 재사용 전 반드시 호출.
    void init();

    // [start, end](양 끝 포함) 범위에 value를 등록한다. 이미 값이
    // 있는 범위와 조금이라도 겹치면 실패(false) - 호출부가 먼저
    // findGap 등으로 빈 공간을 확인했다는 전제(§3.3). value는
    // nullptr일 수 없다(gap과 구분이 안 되므로). 노드가 꽉 차 있으면
    // 필요한 만큼 자동으로 분할한다(v2, 위 클래스 문서 참고) - v1과
    // 달리 엔트리 개수 상한 때문에 실패하는 경우는 사실상 없다.
    bool store(uint64_t start, uint64_t end, void* value);

    // addr을 포함하는 범위를 찾는다 - gap 안이면(등록된 값이 없으면)
    // nullptr. 찾으면 outRangeStart/outRangeEnd(둘 다 null 허용)에
    // 그 범위 전체(호출 시 넘긴 start/end 그대로)를 채운다.
    void* find(uint64_t addr, uint64_t* outRangeStart, uint64_t* outRangeEnd) const;

    // [searchFloor, searchCeil] 범위 안에서 size 이상인 첫 연속 gap을
    // 찾는다(first-fit). v2는 내부 노드의 gap 집계(§3.3 "서브트리
    // 단위로 건너뛸 수 있게")를 실제로 활용해, 요청 크기보다 작은
    // 최대 gap만 가진 서브트리는 통째로 건너뛴다. searchFloor >
    // searchCeil이거나 size==0이면 false.
    bool findGap(uint64_t searchFloor, uint64_t searchCeil, uint64_t size, uint64_t* outStart) const;

    // [start, end] 범위를 지운다 - 부분 겹침도 지원한다(그 범위와
    // 겹치는 기존 엔트리가 있으면, 겹치지 않는 나머지 조각은 **같은
    // value 포인터를 유지한 채** gap 옆에 남는다 - 한 엔트리 중간에
    // 구멍을 내면 조각이 둘로 남을 수 있다). 실제로 무언가 지웠으면
    // true, [start,end]가 처음부터 전부 gap이었으면 false. **노드
    // 병합/트리 축소는 하지 않는다**(위 클래스 문서의 알려진 한계 1번).
    //
    // **주의**: 이 함수는 순수하게 트리의 키 구조만 다룬다 - 값
    // 포인터가 가리키는 객체(예: Vma) 자신이 캐싱해 둔 start/end
    // 필드까지 갱신하거나, 조각난 값에 대해 새 객체를 할당하는 일은
    // 하지 않는다(그럴 방법 자체가 없다 - value는 opaque `void*`).
    // 한 엔트리가 정말로 둘로 쪼개져야 하는 경우(가운데를 punch-out),
    // 두 조각 모두 원래와 같은 value 포인터를 그대로 가리키게 되므로
    // - 그 값 객체를 두 개로 실제로 나누는 것은 호출부(예:
    // `ProcessAddressSpaceManager`)의 책임이다.
    bool erase(uint64_t start, uint64_t end);

    // 트리 안의 모든 실제 값(비-gap) 엔트리를 방문한다 - fn(start, end,
    // value). ProcessAddressSpaceManager::unmapAll()(SP-2AAD7C8D §2)처럼
    // "지금 등록된 모든 범위를 알아야" 하는 소비자를 위해 추가했다(§3.3
    // 원 설계엔 없던 구현 세부 - RM-23F4B687 §4 취지). v2는 트리 전체를
    // 재귀적으로 순회한다(내부 노드는 그냥 통과, 리프에서만 fn 호출).
    template <typename Fn>
    void forEach(Fn&& fn) const {
        forEachNode(_root, 0, kMapleTreeSpanMax(), fn);
    }

private:
    struct Span {
        uint64_t lower;
        uint64_t upper;
        void* value;  // leaf: nullptr = gap / 내부: 항상 태깅된 자식 포인터(non-null)
    };

    // 분할이 일어났을 때 그 사실과 새로 생긴 형제(및 분리 경계값)를
    // 위로 전달하는 값 - insertIntoLeaf/insertIntoInternal의 반환값.
    struct SplitResult {
        bool split = false;
        uint64_t separatorKey = 0;
        MapleArangeNode* sibling = nullptr;
    };

    // erase()가 리프까지 내려가며 지나온 조상 경로를 기록해 두는 데
    // 쓰는 상한 - 분기 계수가 최소 절반(5)씩이라 실질적으로 이보다
    // 훨씬 얕게 끝나지만, 병적인 삽입 패턴에도 안전하도록 넉넉히 잡음.
    static constexpr uint32_t kMaxTreeDepth = 32;

    static uint64_t kMapleTreeSpanMax();

    MapleArangeNode* allocNode(bool leafFlag);
    void freeSubtree(MapleArangeNode* node);

    // node가 [lower,upper] 범위를 담당한다는 전제로 Span 배열로
    // 풀어낸다 - v1의 decompose()를 임의 노드/범위에 대해 쓸 수
    // 있도록 일반화한 버전.
    uint32_t decomposeNode(const MapleArangeNode* node, uint64_t lower, uint64_t upper, Span* outSpans) const;

    // spans(count개, [lower,upper]를 빈틈/겹침 없이 정확히 덮음)로
    // node를 다시 구성한다 - leafFlag에 따라 gap[] 계산 방식이
    // 갈린다(리프: 각 슬롯 자신의 gap 크기 / 내부: 각 자식 서브트리의
    // 최대 gap 집계, childMaxGap() 재사용). count는 항상
    // kMapleArangeSlotCount 이하여야 한다(호출부가 분할 여부를 먼저
    // 판단해 보장).
    void rebuildNode(MapleArangeNode* node, const Span* spans, uint32_t count, bool leafFlag);

    // child 서브트리 전체에서 findGap이 쓸 수 있는 최대 단일 gap -
    // 리프면 자기 gap[] 중 최댓값, 내부면 이미 유지되고 있는
    // gap[maxGapSlot]을 그대로 재사용(재귀적으로 이미 정확함이 보장됨).
    uint64_t childMaxGap(const MapleArangeNode* child) const;

    // leaf에 [start,end]=value를 삽입한다 - leaf가 [lower,upper]를
    // 담당한다는 전제. 겹치거나 여러 슬롯에 걸치면 *outOk=false(트리는
    // 안 건드림). 성공하면 *outOk=true - 분할이 필요 없었으면
    // {split=false}, 필요했으면 {split=true, separatorKey, sibling}.
    SplitResult insertIntoLeaf(MapleArangeNode* leaf, uint64_t lower, uint64_t upper, uint64_t start, uint64_t end,
                                void* value, bool* outOk);

    // internal의 자식들 중 start를 담당하는 자식을 찾아 재귀적으로
    // 삽입한다 - 그 자식이 분할되면 (separatorKey, 새 형제)를 이
    // internal 노드에 편입시키고, 그 결과 이 노드 자신도 꽉 차면
    // 다시 분할해 위로 전파한다. [start,end]가 자식 하나의 경계를
    // 넘으면 *outOk=false(위 클래스 문서의 알려진 한계 3번).
    SplitResult insertIntoInternal(MapleArangeNode* internal, uint64_t lower, uint64_t upper, uint64_t start,
                                    uint64_t end, void* value, bool* outOk);

    // findGap()의 재귀 구현 - node가 [lower,upper]를 담당.
    bool findGapInNode(const MapleArangeNode* node, uint64_t lower, uint64_t upper, uint64_t searchFloor,
                        uint64_t searchCeil, uint64_t size, uint64_t* outStart) const;

    // forEach()의 재귀 구현 - node가 [lower,upper]를 담당.
    template <typename Fn>
    void forEachNode(const MapleArangeNode* node, uint64_t lower, uint64_t upper, Fn&& fn) const {
        if (!node) {
            return;
        }
        Span spans[kMapleArangeSlotCount];
        const uint32_t count = decomposeNode(node, lower, upper, spans);
        for (uint32_t i = 0; i < count; ++i) {
            if (node->leaf) {
                if (spans[i].value != nullptr) {
                    fn(spans[i].lower, spans[i].upper, spans[i].value);
                }
            } else {
                forEachNode(static_cast<const MapleArangeNode*>(kMapleNodePtr(spans[i].value)), spans[i].lower,
                            spans[i].upper, fn);
            }
        }
    }

    MapleArangeNode* _root = nullptr;
    AllocFn _alloc = nullptr;
    FreeFn _free = nullptr;
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_MAPLE_TREE_H
