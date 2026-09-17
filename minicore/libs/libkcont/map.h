#ifndef MINICORE_LIBS_LIBKCONT_MAP_H
#define MINICORE_LIBS_LIBKCONT_MAP_H

#include "libkcont/rbtree.h"

// libkcont: Map<K, V>(SP-FAF768AB §5-A) - Rbtree 기반 정렬 맵(중복
// 키 없음 - 중복 키가 필요하면 rbtree.h의 RbMultiTree를 직접 쓴다).

namespace kernel {

// 호출부가 직접 할당해(스택/슬랩 등, Rbtree가 침습적이므로) insert()
// 에 넘기는 관례 - "무엇을 침습 대상으로 삼는가"는 Rbtree 입장에서
// 자유이므로, Map은 K/V/RbNode를 한 구조체에 묶은 전용 MapEntry를
// 그 대상으로 삼아 "Map을 쓰는 코드는 K/V만 다루면 된다"는 편의를
// 얻는다.
template <typename K, typename V>
struct MapEntry {
    K key;
    V value;
    RbNode link;
};

// §0-B의 Traits 형태 - Rbtree<T, Traits>가 요구하는 Key/keyOf/Link를
// 한 구조체로 묶는다.
template <typename K, typename V>
struct MapEntryTraits {
    using Key = K;
    static K keyOf(const MapEntry<K, V>& entry) { return entry.key; }
    static constexpr RbNode MapEntry<K, V>::* Link = &MapEntry<K, V>::link;
};

template <typename K, typename V>
using Map = Rbtree<MapEntry<K, V>, MapEntryTraits<K, V>>;

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKCONT_MAP_H
