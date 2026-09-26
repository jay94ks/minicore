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

void MapleTree::recomputeMaxGapSlot(MapleArangeNode* node) {
    uint64_t maxGap = 0;
    uint16_t maxIdx = 0;
    for (uint32_t j = 0; j < node->usedSlotCount; ++j) {
        if (node->gap[j] >= maxGap) {
            maxGap = node->gap[j];
            maxIdx = static_cast<uint16_t>(j);
        }
    }
    node->maxGapSlot = maxIdx;
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

bool MapleTree::insertIntoSpanArray(const Span* spans, uint32_t count, uint64_t start, uint64_t end, void* value,
                                     Span* outNewSpans, uint32_t* outNewCount) const {
    uint32_t p = count;
    for (uint32_t k = 0; k < count; ++k) {
        if (start >= spans[k].lower && start <= spans[k].upper) {
            p = k;
            break;
        }
    }
    uint32_t q = count;
    for (uint32_t k = 0; k < count; ++k) {
        if (end >= spans[k].lower && end <= spans[k].upper) {
            q = k;
            break;
        }
    }
    if (p == count || q == count || q < p) {
        return false;
    }
    for (uint32_t k = p; k <= q; ++k) {
        if (spans[k].value != nullptr) {
            return false;
        }
    }

    uint32_t newCount = 0;
    for (uint32_t k = 0; k < p; ++k) {
        outNewSpans[newCount++] = spans[k];
    }
    if (start > spans[p].lower) {
        outNewSpans[newCount++] = Span{spans[p].lower, start - 1, nullptr};
    }
    outNewSpans[newCount++] = Span{start, end, value};
    if (end < spans[q].upper) {
        outNewSpans[newCount++] = Span{end + 1, spans[q].upper, nullptr};
    }
    for (uint32_t k = q + 1; k < count; ++k) {
        outNewSpans[newCount++] = spans[k];
    }
    *outNewCount = newCount;
    return true;
}

MapleTree::SplitResult MapleTree::spanTwoLeaves(MapleArangeNode* internal, const Span* spans, uint32_t count,
                                                 uint32_t i, uint64_t start, uint64_t end, void* value, bool* outOk) {
    auto* childA = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i].value));
    auto* childB = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i + 1].value));

    Span combined[kMapleArangeSlotCount * 2];
    uint32_t combinedCount = decomposeNode(childA, spans[i].lower, spans[i].upper, combined);
    combinedCount += decomposeNode(childB, spans[i + 1].lower, spans[i + 1].upper, combined + combinedCount);

    Span merged[kMapleArangeSlotCount * 2 + 2];
    uint32_t mergedCount = 0;
    if (!insertIntoSpanArray(combined, combinedCount, start, end, value, merged, &mergedCount)) {
        *outOk = false;
        return SplitResult{};
    }
    if (mergedCount > kMapleArangeSlotCount * 2) {
        // [안전장치] 두 리프가 이미 꽉 찬 상태에서 경계에 걸친 삽입이
        // 슬롯을 하나 더 늘리는 극단적인 경우 - 2개의 리프로도 도저히
        // 못 담아 3개가 필요해지므로(이 v3의 스코프 "정확히 2개"를
        // 벗어남) 트리는 안 건드리고 실패한다(merge/redistribute 아래
        // 두 분기 모두 kMapleArangeSlotCount*2개까지만 담을 수 있다는
        // 전제 위에 있다 - 이 검사 없이 진행하면 리프당 10슬롯을
        // 넘는 rebuildNode 호출로 이어져 메모리를 손상시킨다).
        *outOk = false;
        return SplitResult{};
    }
    *outOk = true;

    Span internalNewSpans[kMapleArangeSlotCount];
    uint32_t internalNewCount = 0;
    for (uint32_t k = 0; k < i; ++k) {
        internalNewSpans[internalNewCount++] = spans[k];
    }

    if (mergedCount <= kMapleArangeSlotCount) {
        // 리프 하나로 합쳐진다 - childB는 반납, internal 슬롯 하나가 준다.
        rebuildNode(childA, merged, mergedCount, true);
        freeSubtree(childB);
        internalNewSpans[internalNewCount++] =
            Span{spans[i].lower, spans[i + 1].upper, kMapleTagNode(childA, MapleNodeType::Arange64)};
    } else {
        // 넘친다 - 기존 두 리프에 다시 반씩 재분배(새 할당 없음).
        const uint32_t leftCount = (mergedCount + 1) / 2;
        const uint32_t rightCount = mergedCount - leftCount;
        const uint64_t newSeparator = merged[leftCount - 1].upper;
        rebuildNode(childA, merged, leftCount, true);
        rebuildNode(childB, merged + leftCount, rightCount, true);
        internalNewSpans[internalNewCount++] =
            Span{spans[i].lower, newSeparator, kMapleTagNode(childA, MapleNodeType::Arange64)};
        internalNewSpans[internalNewCount++] =
            Span{newSeparator + 1, spans[i + 1].upper, kMapleTagNode(childB, MapleNodeType::Arange64)};
    }

    for (uint32_t k = i + 2; k < count; ++k) {
        internalNewSpans[internalNewCount++] = spans[k];
    }

    // internal 자신은 슬롯 수가 줄거나 그대로일 뿐 늘지 않으므로
    // 분할이 필요 없다(클래스 문서 §2.2 항목4 참고).
    rebuildNode(internal, internalNewSpans, internalNewCount, false);
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
            // 요청 범위가 이 자식 하나의 경계를 넘어선다 - [v3] 정확히
            // 다음 형제 하나까지만 걸치고 둘 다 리프면 spanTwoLeaves()
            // 로 위임, 그 외(2개 이상 건너뛰거나 자식이 내부 노드)는
            // 여전히 실패(클래스 문서의 알려진 한계 3번).
            if (i + 1 >= count || end < spans[i + 1].lower || end > spans[i + 1].upper) {
                *outOk = false;
                return SplitResult{};
            }
            auto* childA = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i].value));
            auto* childB = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i + 1].value));
            if (!childA->leaf || !childB->leaf) {
                *outOk = false;
                return SplitResult{};
            }
            return spanTwoLeaves(internal, spans, count, i, start, end, value, outOk);
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
            recomputeMaxGapSlot(internal);
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

bool MapleTree::erasePartInLeaf(MapleArangeNode* node, uint64_t lower, uint64_t upper, uint64_t start,
                                 uint64_t end) {
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
        return false;  // [start,end]가 이 리프 범위 안에서는 처음부터 전부 gap이었음
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

    // [안전장치, 2026-09-26, PN-2EA94B1A 착수 중 발견 - v2부터 있던
    // 별개의 잠재 버그] 리프가 이미 꽉 찬 상태(usedSlotCount==
    // kMapleArangeSlotCount)에서 어느 한 값 슬롯의 "가운데"를
    // punch-out하면(양옆 슬롯이 gap이 아니라 이 새 gap과 안 합쳐짐)
    // 슬롯 하나가 최대 둘 더 늘 수 있다(왼쪽 조각+가운데 gap+오른쪽
    // 조각) - erase()는 insertIntoLeaf()와 달리 넘칠 때 리프를
    // 분할하는 절차가 아예 없어(이 v3 이전부터 없었음, 별도 계획으로
    // 분리 등록) rebuildNode를 그대로 불렀다면 MapleArangeNode의
    // 고정 배열(슬롯 10개)을 넘겨 써 메모리를 손상시켰다. 리프 분할을
    // 지원하는 진짜 수정 전까지는, 트리를 부분적으로도 건드리지 않고
    // 안전하게 거부한다(호출부는 false를 "아무것도 안 지워짐"과
    // 구분 못 하지만, 손상보다는 훨씬 낫다).
    if (mergedCount > kMapleArangeSlotCount) {
        return false;
    }

    rebuildNode(node, merged, mergedCount, true);
    return true;
}

bool MapleTree::eraseAcrossTwoLeaves(MapleArangeNode* leafA, uint64_t lowerA, uint64_t upperA, MapleArangeNode* leafB,
                                      uint64_t lowerB, uint64_t upperB, uint64_t start, uint64_t end) {
    const bool erasedA = erasePartInLeaf(leafA, lowerA, upperA, start, end);
    const bool erasedB = erasePartInLeaf(leafB, lowerB, upperB, start, end);
    return erasedA || erasedB;
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
    for (;;) {
        if (!node) {
            return false;
        }
        if (node->leaf) {
            break;
        }

        Span spans[kMapleArangeSlotCount];
        const uint32_t count = decomposeNode(node, lower, upper, spans);
        uint32_t i = count;
        for (uint32_t k = 0; k < count; ++k) {
            if (start >= spans[k].lower && start <= spans[k].upper) {
                i = k;
                break;
            }
        }
        if (i == count) {
            return false;
        }

        if (end <= spans[i].upper) {
            // 기존 단일 자식 경로(변경 없음).
            if (depth < kMaxTreeDepth) {
                path[depth++] = PathEntry{node, i};
            }
            node = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i].value));
            lower = spans[i].lower;
            upper = spans[i].upper;
            continue;
        }

        // [v3] end가 이 자식의 경계를 넘는다 - 정확히 다음 형제 하나까지,
        // 그리고 둘 다 리프일 때만 지원(store()의 spanTwoLeaves()와
        // 동일한 스코프 제한).
        if (i + 1 >= count || end < spans[i + 1].lower || end > spans[i + 1].upper) {
            return false;
        }
        auto* childA = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i].value));
        auto* childB = static_cast<MapleArangeNode*>(kMapleNodePtr(spans[i + 1].value));
        if (!childA->leaf || !childB->leaf) {
            return false;
        }

        if (!eraseAcrossTwoLeaves(childA, spans[i].lower, spans[i].upper, childB, spans[i + 1].lower,
                                   spans[i + 1].upper, start, end)) {
            return false;
        }

        // 두 리프 다 gap이 바뀌었을 수 있으니 이 레벨(node)부터 위로
        // 다시 계산한다(트리 병합/축소는 하지 않는다 - 클래스 문서의
        // 알려진 한계 1번).
        node->gap[i] = childMaxGap(childA);
        node->gap[i + 1] = childMaxGap(childB);
        recomputeMaxGapSlot(node);
        for (uint32_t d = depth; d-- > 0;) {
            MapleArangeNode* ancestor = path[d].node;
            const uint32_t idx = path[d].childIndex;
            const auto* child = static_cast<const MapleArangeNode*>(kMapleNodePtr(ancestor->slot[idx]));
            ancestor->gap[idx] = childMaxGap(child);
            recomputeMaxGapSlot(ancestor);
        }
        return true;
    }

    if (!erasePartInLeaf(node, lower, upper, start, end)) {
        return false;
    }

    // 리프에서 지워진 만큼 gap이 늘었을 수 있으니, 지나온 조상 전부의
    // gap 집계를 아래에서 위로 다시 계산한다(트리 병합/축소는 하지
    // 않는다 - 클래스 문서의 알려진 한계 1번).
    for (uint32_t d = depth; d-- > 0;) {
        MapleArangeNode* ancestor = path[d].node;
        const uint32_t idx = path[d].childIndex;
        const auto* child = static_cast<const MapleArangeNode*>(kMapleNodePtr(ancestor->slot[idx]));
        ancestor->gap[idx] = childMaxGap(child);
        recomputeMaxGapSlot(ancestor);
    }

    return true;
}

}  // namespace kernel
