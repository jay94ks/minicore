#ifndef MINICORE_LIBS_LIBKENV_SPINLOCK_H
#define MINICORE_LIBS_LIBKENV_SPINLOCK_H

#include "libkenv/types.h"

// libkenv: SMP AP 기동(PL-65C20380) 0단계로 확정된 최소 동시성
// 프리미티브(설계자 지시, QU-B97FDA44, 2026-09-14) - 코어가 2개
// 이상 실제로 돌기 전에 공유 자료구조(Serial, PageFrameAllocator의
// 노드별 buddy 리스트 등)를 보호할 수 있어야 한다. 표준 라이브러리
// <atomic>에 기대지 않고 컴파일러 내장 원자 빌트인만 쓴다 - 헤더
// 하나로 프리스탠딩 어디서든 그대로 쓸 수 있게.

namespace kernel {

// TAS(test-and-set) 기반 스핀락 - 재진입 불가(같은 코어가 이미 든
// 락을 다시 잡으려 하면 그대로 멈춘다, 데드락 방지는 호출부 책임).
class Spinlock {
public:
    void lock() {
        while (__atomic_test_and_set(&_locked, __ATOMIC_ACQUIRE)) {
            while (_locked) {
                asm volatile("pause");  // 버스 트래픽 줄이는 힌트(x86) - 다른 아키텍처에선 무해한 nop 취급
            }
        }
    }

    void unlock() {
        __atomic_clear(&_locked, __ATOMIC_RELEASE);
    }

private:
    uint8_t _locked = 0;
};

// lock()/unlock()을 스코프에 맞춰 자동으로 걸고 푸는 RAII 래퍼.
class SpinlockGuard {
public:
    explicit SpinlockGuard(Spinlock& lock) : _lock(lock) { _lock.lock(); }
    ~SpinlockGuard() { _lock.unlock(); }

    SpinlockGuard(const SpinlockGuard&) = delete;
    SpinlockGuard& operator=(const SpinlockGuard&) = delete;

private:
    Spinlock& _lock;
};

// [신규, 2026-09-27, DC-2CB9DDA0 방향(A), 설계자 지시] `Spinlock`은
// `cli`를 하지 않아 락을 쥔 코어가 인터럽트에 의해 임의로 오래
// 선점당하면(예: 우선순위가 높은 IPI 폭주) 락을 반납하지 못한 채
// 멈출 수 있고, 그 락이 여러 코어가 공유하는 전역 락이면 나머지
// 모든 코어가 죽은 락에서 캐스케이딩으로 함께 멈춘다(`DC-54D69BEE`
// "[보강]" 절이 처음 지목, `DC-2CB9DDA0`가 dbgdriver 60회 배치에서
// 실측 재확인 - `DelayedExecutionQueue::gLock`). 이 락은 보유 구간
// 동안 로컬 코어의 인터럽트 자체를 꺼서(pci.cpp의 `PciConfigAccessGuard`
// /scheduler.cpp의 `enqueue()`/`scheduleImmediate()`와 완전히 같은
// "진입 시점의 실제 RFLAGS를 저장했다가 그대로 복원"기법 재사용 -
// 이미 cli된 인터럽트 핸들러 도중 잡혀도 무조건 `sti`로 IF=1을
// 강제하지 않아 `PN-9326B06F`류의 재발을 피한다) 그 선점 경로 자체를
// 원천 차단한다 - 짧은 임계구역에만 쓸 것(길게 쥐면 그 코어의 인터럽트
// 응답성 자체가 그만큼 떨어진다).
class IrqSpinlock {
public:
    void lock() {
        uint64_t savedRflags;
        asm volatile("pushfq; pop %0; cli" : "=r"(savedRflags) : : "memory");
        while (__atomic_test_and_set(&_locked, __ATOMIC_ACQUIRE)) {
            while (_locked) {
                asm volatile("pause");
            }
        }
        _savedRflags = savedRflags;
    }

    void unlock() {
        const uint64_t savedRflags = _savedRflags;
        __atomic_clear(&_locked, __ATOMIC_RELEASE);
        asm volatile("push %0; popfq" : : "r"(savedRflags) : "memory", "cc");
    }

private:
    uint8_t _locked = 0;
    uint64_t _savedRflags = 0;
};

// lock()/unlock()을 스코프에 맞춰 자동으로 걸고 푸는 RAII 래퍼.
class IrqSpinlockGuard {
public:
    explicit IrqSpinlockGuard(IrqSpinlock& lock) : _lock(lock) { _lock.lock(); }
    ~IrqSpinlockGuard() { _lock.unlock(); }

    IrqSpinlockGuard(const IrqSpinlockGuard&) = delete;
    IrqSpinlockGuard& operator=(const IrqSpinlockGuard&) = delete;

private:
    IrqSpinlock& _lock;
};

// [SP-201238BB §0, PN-68871BC9 착수 1번째 증분] 원래 여기 각자 따로
// 있던 `AtomicU32`/`AtomicPtr<T>`를 하나의 템플릿으로 통합했다 -
// 설계자 지시("AtomicU32 이런 종류도 Atomic<T>로 일반화하는 것을
// 검토해봐")로 SP-201238BB의 SharedPtr/WeakPtr 참조 카운트가 내부적으로
// 쓸 원자 원시 연산을 하나로 모으면서, 기존 두 클래스가 각자 갖고
// 있던 연산 전부(합집합)를 그대로 옮겼다. **기존 호출부는 전혀 안
// 바뀐다** - `AtomicU32`/`AtomicPtr<T>`라는 이름과 시그니처를 아래
// 타입 별칭으로 그대로 유지하기 때문에(같은 헤더 경로에 그대로 둔
// 이유도 이것 - 새 헤더로 옮기면 include 하는 13개 파일을 전부
// 건드려야 해서, 이름만 별칭으로 유지하는 이 방식보다 위험이 크다).
//
// `T`가 정수(`uint32_t`)든 포인터(`T*`)든 멤버 함수 바디는 실제로
// **호출되는 것만** 인스턴스화된다는 C++ 템플릿의 지연 인스턴스화
// 규칙 덕분에, `fetchOr`/`fetchAnd`처럼 포인터엔 원래 의미가 없는
// 연산도 안전하게 같은 클래스에 둘 수 있다 - `AtomicPtr<T>`로 쓰는
// 코드가 그 연산들을 애초에 호출하지 않으므로 컴파일 자체가 걸릴
// 일이 없다(포인터에 `__atomic_fetch_or` 등을 실제로 호출하려고
// 시도하면 그때 비로소 컴파일 에러가 난다 - 방어가 필요 없는 이유).
template <typename T>
class Atomic {
public:
    Atomic() = default;
    explicit Atomic(T initial) : _value(initial) {}

    T load() const {
        return __atomic_load_n(&_value, __ATOMIC_ACQUIRE);
    }

    void store(T value) {
        __atomic_store_n(&_value, value, __ATOMIC_RELEASE);
    }

    // 원래 AtomicPtr<T> 전용이었으나(포인터 교체), 정수에도 동일하게
    // 의미가 통해 그대로 합집합에 포함시켰다.
    T exchange(T value) {
        return __atomic_exchange_n(&_value, value, __ATOMIC_ACQ_REL);
    }

    // weak CAS(스퓨리어스 실패 가능) - lock-free 알고리즘 관례대로
    // 루프 안에서 재시도하는 용도. 실패하면 expected가 현재 값으로
    // 갱신된다(표준 CAS 관례).
    bool compareExchange(T& expected, T desired) {
        return __atomic_compare_exchange_n(&_value, &expected, desired, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    // 원래 AtomicU32 전용 - lock-free 자료구조(스케줄러 코어별 큐 등,
    // PL-2D3184BC)의 기반.
    T fetchAdd(T delta) {
        return __atomic_fetch_add(&_value, delta, __ATOMIC_ACQ_REL);
    }

    // kernel::string(libkenv/string.h)의 참조 카운트 감소에 쓴다 -
    // 반환값은 감소 전 값(호출부가 "이번에 0으로 떨어졌는지"를
    // fetchSub(1)==1로 판정할 수 있게).
    T fetchSub(T delta) {
        return __atomic_fetch_sub(&_value, delta, __ATOMIC_ACQ_REL);
    }

    // 비트마스크 자료구조(PN-D132A1E9의 수신자별 Target Pending Mask
    // 등)가 락 없이 비트를 세우고/지우는 데 쓴다 - 반환값은 연산 전
    // 값(호출부가 "이번 호출로 실제로 바뀐 비트"를 알고 싶을 때 유용).
    T fetchOr(T bits) {
        return __atomic_fetch_or(&_value, bits, __ATOMIC_ACQ_REL);
    }

    T fetchAnd(T bits) {
        return __atomic_fetch_and(&_value, bits, __ATOMIC_ACQ_REL);
    }

private:
    T _value{};
};

// [SP-9F1DB1D8, QU-68D76FC4/QU-E847DB03] 읽기 다수/쓰기 희소 패턴
// 전용의 최소 스핀 기반 RW락 - 첫 소비자는 Scheduler의 gCurrentTask[]
// 크로스코어 접근 보호(scheduler.cpp). 쓰기 우선순위(anti-starvation)
// 없음 - 리더가 계속 몰리면 라이터가 무기한 대기할 수 있다. 첫
// 소비자는 라이터가 극히 드물고(스케줄러 디스패치 지점 몇 곳) 리더도
// 드물어(TLB 샷다운/강제 이관/로드밸런싱 진단 API) 실질적 스타베이션
// 위험이 없다고 판단해 생략했다(RM-23F4B687 §4 과설계 방지) - 이후
// 다른 소비자가 리더 폭주 패턴이면 그때 티켓 기반 등으로 재검토.
class RwSpinlock {
public:
    void lockRead() {
        for (;;) {
            uint32_t v = _state.load();
            if (v != kWriteLocked && _state.compareExchange(v, v + 1)) {
                return;
            }
            asm volatile("pause");
        }
    }

    void unlockRead() {
        _state.fetchSub(1);
    }

    void lockWrite() {
        uint32_t expected = 0;
        while (!_state.compareExchange(expected, kWriteLocked)) {
            expected = 0;
            asm volatile("pause");
        }
    }

    void unlockWrite() {
        _state.store(0);
    }

private:
    static constexpr uint32_t kWriteLocked = 0xFFFFFFFFu;
    Atomic<uint32_t> _state{0};
};

// lockRead()/unlockRead()를 스코프에 맞춰 자동으로 걸고 푸는 RAII 래퍼.
class RwSpinlockReadGuard {
public:
    explicit RwSpinlockReadGuard(RwSpinlock& lock) : _lock(lock) { _lock.lockRead(); }
    ~RwSpinlockReadGuard() { _lock.unlockRead(); }

    RwSpinlockReadGuard(const RwSpinlockReadGuard&) = delete;
    RwSpinlockReadGuard& operator=(const RwSpinlockReadGuard&) = delete;

private:
    RwSpinlock& _lock;
};

// lockWrite()/unlockWrite()를 스코프에 맞춰 자동으로 걸고 푸는 RAII 래퍼.
class RwSpinlockWriteGuard {
public:
    explicit RwSpinlockWriteGuard(RwSpinlock& lock) : _lock(lock) { _lock.lockWrite(); }
    ~RwSpinlockWriteGuard() { _lock.unlockWrite(); }

    RwSpinlockWriteGuard(const RwSpinlockWriteGuard&) = delete;
    RwSpinlockWriteGuard& operator=(const RwSpinlockWriteGuard&) = delete;

private:
    RwSpinlock& _lock;
};

// SMP 기동 동기화(예: "몇 개 AP가 떴는지")에 쓸 최소 원자 카운터 -
// 기존 이름 유지(위 통합 이전과 완전히 동일하게 계속 쓸 수 있음).
using AtomicU32 = Atomic<uint32_t>;

// [신규, 2026-09-17, PN-495C11B7, SP-B1E258D8 §5.2] RCU grace-period
// 시퀀스 번호(64비트 - 32비트로는 장기 가동 시 랩어라운드 여지가
// 있어 애초에 64비트로 확정, SP-B1E258D8 §5.2의 `RcuGraceperiodSeq
// = uint64_t` 그대로) 전용 - `AtomicU32`와 동일한 관례.
using AtomicU64 = Atomic<uint64_t>;

// lock-free 자료구조가 포인터를 원자적으로 교체할 때 쓴다(예: 큐의
// head/tail, 스택 top) - 기존 이름 유지. 초기값은 nullptr(Atomic<T*>
// 의 `T _value{}`가 포인터에 대해 그대로 nullptr).
template <typename T>
using AtomicPtr = Atomic<T*>;

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_SPINLOCK_H
