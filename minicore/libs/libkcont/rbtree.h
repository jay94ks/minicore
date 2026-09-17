#ifndef MINICORE_LIBS_LIBKCONT_RBTREE_H
#define MINICORE_LIBS_LIBKCONT_RBTREE_H

#include "libkenv/types.h"

// libkcont: Rbtree<T, Traits>/RbMultiTree<T, Traits>(SP-FAF768AB §4,
// §4-A) - 침습적 레드-블랙 트리(정렬된 O(log n) 조회). find/insert/
// remove/first/next의 실제 회전(rotateLeft/rotateRight)/재색칠 로직은
// 표준 CLRS 레드-블랙 트리 알고리즘 그대로다(자체 발명 없음,
// RM-23F4B687 §4 - 검증된 알고리즘 재구현, 새 알고리즘 설계 아님).
// CLRS 원 의사코드는 항상 존재하는 sentinel(T.nil) 리프 노드를 쓰지만,
// 이 구현은 별도 sentinel 인스턴스 없이 nullptr을 "항상 검정(Black)"
// 리프로 취급하는 흔한 실전 변형을 쓴다(RbColorOf() 헬퍼) - 알고리즘
// 자체는 동일, sentinel 유무만 다른 표준적인 어댑테이션이다.
//
// Traits 관례(§0-B): 정렬 키가 필요한 컨테이너 공통 형태 - Key 타입 +
// keyOf() 정적 함수 + RbNode 멤버 포인터.
//
//   template <typename T>
//   struct MyTreeTraits {
//       using Key = Uid;
//       static Key keyOf(const T& item) { return item.uid; }
//       static constexpr RbNode T::* Link = &T::treeLink;
//   };

namespace kernel {

enum class RbColor : uint8_t { Red, Black };

// List(intrusive_list.h)의 Node와 같은 역할이지만 트리용 - 역시
// 완전히 타입 무관, T와의 연결은 Rbtree<T, Traits>/RbMultiTree<T,
// Traits>가 Traits::Link로 계산.
struct RbNode {
    RbNode* parent = nullptr;
    RbNode* left = nullptr;
    RbNode* right = nullptr;
    RbColor color = RbColor::Red;
};

namespace detail {

inline RbColor kRbColorOf(RbNode* node) {
    return node ? node->color : RbColor::Black;
}

// Rbtree<T, Traits>와 RbMultiTree<T, Traits>가 공유하는 CLRS 표준
// 알고리즘 코어 - 두 컨테이너의 유일한 차이는 insert() 시 "중복 키를
// 거부하는지"뿐이라(§4-A), 회전/fixup/삭제처럼 까다롭고 실수하기 쉬운
// 부분은 여기 한 곳에서만 구현해 두 컨테이너 모두가 재사용한다.
class RbCore {
public:
    static void rotateLeft(RbNode** root, RbNode* x) {
        RbNode* y = x->right;
        x->right = y->left;
        if (y->left) {
            y->left->parent = x;
        }
        y->parent = x->parent;
        if (!x->parent) {
            *root = y;
        } else if (x == x->parent->left) {
            x->parent->left = y;
        } else {
            x->parent->right = y;
        }
        y->left = x;
        x->parent = y;
    }

    static void rotateRight(RbNode** root, RbNode* x) {
        RbNode* y = x->left;
        x->left = y->right;
        if (y->right) {
            y->right->parent = x;
        }
        y->parent = x->parent;
        if (!x->parent) {
            *root = y;
        } else if (x == x->parent->right) {
            x->parent->right = y;
        } else {
            x->parent->left = y;
        }
        y->right = x;
        x->parent = y;
    }

    // z는 이미 이진 탐색 트리 규칙대로 삽입 위치에 연결되고 Red로
    // 표시된 상태로 들어온다(호출자 책임) - CLRS RB-INSERT-FIXUP
    // 그대로 위반된 레드-블랙 불변조건만 복구한다.
    static void insertFixup(RbNode** root, RbNode* z) {
        while (z->parent && z->parent->color == RbColor::Red) {
            RbNode* parent = z->parent;
            RbNode* grandparent = parent->parent;
            if (parent == grandparent->left) {
                RbNode* uncle = grandparent->right;
                if (kRbColorOf(uncle) == RbColor::Red) {
                    parent->color = RbColor::Black;
                    uncle->color = RbColor::Black;
                    grandparent->color = RbColor::Red;
                    z = grandparent;
                } else {
                    if (z == parent->right) {
                        z = parent;
                        rotateLeft(root, z);
                        parent = z->parent;
                        grandparent = parent->parent;
                    }
                    parent->color = RbColor::Black;
                    grandparent->color = RbColor::Red;
                    rotateRight(root, grandparent);
                }
            } else {
                RbNode* uncle = grandparent->left;
                if (kRbColorOf(uncle) == RbColor::Red) {
                    parent->color = RbColor::Black;
                    uncle->color = RbColor::Black;
                    grandparent->color = RbColor::Red;
                    z = grandparent;
                } else {
                    if (z == parent->left) {
                        z = parent;
                        rotateRight(root, z);
                        parent = z->parent;
                        grandparent = parent->parent;
                    }
                    parent->color = RbColor::Black;
                    grandparent->color = RbColor::Red;
                    rotateLeft(root, grandparent);
                }
            }
        }
        (*root)->color = RbColor::Black;
    }

    static void transplant(RbNode** root, RbNode* u, RbNode* v) {
        if (!u->parent) {
            *root = v;
        } else if (u == u->parent->left) {
            u->parent->left = v;
        } else {
            u->parent->right = v;
        }
        if (v) {
            v->parent = u->parent;
        }
    }

    static RbNode* minimum(RbNode* node) {
        while (node->left) {
            node = node->left;
        }
        return node;
    }

    static RbNode* successor(RbNode* node) {
        if (node->right) {
            return minimum(node->right);
        }
        RbNode* parent = node->parent;
        while (parent && node == parent->right) {
            node = parent;
            parent = parent->parent;
        }
        return parent;
    }

    // CLRS RB-DELETE 그대로 - z를 트리에서 제거한다(호출부가 이미
    // z가 이 트리 안에 있음을 보장). sentinel 인스턴스가 없는 이
    // 구현은 "삭제로 실제 트리에서 사라진 색"이 Black일 때 fixup이
    // 필요한 지점(x)이 nullptr일 수 있어, 그 경우를 위해 부모
    // (xParent)를 별도로 들고 다닌다(CLRS 원 의사코드의 T.nil
    // sentinel 대신 nullptr을 쓰는 이 구현의 대가).
    static void remove(RbNode** root, RbNode* z) {
        RbNode* y = z;
        RbColor yOriginalColor = y->color;
        RbNode* x = nullptr;
        RbNode* xParent = nullptr;

        if (!z->left) {
            x = z->right;
            xParent = z->parent;
            transplant(root, z, z->right);
        } else if (!z->right) {
            x = z->left;
            xParent = z->parent;
            transplant(root, z, z->left);
        } else {
            y = minimum(z->right);
            yOriginalColor = y->color;
            x = y->right;
            if (y->parent == z) {
                xParent = y;
            } else {
                xParent = y->parent;
                transplant(root, y, y->right);
                y->right = z->right;
                y->right->parent = y;
            }
            transplant(root, z, y);
            y->left = z->left;
            y->left->parent = y;
            y->color = z->color;
        }

        if (yOriginalColor == RbColor::Black) {
            deleteFixup(root, x, xParent);
        }
    }

private:
    static void deleteFixup(RbNode** root, RbNode* x, RbNode* xParent) {
        while (x != *root && kRbColorOf(x) == RbColor::Black) {
            if (x == xParent->left) {
                RbNode* w = xParent->right;
                if (kRbColorOf(w) == RbColor::Red) {
                    w->color = RbColor::Black;
                    xParent->color = RbColor::Red;
                    rotateLeft(root, xParent);
                    w = xParent->right;
                }
                if (kRbColorOf(w->left) == RbColor::Black && kRbColorOf(w->right) == RbColor::Black) {
                    w->color = RbColor::Red;
                    x = xParent;
                    xParent = x->parent;
                } else {
                    if (kRbColorOf(w->right) == RbColor::Black) {
                        if (w->left) {
                            w->left->color = RbColor::Black;
                        }
                        w->color = RbColor::Red;
                        rotateRight(root, w);
                        w = xParent->right;
                    }
                    w->color = xParent->color;
                    xParent->color = RbColor::Black;
                    if (w->right) {
                        w->right->color = RbColor::Black;
                    }
                    rotateLeft(root, xParent);
                    x = *root;
                    xParent = nullptr;
                }
            } else {
                RbNode* w = xParent->left;
                if (kRbColorOf(w) == RbColor::Red) {
                    w->color = RbColor::Black;
                    xParent->color = RbColor::Red;
                    rotateRight(root, xParent);
                    w = xParent->left;
                }
                if (kRbColorOf(w->right) == RbColor::Black && kRbColorOf(w->left) == RbColor::Black) {
                    w->color = RbColor::Red;
                    x = xParent;
                    xParent = x->parent;
                } else {
                    if (kRbColorOf(w->left) == RbColor::Black) {
                        if (w->right) {
                            w->right->color = RbColor::Black;
                        }
                        w->color = RbColor::Red;
                        rotateLeft(root, w);
                        w = xParent->left;
                    }
                    w->color = xParent->color;
                    xParent->color = RbColor::Black;
                    if (w->left) {
                        w->left->color = RbColor::Black;
                    }
                    rotateRight(root, xParent);
                    x = *root;
                    xParent = nullptr;
                }
            }
        }
        if (x) {
            x->color = RbColor::Black;
        }
    }
};

}  // namespace detail

// Maple Tree(SP-2AAD7C8D)가 VMA 전용으로 이미 검증한 "커널판 균형
// 이진 트리" 패턴을 범용화한 것이지만, Maple Tree 자체(포인터 태깅 +
// range 노드 4종 - Linux mm 이식)를 대체하지 않는다 - VMA는 계속
// Maple Tree.
template <typename T, typename Traits>
class Rbtree {
public:
    using Key = typename Traits::Key;

    T* find(const Key& key) const {
        RbNode* cur = _root;
        while (cur) {
            T* item = kContainerOf(cur);
            const Key itemKey = Traits::keyOf(*item);
            if (key < itemKey) {
                cur = cur->left;
            } else if (itemKey < key) {
                cur = cur->right;
            } else {
                return item;
            }
        }
        return nullptr;
    }

    // 이미 같은 키가 있으면 false(중복 거부) - 중복을 허용하려면
    // RbMultiTree(아래)를 쓴다.
    bool insert(T* item) {
        RbNode* z = &(item->*Traits::Link);
        *z = RbNode{};
        const Key key = Traits::keyOf(*item);

        RbNode* parent = nullptr;
        RbNode* cur = _root;
        while (cur) {
            T* curItem = kContainerOf(cur);
            const Key curKey = Traits::keyOf(*curItem);
            parent = cur;
            if (key < curKey) {
                cur = cur->left;
            } else if (curKey < key) {
                cur = cur->right;
            } else {
                return false;
            }
        }

        z->parent = parent;
        if (!parent) {
            _root = z;
        } else if (key < Traits::keyOf(*kContainerOf(parent))) {
            parent->left = z;
        } else {
            parent->right = z;
        }
        detail::RbCore::insertFixup(&_root, z);
        return true;
    }

    // [정정, 2026-09-17, PN-633BF2D8 TEMP 검증 중 실측 발견 - SP-FAF768AB
    // §4 원안은 `static void remove(T* item)`이었으나 버그였다] List
    // (intrusive_list.h)의 unlink()는 완전히 지역적이라(양쪽 이웃만
    // 다시 잇는다) 어떤 리스트 인스턴스에도 안 묶여도 되지만, 레드-
    // 블랙 트리는 다르다 - z를 지우다가 실제 루트가 바뀌면(z 자신이
    // 루트였거나, deleteFixup의 회전이 맨 위까지 올라온 경우 - 둘 다
    // detail::RbCore::remove가 `*root = ...`로 반영) 그 새 루트를
    // *어딘가에는* 기록해야 한다. static이면 "z에서 parent를 타고
    // 올라가 찾은 루트"를 지역 변수로만 들고 있다가 그 함수가 끝나면
    // 사라져, 이 Rbtree 인스턴스의 `_root` 멤버는 절대 갱신되지
    // 않는다(QEMU 실측 - 루트 근처 원소를 remove() 후 find()가 여전히
    // 찾아버리는 데이터 구조 손상으로 드러남). 그래서 remove()는
    // 반드시 인스턴스 메서드여야 `_root`를 직접 갱신할 수 있다.
    void remove(T* item) {
        RbNode* z = &(item->*Traits::Link);
        detail::RbCore::remove(&_root, z);
    }

    T* first() const { return _root ? kContainerOf(detail::RbCore::minimum(_root)) : nullptr; }

    T* next(T* item) const {
        RbNode* succ = detail::RbCore::successor(&(item->*Traits::Link));
        return succ ? kContainerOf(succ) : nullptr;
    }

    bool empty() const { return _root == nullptr; }

private:
    static T* kContainerOf(RbNode* node) {
        const uint64_t offset = reinterpret_cast<uint64_t>(&(reinterpret_cast<T*>(0)->*Traits::Link));
        return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(node) - offset);
    }

    RbNode* _root = nullptr;
};

// Rbtree<T, Traits>(위)와 노드 구조(RbNode)는 동일하게 재사용하되,
// insert()가 중복 키를 거부하지 않고 항상 성공한다(SP-FAF768AB
// §4-A) - 같은 키의 새 항목은 항상 비교에서 "오른쪽"으로 취급돼
// 기존 동일 키 그룹의 오른쪽 서브트리에 배치된다(삽입 순서가
// in-order 순회 순서와 일치하도록, 표준 멀티맵 관용구). `Traits`는
// Rbtree와 동일한 형태 - 같은 Traits 타입을 Rbtree/RbMultiTree
// 양쪽에 재사용할 수 있다. Map<K,V>(map.h)는 여전히 중복 키 없는
// Rbtree 기반을 유지한다 - 이 타입은 완전히 별도 소비자가 필요할
// 때(같은 키에 여러 값이 달리는 인덱스류) 쓰는 독립적인 타입이다.
template <typename T, typename Traits>
class RbMultiTree {
public:
    using Key = typename Traits::Key;

    void insert(T* item) {
        RbNode* z = &(item->*Traits::Link);
        *z = RbNode{};
        const Key key = Traits::keyOf(*item);

        RbNode* parent = nullptr;
        RbNode* cur = _root;
        bool goLeft = false;
        while (cur) {
            T* curItem = kContainerOf(cur);
            parent = cur;
            if (key < Traits::keyOf(*curItem)) {
                goLeft = true;
                cur = cur->left;
            } else {
                goLeft = false;  // 동률(key >= curKey)이면 항상 오른쪽
                cur = cur->right;
            }
        }

        z->parent = parent;
        if (!parent) {
            _root = z;
        } else if (goLeft) {
            parent->left = z;
        } else {
            parent->right = z;
        }
        detail::RbCore::insertFixup(&_root, z);
    }

    // key 이상인 첫 원소 - 표준 멀티맵 lowerBound와 동일한 발상.
    T* lowerBound(const Key& key) const {
        RbNode* cur = _root;
        RbNode* result = nullptr;
        while (cur) {
            T* item = kContainerOf(cur);
            if (Traits::keyOf(*item) < key) {
                cur = cur->right;
            } else {
                result = cur;
                cur = cur->left;
            }
        }
        return result ? kContainerOf(result) : nullptr;
    }

    // key 초과인 첫 원소 - [lowerBound(key), upperBound(key))가 그
    // 키를 가진 전체 구간이다(호출부가 next()로 순회).
    T* upperBound(const Key& key) const {
        RbNode* cur = _root;
        RbNode* result = nullptr;
        while (cur) {
            T* item = kContainerOf(cur);
            if (key < Traits::keyOf(*item)) {
                result = cur;
                cur = cur->left;
            } else {
                cur = cur->right;
            }
        }
        return result ? kContainerOf(result) : nullptr;
    }

    T* next(T* item) const {
        RbNode* succ = detail::RbCore::successor(&(item->*Traits::Link));
        return succ ? kContainerOf(succ) : nullptr;
    }

    // 특정 인스턴스만 제거(같은 키의 나머지는 유지) - Rbtree::remove와
    // 동일한 이유(위 정정 주석 참고)로 인스턴스 메서드다.
    void remove(T* item) {
        RbNode* z = &(item->*Traits::Link);
        detail::RbCore::remove(&_root, z);
    }

    bool empty() const { return _root == nullptr; }

private:
    static T* kContainerOf(RbNode* node) {
        const uint64_t offset = reinterpret_cast<uint64_t>(&(reinterpret_cast<T*>(0)->*Traits::Link));
        return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(node) - offset);
    }

    RbNode* _root = nullptr;
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKCONT_RBTREE_H
