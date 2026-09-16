#include "libkenv/maple_tree.h"

namespace {

// libkenv/types.h엔 <limits> 상당의 UINT64_MAX가 없어(freestanding,
// QU-148B7262) 여기서 직접 정의한다 - MapleTree가 다루는 전체 키
// 공간의 위쪽 끝(포함).
constexpr kernel::uint64_t kMapleTreeMaxAddr = ~kernel::uint64_t(0);

// [lower, upper](포함) 구간의 크기 - upper==kMapleTreeMaxAddr &&
// lower==0(트리 전체가 아직 하나의 큰 gap인 초기 상태)일 때만
// `upper - lower + 1`이 2^64로 오버플로우하므로, 그 한 경우만
// kMapleTreeMaxAddr로 saturate한다(실질적으로 "요청 가능한 어떤
// 크기보다도 크다"는 의미만 필요하므로 정확한 2^64 값 자체는 필요
// 없음). 그 외 모든 경우는 정확한 크기를 오버플로우 없이 돌려준다
// (lower>0이거나 upper<kMapleTreeMaxAddr이면 +1이 안전).
kernel::uint64_t kSpanSize(kernel::uint64_t lower, kernel::uint64_t upper) {
    if (lower == 0 && upper == kMapleTreeMaxAddr) {
        return kMapleTreeMaxAddr;
    }
    return upper - lower + 1;
}

}  // namespace

namespace kernel {

uint64_t MapleTree::kMapleTreeSpanMax() {
    return kMapleTreeMaxAddr;
}

void MapleTree::ensureAllocator(AllocFn allocFn, FreeFn freeFn) {
    if (_alloc == nullptr) {
        _alloc = allocFn;
        _free = freeFn;
    }
}

MapleArangeNode* MapleTree::allocNode(bool leafFlag) {
    void* mem = _alloc(sizeof(MapleArangeNode));
    if (!mem) {
        return nullptr;
    }
    auto* node = reinterpret_cast<MapleArangeNode*>(mem);
    node->parent = nullptr;
    node->leaf = leafFlag;
    node->usedSlotCount = 0;
    node->maxGapSlot = 0;
    return node;
}

void MapleTree::freeSubtree(MapleArangeNode* node) {
    if (!node) {
        return;
    }
    if (!node->leaf) {
        for (uint32_t i = 0; i < node->usedSlotCount; ++i) {
            freeSubtree(static_cast<MapleArangeNode*>(kMapleNodePtr(node->slot[i])));
        }
    }
    _free(node, sizeof(MapleArangeNode));
}

void MapleTree::init() {
    if (_root) {
        freeSubtree(_root);
        _root = nullptr;
    }

    // 빈 트리는 슬롯 1개짜리 큰 gap 하나로 시작한다(MapleArangeNode
    // 문서 주석 참고) - **버그 수정(PN-012E8C1A 검증 중 실측 발견,
    // 2026-09-15)**: 예전엔 이 함수가 _root를 nullptr로만 남겨 뒀는데,
    // find()/findGap()은 store()와 달리 const라서 지연 생성을 할 수
    // 없어 init() 직후 첫 findGap() 호출이 항상 실패했다 - 여기서
    // 즉시 그 불변조건을 채운다(할당 실패 시엔 _root가 nullptr로
    // 남고, store()의 기존 지연 생성 분기가 다음 store() 시점에 다시
    // 시도한다 - 비블로킹 정책 그대로 유지).
    _root = allocNode(true);
    if (!_root) {
        return;
    }
    const Span initial[1] = {Span{0, kMapleTreeMaxAddr, nullptr}};
    rebuildNode(_root, initial, 1, true);
}

void MapleTree::destroy() {
    if (_root) {
        freeSubtree(_root);
        _root = nullptr;
    }
}

uint32_t MapleTree::decomposeNode(const MapleArangeNode* node, uint64_t lower, uint64_t upper, Span* outSpans) const {
    if (!node) {
        return 0;
    }
    const uint32_t count = node->usedSlotCount;
    uint64_t cur = lower;
    for (uint32_t i = 0; i < count; ++i) {
        const bool isLast = (i + 1 == count);
        const uint64_t hi = isLast ? upper : node->pivot[i];
        outSpans[i] = Span{cur, hi, node->slot[i]};
        if (!isLast) {
            cur = hi + 1;  // pivot[i] < upper가 보장됨(그렇지 않으면 i가 마지막이어야 함) - 오버플로우 없음
        }
    }
    return count;
}

uint64_t MapleTree::childMaxGap(const MapleArangeNode* child) const {
    if (!child) {
        return 0;
    }
    if (child->leaf) {
        uint64_t maxGap = 0;
        for (uint32_t i = 0; i < child->usedSlotCount; ++i) {
            if (child->gap[i] > maxGap) {
                maxGap = child->gap[i];
            }
        }
        return maxGap;
    }
    return child->gap[child->maxGapSlot];
}

void MapleTree::rebuildNode(MapleArangeNode* node, const Span* spans, uint32_t count, bool leafFlag) {
    node->leaf = leafFlag;
    node->usedSlotCount = static_cast<uint16_t>(count);
    for (uint32_t i = 0; i < count; ++i) {
        node->slot[i] = spans[i].value;
        if (i + 1 < count) {
            node->pivot[i] = spans[i].upper;
        }
    }

    uint64_t maxGap = 0;
    uint16_t maxIdx = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t gapSize;
        if (leafFlag) {
            gapSize = (spans[i].value == nullptr) ? kSpanSize(spans[i].lower, spans[i].upper) : 0;
        } else {
            gapSize = childMaxGap(static_cast<const MapleArangeNode*>(kMapleNodePtr(spans[i].value)));
        }
        node->gap[i] = gapSize;
        if (gapSize >= maxGap) {
            maxGap = gapSize;
            maxIdx = static_cast<uint16_t>(i);
        }
    }
    node->maxGapSlot = maxIdx;
}

MapleTree::SplitResult MapleTree::insertIntoLeaf(MapleArangeNode* leaf, uint64_t lower, uint64_t upper,
                                                  uint64_t start, uint64_t end, void* value, bool* outOk) {
    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decomposeNode(leaf, lower, upper, spans);

    for (uint32_t i = 0; i < count; ++i) {
        if (start < spans[i].lower || start > spans[i].upper) {
            continue;
        }
        if (end > spans[i].upper || spans[i].value != nullptr) {
            // 여러 슬롯에 걸치거나 이미 값이 있는 슬롯과 겹친다.
            *outOk = false;
            return SplitResult{};
        }

        // 이 gap 슬롯을 최대 3조각(왼쪽 gap/값/오른쪽 gap)으로 쪼갠다.
        Span newSpans[kMapleArangeSlotCount + 2];
        uint32_t newCount = 0;
        for (uint32_t j = 0; j < i; ++j) {
            newSpans[newCount++] = spans[j];
        }
        if (start > spans[i].lower) {
            newSpans[newCount++] = Span{spans[i].lower, start - 1, nullptr};
        }
        newSpans[newCount++] = Span{start, end, value};
        if (end < spans[i].upper) {
            newSpans[newCount++] = Span{end + 1, spans[i].upper, nullptr};
        }
        for (uint32_t j = i + 1; j < count; ++j) {
            newSpans[newCount++] = spans[j];
        }

        *outOk = true;
        if (newCount <= kMapleArangeSlotCount) {
            rebuildNode(leaf, newSpans, newCount, true);
            return SplitResult{};
        }

        // 노드가 꽉 찼다 - 절반씩 나눠 형제 리프를 새로 만든다(v2,
        // PN-38D17292).
        const uint32_t leftCount = (newCount + 1) / 2;
        const uint32_t rightCount = newCount - leftCount;
        MapleArangeNode* sibling = allocNode(true);
        if (!sibling) {
            *outOk = false;
            return SplitResult{};
        }
        const uint64_t separator = newSpans[leftCount - 1].upper;
        rebuildNode(leaf, newSpans, leftCount, true);
        rebuildNode(sibling, newSpans + leftCount, rightCount, true);
        return SplitResult{true, separator, sibling};
    }

    *outOk = false;
    return SplitResult{};
}

MapleTree::SplitResult MapleTree::insertIntoInternal(MapleArangeNode* internal, uint64_t lower, uint64_t upper,
                                                      uint64_t start, uint64_t end, void* value, bool* outOk) {
    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decomposeNode(internal, lower, upper, spans);

    for (uint32_t i = 0; i < count; ++i) {
        if (start < spans[i].lower || start > spans[i].upper) {
            continue;
        }
        if (end > spans[i].upper) {
            // 요청 범위가 이 자식 하나의 경계를 넘어선다 - 클래스
            // 문서의 알려진 한계 3번, 트리는 안 건드리고 실패한다.
            *outOk = false;
            return SplitResult{};
        }

        auto* child = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i].value));
        SplitResult childResult = child->leaf
                                       ? insertIntoLeaf(child, spans[i].lower, spans[i].upper, start, end, value, outOk)
                                       : insertIntoInternal(child, spans[i].lower, spans[i].upper, start, end, value,
                                                             outOk);
        if (!*outOk) {
            return SplitResult{};
        }

        if (!childResult.split) {
            // 자식 내용이 바뀌었을 뿐 구조는 안 바뀜 - 이 레벨의 gap
            // 집계만 갱신.
            internal->gap[i] = childMaxGap(child);
            uint64_t maxGap = 0;
            uint16_t maxIdx = 0;
            for (uint32_t j = 0; j < internal->usedSlotCount; ++j) {
                if (internal->gap[j] >= maxGap) {
                    maxGap = internal->gap[j];
                    maxIdx = static_cast<uint16_t>(j);
                }
            }
            internal->maxGapSlot = maxIdx;
            return SplitResult{};
        }

        // 자식이 분할됐다 - (separatorKey, 새 형제)를 이 노드에 편입.
        Span newSpans[kMapleArangeSlotCount + 2];
        uint32_t newCount = 0;
        for (uint32_t j = 0; j < i; ++j) {
            newSpans[newCount++] = spans[j];
        }
        newSpans[newCount++] =
            Span{spans[i].lower, childResult.separatorKey, kMapleTagNode(child, MapleNodeType::Arange64)};
        newSpans[newCount++] = Span{childResult.separatorKey + 1, spans[i].upper,
                                     kMapleTagNode(childResult.sibling, MapleNodeType::Arange64)};
        for (uint32_t j = i + 1; j < count; ++j) {
            newSpans[newCount++] = spans[j];
        }

        if (newCount <= kMapleArangeSlotCount) {
            rebuildNode(internal, newSpans, newCount, false);
            return SplitResult{};
        }

        const uint32_t leftCount = (newCount + 1) / 2;
        const uint32_t rightCount = newCount - leftCount;
        MapleArangeNode* sibling = allocNode(false);
        if (!sibling) {
            *outOk = false;
            return SplitResult{};
        }
        const uint64_t separator = newSpans[leftCount - 1].upper;
        rebuildNode(internal, newSpans, leftCount, false);
        rebuildNode(sibling, newSpans + leftCount, rightCount, false);
        return SplitResult{true, separator, sibling};
    }

    *outOk = false;
    return SplitResult{};
}

bool MapleTree::store(uint64_t start, uint64_t end, void* value) {
    if (value == nullptr || start > end) {
        return false;
    }

    if (!_root) {
        _root = allocNode(true);
        if (!_root) {
            return false;
        }
        const Span initial[1] = {Span{0, kMapleTreeMaxAddr, nullptr}};
        rebuildNode(_root, initial, 1, true);
    }

    bool ok = true;
    SplitResult result = _root->leaf ? insertIntoLeaf(_root, 0, kMapleTreeMaxAddr, start, end, value, &ok)
                                      : insertIntoInternal(_root, 0, kMapleTreeMaxAddr, start, end, value, &ok);
    if (!ok) {
        return false;
    }
    if (result.split) {
        // 루트 자체가 분할됐다 - 새 내부 루트를 만들어 트리 높이를
        // 하나 늘린다.
        MapleArangeNode* newRoot = allocNode(false);
        if (!newRoot) {
            // 클래스 문서의 알려진 한계 2번 - 이 지점에서 실패하면
            // 트리는 이미 부분적으로 갱신된 상태(옛 루트가 반으로
            // 쪼개진 채)로 남는다. v1 규모 슬랩 고갈에서만 발현될
            // 수 있는 극단적 경로라 실측된 적 없음 - 재발하면 재검토.
            return false;
        }
        const Span rootSpans[2] = {
            Span{0, result.separatorKey, kMapleTagNode(_root, MapleNodeType::Arange64)},
            Span{result.separatorKey + 1, kMapleTreeMaxAddr, kMapleTagNode(result.sibling, MapleNodeType::Arange64)},
        };
        rebuildNode(newRoot, rootSpans, 2, false);
        _root = newRoot;
    }
    return true;
}

void* MapleTree::find(uint64_t addr, uint64_t* outRangeStart, uint64_t* outRangeEnd) const {
    const MapleArangeNode* node = _root;
    uint64_t lower = 0;
    uint64_t upper = kMapleTreeMaxAddr;

    while (node) {
        Span spans[kMapleArangeSlotCount];
        const uint32_t count = decomposeNode(node, lower, upper, spans);
        bool advanced = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (addr < spans[i].lower || addr > spans[i].upper) {
                continue;
            }
            if (node->leaf) {
                if (spans[i].value == nullptr) {
                    return nullptr;  // gap 안
                }
                if (outRangeStart) {
                    *outRangeStart = spans[i].lower;
                }
                if (outRangeEnd) {
                    *outRangeEnd = spans[i].upper;
                }
                return spans[i].value;
            }
            node = static_cast<const MapleArangeNode*>(kMapleNodePtr(spans[i].value));
            lower = spans[i].lower;
            upper = spans[i].upper;
            advanced = true;
            break;
        }
        if (!advanced) {
            return nullptr;
        }
    }
    return nullptr;
}

bool MapleTree::findGapInNode(const MapleArangeNode* node, uint64_t lower, uint64_t upper, uint64_t searchFloor,
                               uint64_t searchCeil, uint64_t size, uint64_t* outStart) const {
    if (!node) {
        return false;
    }
    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decomposeNode(node, lower, upper, spans);

    for (uint32_t i = 0; i < count; ++i) {
        if (spans[i].upper < searchFloor || spans[i].lower > searchCeil) {
            continue;
        }
        if (node->leaf) {
            if (spans[i].value != nullptr) {
                continue;
            }
            const uint64_t lo = spans[i].lower < searchFloor ? searchFloor : spans[i].lower;
            const uint64_t hi = spans[i].upper > searchCeil ? searchCeil : spans[i].upper;
            if (hi < lo) {
                continue;
            }
            if (kSpanSize(lo, hi) >= size) {
                if (outStart) {
                    *outStart = lo;
                }
                return true;
            }
        } else {
            if (node->gap[i] < size) {
                continue;  // 이 자식 서브트리엔 충분한 gap이 없음 - 통째로 건너뜀(§3.3 취지)
            }
            const auto* child = static_cast<const MapleArangeNode*>(kMapleNodePtr(spans[i].value));
            if (findGapInNode(child, spans[i].lower, spans[i].upper, searchFloor, searchCeil, size, outStart)) {
                return true;
            }
        }
    }
    return false;
}

bool MapleTree::findGap(uint64_t searchFloor, uint64_t searchCeil, uint64_t size, uint64_t* outStart) const {
    if (size == 0 || searchFloor > searchCeil) {
        return false;
    }
    return findGapInNode(_root, 0, kMapleTreeMaxAddr, searchFloor, searchCeil, size, outStart);
}

bool MapleTree::erase(uint64_t start, uint64_t end) {
    if (!_root || start > end) {
        return false;
    }

    struct PathEntry {
        MapleArangeNode* node;
        uint32_t childIndex;
    };
    PathEntry path[kMaxTreeDepth];
    uint32_t depth = 0;

    MapleArangeNode* node = _root;
    uint64_t lower = 0;
    uint64_t upper = kMapleTreeMaxAddr;
    while (node && !node->leaf) {
        Span spans[kMapleArangeSlotCount];
        const uint32_t count = decomposeNode(node, lower, upper, spans);
        bool advanced = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (start < spans[i].lower || start > spans[i].upper) {
                continue;
            }
            if (depth < kMaxTreeDepth) {
                path[depth++] = PathEntry{node, i};
            }
            node = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i].value));
            lower = spans[i].lower;
            upper = spans[i].upper;
            advanced = true;
            break;
        }
        if (!advanced) {
            return false;
        }
    }
    if (!node) {
        return false;
    }

    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decomposeNode(node, lower, upper, spans);

    Span newSpans[kMapleArangeSlotCount + 2];
    uint32_t newCount = 0;
    bool erasedSomething = false;

    for (uint32_t i = 0; i < count; ++i) {
        const Span& s = spans[i];
        if (end < s.lower || start > s.upper || s.value == nullptr) {
            newSpans[newCount++] = s;  // 안 겹치거나 이미 gap - 그대로 유지
            continue;
        }
        erasedSomething = true;

        if (start > s.lower) {
            newSpans[newCount++] = Span{s.lower, start - 1, s.value};  // 왼쪽 남은 조각(같은 값)
        }
        const uint64_t midLower = start > s.lower ? start : s.lower;
        const uint64_t midUpper = end < s.upper ? end : s.upper;
        newSpans[newCount++] = Span{midLower, midUpper, nullptr};  // 지워진 가운데 - gap
        if (end < s.upper) {
            newSpans[newCount++] = Span{end + 1, s.upper, s.value};  // 오른쪽 남은 조각(같은 값)
        }
    }

    if (!erasedSomething) {
        return false;  // [start,end]가 처음부터 전부 gap이었음
    }

    // 인접한 gap끼리 합친다(rebuild 전제 조건 + 슬롯 예산 절약).
    Span merged[kMapleArangeSlotCount + 2];
    uint32_t mergedCount = 0;
    for (uint32_t i = 0; i < newCount; ++i) {
        if (mergedCount > 0 && merged[mergedCount - 1].value == nullptr && newSpans[i].value == nullptr) {
            merged[mergedCount - 1].upper = newSpans[i].upper;
        } else {
            merged[mergedCount++] = newSpans[i];
        }
    }

    rebuildNode(node, merged, mergedCount, true);

    // 리프에서 지워진 만큼 gap이 늘었을 수 있으니, 지나온 조상 전부의
    // gap 집계를 아래에서 위로 다시 계산한다(트리 병합/축소는 하지
    // 않는다 - 클래스 문서의 알려진 한계 1번).
    for (uint32_t d = depth; d-- > 0;) {
        MapleArangeNode* ancestor = path[d].node;
        const uint32_t idx = path[d].childIndex;
        const auto* child = static_cast<const MapleArangeNode*>(kMapleNodePtr(ancestor->slot[idx]));
        ancestor->gap[idx] = childMaxGap(child);

        uint64_t maxGap = 0;
        uint16_t maxIdx = 0;
        for (uint32_t j = 0; j < ancestor->usedSlotCount; ++j) {
            if (ancestor->gap[j] >= maxGap) {
                maxGap = ancestor->gap[j];
                maxIdx = static_cast<uint16_t>(j);
            }
        }
        ancestor->maxGapSlot = maxIdx;
    }

    return true;
}

}  // namespace kernel
