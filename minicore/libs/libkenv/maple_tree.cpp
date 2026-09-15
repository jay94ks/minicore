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

void MapleTree::ensureAllocator(AllocFn allocFn, FreeFn freeFn) {
    if (_alloc == nullptr) {
        _alloc = allocFn;
        _free = freeFn;
    }
}

void MapleTree::init() {
    if (_root) {
        _free(_root, sizeof(MapleArangeNode));
        _root = nullptr;
    }
}

uint32_t MapleTree::decompose(Span* outSpans) const {
    if (!_root) {
        return 0;
    }
    const uint32_t count = _root->usedSlotCount;
    uint64_t lower = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const bool isLast = (i + 1 == count);
        const uint64_t upper = isLast ? kMapleTreeMaxAddr : _root->pivot[i];
        outSpans[i] = Span{lower, upper, _root->slot[i]};
        if (!isLast) {
            lower = upper + 1;  // pivot[i] < kMapleTreeMaxAddr이 보장됨(그렇지 않으면 i가 마지막이어야 함) - 오버플로우 없음
        }
    }
    return count;
}

bool MapleTree::rebuild(const Span* spans, uint32_t count) {
    // 호출부(store/erase) 책임: _root는 이미 할당돼 있어야 하고,
    // spans는 [0, kMapleTreeMaxAddr]를 빈틈/겹침 없이 정확히 덮어야
    // 하며 인접한 gap끼리는 미리 합쳐져 있어야 한다.
    if (count == 0 || count > kMapleArangeSlotCount) {
        return false;  // v1 한계(멀티레벨 분할 없음) - 트리는 바뀌지 않고 그대로 실패
    }

    _root->usedSlotCount = static_cast<uint16_t>(count);
    uint64_t maxGap = 0;
    uint16_t maxGapIdx = 0;
    for (uint32_t i = 0; i < count; ++i) {
        _root->slot[i] = spans[i].value;
        if (i + 1 < count) {
            _root->pivot[i] = spans[i].upper;
        }
        const uint64_t gapSize = (spans[i].value == nullptr) ? kSpanSize(spans[i].lower, spans[i].upper) : 0;
        _root->gap[i] = gapSize;
        if (gapSize >= maxGap) {
            maxGap = gapSize;
            maxGapIdx = static_cast<uint16_t>(i);
        }
    }
    _root->maxGapSlot = maxGapIdx;
    return true;
}

bool MapleTree::store(uint64_t start, uint64_t end, void* value) {
    if (value == nullptr || start > end) {
        return false;
    }

    if (!_root) {
        void* mem = _alloc(sizeof(MapleArangeNode));
        if (!mem) {
            return false;
        }
        _root = reinterpret_cast<MapleArangeNode*>(mem);
        _root->parent = nullptr;
        _root->usedSlotCount = 1;
        _root->slot[0] = nullptr;
        _root->gap[0] = kMapleTreeMaxAddr;
        _root->maxGapSlot = 0;
    }

    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decompose(spans);

    for (uint32_t i = 0; i < count; ++i) {
        if (start < spans[i].lower || start > spans[i].upper) {
            continue;
        }
        // start는 이 슬롯 안에 있다.
        if (end > spans[i].upper || spans[i].value != nullptr) {
            // 여러 슬롯에 걸치거나(v1 미지원 - 멀티레벨 분할 필요) 이미
            // 값이 있는 슬롯과 겹친다.
            return false;
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

        return rebuild(newSpans, newCount);
    }
    return false;  // count==0(할당 직후엔 있을 수 없음) 등 방어적 폴백
}

void* MapleTree::find(uint64_t addr, uint64_t* outRangeStart, uint64_t* outRangeEnd) const {
    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decompose(spans);
    for (uint32_t i = 0; i < count; ++i) {
        if (addr < spans[i].lower || addr > spans[i].upper) {
            continue;
        }
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
    return nullptr;
}

bool MapleTree::findGap(uint64_t searchFloor, uint64_t searchCeil, uint64_t size, uint64_t* outStart) const {
    if (size == 0 || searchFloor > searchCeil) {
        return false;
    }
    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decompose(spans);
    for (uint32_t i = 0; i < count; ++i) {
        if (spans[i].value != nullptr) {
            continue;
        }
        if (spans[i].upper < searchFloor || spans[i].lower > searchCeil) {
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
    }
    return false;
}

bool MapleTree::erase(uint64_t start, uint64_t end) {
    if (!_root || start > end) {
        return false;
    }

    Span spans[kMapleArangeSlotCount];
    const uint32_t count = decompose(spans);

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

    return rebuild(merged, mergedCount);
}

}  // namespace kernel
