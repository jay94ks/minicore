// libk 호스트 네이티브 단위 테스트 (ADR-077). 외부 테스트 프레임워크
// 없이 최소한의 CHECK 매크로만 쓴다 — libk 자체가 freestanding이라
// 테스트 하니스도 가벼운 편이 그 정신과 맞는다.
#include <cstdio>
#include <utility>

#include <libk/intrusive_list.hpp>
#include <libk/mcs_lock.hpp>
#include <libk/optional.hpp>
#include <libk/result.hpp>
#include <libk/span.hpp>
#include <libk/spinlock.hpp>
#include <libk/ticket_lock.hpp>

namespace {

int g_total = 0;
int g_failures = 0;

}  // namespace

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_total;                                                             \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

enum class my_err { a, b };

void test_result() {
    auto ok = result<int, my_err>::ok(42);
    CHECK(ok.is_ok());
    CHECK(!ok.is_err());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == 42);

    auto err = result<int, my_err>::err(my_err::b);
    CHECK(err.is_err());
    CHECK(err.error() == my_err::b);

    result<int, my_err> moved = std::move(ok);
    CHECK(moved.value() == 42);

    result<int, my_err> e2 = result<int, my_err>::err(my_err::a);
    CHECK(std::move(e2).value_or(99) == 99);

    result<int, my_err> ok2 = result<int, my_err>::ok(7);
    CHECK(std::move(ok2).value_or(99) == 7);

    result<void, my_err> v = result<void, my_err>::ok();
    CHECK(v.is_ok());
    result<void, my_err> ve = result<void, my_err>::err(my_err::a);
    CHECK(ve.is_err());
    CHECK(ve.error() == my_err::a);
}

void test_optional() {
    optional<int> empty;
    CHECK(!empty.has_value());

    optional<int> with_value(5);
    CHECK(with_value.has_value());
    CHECK(with_value.value() == 5);

    optional<int> moved = std::move(with_value);
    CHECK(moved.value() == 5);

    CHECK(optional<int>::none().has_value() == false);

    optional<int> e;
    CHECK(std::move(e).value_or(42) == 42);

    optional<int> em;
    em.emplace(9);
    CHECK(em.has_value() && em.value() == 9);
    em.reset();
    CHECK(!em.has_value());
}

void test_span() {
    int arr[5] = {1, 2, 3, 4, 5};
    span<int> s(arr);
    CHECK(s.size() == 5);
    CHECK(!s.empty());
    CHECK(s[0] == 1 && s[4] == 5);
    CHECK(s.size_bytes() == 5 * sizeof(int));

    span<int> sub = s.subspan(1, 2);
    CHECK(sub.size() == 2 && sub[0] == 2 && sub[1] == 3);

    span<const int> cs = s;  // 비-const -> const 변환은 허용된다.
    CHECK(cs.size() == 5 && cs[2] == 3);

    int sum = 0;
    for (int v : s) {
        sum += v;
    }
    CHECK(sum == 15);
}

struct list_node {
    int value;
    list_hook hook;
};

void test_intrusive_list() {
    intrusive_list<list_node, &list_node::hook> list;
    CHECK(list.empty());

    list_node a{1, {}};
    list_node b{2, {}};
    list_node c{3, {}};

    list.push_back(a);
    list.push_back(b);
    list.push_front(c);
    // 순서: c, a, b
    CHECK(!list.empty());

    auto it = list.begin();
    CHECK(it->value == 3);
    ++it;
    CHECK(it->value == 1);
    ++it;
    CHECK(it->value == 2);
    ++it;
    CHECK(it == list.end());

    decltype(list)::erase(a);
    int sum = 0;
    int count = 0;
    for (auto& n : list) {
        sum += n.value;
        ++count;
    }
    CHECK(count == 2);
    CHECK(sum == 5);  // c(3) + b(2)

    decltype(list)::erase(b);
    decltype(list)::erase(c);
    CHECK(list.empty());
}

void test_spinlock() {
    spinlock lock;
    CHECK(lock.try_lock());
    CHECK(!lock.try_lock());  // 이미 잠김
    lock.unlock();

    CHECK(lock.try_lock());
    lock.unlock();

    lock.lock();
    lock.unlock();
}

void test_ticket_lock() {
    ticket_lock lock;
    // 단일 스레드로는 FIFO 공정성 자체를 관찰할 수 없다 — lock/unlock이
    // 반복해서 정상 왕복하는지만 확인한다.
    for (int i = 0; i < 3; ++i) {
        lock.lock();
        lock.unlock();
    }
}

void test_mcs_lock() {
    mcs_lock lock;

    mcs_lock::qnode n1;
    lock.lock(n1);
    lock.unlock(n1);

    // 두 번째 획득도 정상 왕복하는지(경합 없는 경로) 확인한다.
    mcs_lock::qnode n2;
    lock.lock(n2);
    lock.unlock(n2);
}

}  // namespace

int main() {
    test_result();
    test_optional();
    test_span();
    test_intrusive_list();
    test_spinlock();
    test_ticket_lock();
    test_mcs_lock();

    std::printf("%d/%d checks passed\n", g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
