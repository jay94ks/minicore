#ifndef MINICORE_LIBS_LIBKENV_SHARED_PTR_H
#define MINICORE_LIBS_LIBKENV_SHARED_PTR_H

#include "libkenv/spinlock.h"
#include "libkenv/type_traits.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"

// libkenv: 커널용 lock-free 공유/약한/배타적 소유 포인터 템플릿
// (SP-201238BB, PN-68871BC9 착수 2번째 증분) - 설계자 지시("lock-free
// 기반으로 커널용 공유 포인터 템플릿들을 설계해봐")에 따라
// AsyncTaskWeakRef(async_task.h/.cpp, PN-D01B7D07)가 이미 실제로
// 검증해 둔 "대상 수명과 컨트롤 블록 수명을 분리"하는 패턴을 임의
// 타입 T로 일반화한다. §2-B(IntrusiveControlBlock 등 침습적 변형)는
// PN-68871BC9 자신이 "v1 적용 대상 미정이라 범위 밖"으로 명시해 둔
// 항목이라 이 파일에는 포함하지 않는다.
//
// **소멸 관례**: 이 커널은 placement new/실제 소멸자 호출을 쓰지
// 않고(chunked_list.h 등 기존 관례) 명시적 init()/destroy() 메서드
// 쌍을 쓴다 - kDestroyAndFree<T>의 기본 구현이 이 관례를 그대로
// 가정한다(T::destroy() 호출 후 슬랩 반납). 이 관례를 안 따르는 T가
// 있다면 kMakeShared/kMakeUnique에 커스텀 deleter를 넘기면 된다
// (템플릿 파라미터라 기본값을 오버라이드하는 게 자연스러운 확장점).

namespace kernel {

// kIsBaseOf(EnableSharedFromThis<T> 상속 여부 판정) - 원래 이 파일에
// 1회성으로 박아 뒀으나(PN-68871BC9 착수 2번째 증분), <type_traits>
// 부재가 이 프로젝트 전체의 제약이라는 사실이 확인되면서 다른 설계도
// 재사용할 수 있게 libkenv/type_traits.h로 이설했다(PN-3F2D88FF,
// 순수 코드 이동 - 동작 동일). 배경/구현 근거 문서 주석은 그 헤더
// 참고.

// 기본 삭제자 - 이 프로젝트의 init()/destroy() 명시적 초기화 관례를
// 따르는 T를 반납한다(placement new를 안 쓰므로 실제 소멸자 대신
// destroy()를 명시적으로 호출 - Process::destroy() 등 기존 관례와
// 동일). 이 관례를 안 따르는 T는 kMakeShared/kMakeUnique에 커스텀
// deleter를 넘겨 오버라이드한다.
template <typename T>
void kDestroyAndFree(T* ptr) {
    ptr->destroy();
    GenericSlabAllocator::free(ptr, sizeof(T));
}

// [SP-201238BB §2] `Deleter`를 템플릿 파라미터로 받는다(SharedPtr에도
// 커스텀 삭제자를 템플릿 파라미터화하라는 설계자 Opinion 반영 -
// ControlBlock/SharedPtr/WeakPtr/kMakeShared 전부 같은 Deleter로
// 맞춰야 한다). AsyncTaskWeakRef::init()과 동일한 관례로 raw 슬랩
// 메모리 위에 명시적으로 초기화한다(placement new 안 씀).
template <typename T, typename Deleter = void (*)(T*)>
class ControlBlock {
public:
    void init(T* target, Deleter deleter = &kDestroyAndFree<T>) {
        _target.store(target);
        _strongCount.store(1);  // 최초 SharedPtr 생성자 몫
        _weakCount.store(1);    // 강한 참조가 하나라도 있는 동안의 암묵적 몫(표준 shared_ptr 관례)
        _deleter = deleter;
    }

    // WeakPtr::lock()이 쓴다 - "0에서 다시 살아나지 않게" CAS 루프로
    // strongCount를 원자적으로 증가(0이면 실패, 이미 파괴된 대상을
    // 되살리지 않음 - AtomicU32::fetchAdd 단독으로는 이 보장이 안 됨).
    bool tryAddStrongRef() {
        uint32_t cur = _strongCount.load();
        while (cur != 0) {
            if (_strongCount.compareExchange(cur, cur + 1)) {
                return true;
            }
            // compareExchange 실패 시 cur가 최신값으로 갱신됨(관례,
            // spinlock.h의 기존 compareExchange 시그니처 그대로) - 재시도.
        }
        return false;  // 이미 0 - 대상이 파괴됨, 되살릴 수 없음
    }

    T* target() const { return _target.load(); }

    // SharedPtr 소멸/재대입 시 호출 - init()이 저장해 둔 _deleter를
    // 그대로 쓴다(파라미터로 매번 다시 받지 않음).
    void releaseStrong() {
        if (_strongCount.fetchSub(1) == 1) {
            T* t = _target.load();
            if (t) {
                _deleter(t);
            }
            _target.store(nullptr);
            releaseWeak();  // 강한 참조 전부 사라짐 - 암묵적 weak 1개도 해제
        }
    }

    void releaseWeak() {
        if (_weakCount.fetchSub(1) == 1) {
            GenericSlabAllocator::free(this, sizeof(ControlBlock<T, Deleter>));
        }
    }

    void addWeakRef() { _weakCount.fetchAdd(1); }

    // SharedPtr 복사 생성자 전용 - 호출자가 이미 강한 참조 1개를 쥔
    // 채로 부르므로(카운트가 0일 수 없음) tryAddStrongRef()의 CAS
    // 루프 없이 단순 fetchAdd로 충분하다.
    void addStrongRefUnchecked() { _strongCount.fetchAdd(1); }

private:
    AtomicPtr<T> _target;
    AtomicU32 _strongCount;
    AtomicU32 _weakCount;
    Deleter _deleter = &kDestroyAndFree<T>;  // init()이 실제 값으로 덮어씀
};

template <typename T, typename Deleter>
class WeakPtr;

// RAII 강한 참조 - 대상을 살아있게 유지한다. Deleter는 기본값을
// 쓰면 기존 코드와 동일하게 SharedPtr<T>로만 써도 된다(템플릿 기본
// 인자라 명시할 필요 없음) - 커스텀 삭제자를 쓸 때만 SharedPtr<T,
// MyDeleter>로 명시.
template <typename T, typename Deleter = void (*)(T*)>
class SharedPtr {
public:
    SharedPtr() = default;
    ~SharedPtr() { reset(); }

    SharedPtr(const SharedPtr& other) : _block(other._block) {
        if (_block) _block->addStrongRefUnchecked();
    }
    SharedPtr(SharedPtr&& other) noexcept : _block(other._block) { other._block = nullptr; }

    SharedPtr& operator=(const SharedPtr& other) {
        if (this != &other) {
            reset();
            _block = other._block;
            if (_block) _block->addStrongRefUnchecked();
        }
        return *this;
    }
    SharedPtr& operator=(SharedPtr&& other) noexcept {
        if (this != &other) {
            reset();
            _block = other._block;
            other._block = nullptr;
        }
        return *this;
    }

    T* get() const { return _block ? _block->target() : nullptr; }
    T* operator->() const { return get(); }
    T& operator*() const { return *get(); }
    explicit operator bool() const { return get() != nullptr; }

    void reset() {
        if (_block) {
            _block->releaseStrong();  // 컨트롤 블록이 자기 몫 삭제자를 이미 알고 있다
            _block = nullptr;
        }
    }

private:
    friend class WeakPtr<T, Deleter>;
    // [실측으로 발견한 초안의 버그] kMakeShared가 이 private 생성자를
    // 직접 불러 컨트롤 블록을 SharedPtr로 감싸는데, 이 friend 선언이
    // 없으면 "private 생성자 호출" 컴파일 에러가 난다 - 원안(SP-201238BB)
    // 에 이 선언이 누락돼 있었다(EnableSharedFromThis 쪽엔 있었는데
    // SharedPtr 자신엔 빠짐 - 실제 컴파일 전까지는 안 드러나는 종류의
    // 실수). EnableSharedFromThis<T>의 friend 선언과 같은 패턴.
    template <typename U, typename D>
    friend SharedPtr<U, D> kMakeShared(U*, D);
    explicit SharedPtr(ControlBlock<T, Deleter>* block) : _block(block) {}
    ControlBlock<T, Deleter>* _block = nullptr;
};

// 약한 참조 - AsyncTaskWeakRef와 동일한 역할이지만 임의 T에 대해.
// SharedPtr<T, Deleter>와 같은 Deleter로 맞춰야 같은 ControlBlock을
// 가리킬 수 있다(lock()이 그 타입의 SharedPtr을 반환).
template <typename T, typename Deleter = void (*)(T*)>
class WeakPtr {
public:
    WeakPtr() = default;
    explicit WeakPtr(const SharedPtr<T, Deleter>& shared) : _block(shared._block) {
        if (_block) _block->addWeakRef();
    }
    ~WeakPtr() { if (_block) _block->releaseWeak(); }

    WeakPtr(const WeakPtr& other) : _block(other._block) {
        if (_block) _block->addWeakRef();
    }
    WeakPtr(WeakPtr&& other) noexcept : _block(other._block) { other._block = nullptr; }
    WeakPtr& operator=(const WeakPtr& other) {
        if (this != &other) {
            if (_block) _block->releaseWeak();
            _block = other._block;
            if (_block) _block->addWeakRef();
        }
        return *this;
    }
    WeakPtr& operator=(WeakPtr&& other) noexcept {
        if (this != &other) {
            if (_block) _block->releaseWeak();
            _block = other._block;
            other._block = nullptr;
        }
        return *this;
    }

    // AsyncTaskWeakRef::lock()과 동일한 역할이지만, "대상을 살려
    // 두는" SharedPtr을 반환한다(표준 weak_ptr::lock() 관례) - 대상이
    // 이미 파괴됐으면(strongCount==0) 빈 SharedPtr.
    SharedPtr<T, Deleter> lock() const {
        if (_block && _block->tryAddStrongRef()) {
            return SharedPtr<T, Deleter>(_block);
        }
        return SharedPtr<T, Deleter>();
    }

private:
    ControlBlock<T, Deleter>* _block = nullptr;
};

// 표준 enable_shared_from_this와 같은 역할 - 어떤 타입 T가 이걸
// 상속해 두면, T의 멤버 함수 안에서 새 컨트롤 블록을 만들지 않고
// 원래 그 객체를 소유하고 있던 바로 그 컨트롤 블록을 가리키는
// SharedPtr<T>를 안전하게 재구성할 수 있다.
//
// **알려진 제약(Deleter 템플릿 파라미터화의 부작용)**: _weakThis는
// 기본 삭제자(WeakPtr<T> = WeakPtr<T, void(*)(T*)>) 타입으로
// 고정돼 있다 - kMakeShared<T>를 커스텀 삭제자 없이 부른 경우에만
// 채워진다. T가 EnableSharedFromThis<T>를 상속하면서 동시에 커스텀
// 삭제자로 kMakeShared<T>(ptr, myDeleter)를 부르면 타입 불일치로
// 컴파일 에러가 난다 - 지금 그런 조합이 필요한 실사용처가 없어
// 문서화만 해 둔다(SP-201238BB §2). 커스텀 삭제자가 필요한 타입에는
// EnableSharedFromThis를 쓰지 않는다.
template <typename T>
class EnableSharedFromThis {
protected:
    // 이미 SharedPtr로 관리되는 객체 안에서만 안전하다 - kMakeShared로
    // 만들어지지 않은 T(스택/정적 변수 등)에서 부르면 _weakThis가
    // 비어 있어 빈 SharedPtr을 반환한다(예외를 못 쓰는 이 프로젝트
    // 관례상 "실패를 나타내는 빈 값 반환"이 일관된 선택).
    SharedPtr<T> sharedFromThis() { return _weakThis.lock(); }

private:
    template <typename U, typename D>
    friend SharedPtr<U, D> kMakeShared(U*, D);
    WeakPtr<T> _weakThis;
};

// GenericSlabAllocator로 대상 T와 ControlBlock<T, Deleter>를 각각
// 따로 할당(AsyncTaskWeakRef와 동일하게 raw slab 메모리 위에 앉힘,
// placement new 없이 명시적 초기화). Deleter는 템플릿 파라미터라
// deleter 인자의 타입에서 컴파일러가 추론한다(생략하면 기본값
// &kDestroyAndFree<T>) - 반환 타입이 SharedPtr<T, Deleter>이므로
// 커스텀 삭제자를 넘기면 호출부가 받는 타입도 자동으로 그에 맞춰진다.
template <typename T, typename Deleter = void (*)(T*)>
SharedPtr<T, Deleter> kMakeShared(T* preConstructed, Deleter deleter = &kDestroyAndFree<T>) {
    void* mem = GenericSlabAllocator::alloc(sizeof(ControlBlock<T, Deleter>));
    if (!mem) return SharedPtr<T, Deleter>();  // 할당 실패 - 빈 SharedPtr(예외 없음, 이 프로젝트 관례)
    auto* block = reinterpret_cast<ControlBlock<T, Deleter>*>(mem);
    block->init(preConstructed, deleter);
    SharedPtr<T, Deleter> result(block);
    // T가 EnableSharedFromThis<T>를 상속하면 _weakThis를 채운다 -
    // if constexpr로 컴파일 타임 분기(런타임 비용 0, T가 상속 안
    // 했으면 이 분기 자체가 인스턴스화되지 않는다). kIsBaseOf는 이
    // 파일 위쪽의 freestanding 대체(<type_traits> 없음 - 위 주석 참고).
    if constexpr (kIsBaseOf<EnableSharedFromThis<T>, T>) {
        preConstructed->_weakThis = WeakPtr<T>(result);
    }
    return result;
}

// [SP-201238BB §2-A] 배타적 소유 - move-only, 원자 연산/컨트롤
// 블록 없음(zero-cost). Deleter를 템플릿 파라미터로 받는다(SharedPtr
// 과 다른 선택 - 표준 unique_ptr<T, Deleter>와 동일한 이유: 컨트롤
// 블록이 없어 타입 소거로 숨길 자리가 없고, 소유자가 정확히 하나뿐인
// 배타적 소유는 애초에 "다른 삭제자로 만들어진 UniquePtr과 같은
// 타입이어야 할" 이유도 없다).
template <typename T, typename Deleter = void (*)(T*)>
class UniquePtr {
public:
    UniquePtr() = default;
    explicit UniquePtr(T* ptr, Deleter deleter = &kDestroyAndFree<T>) : _ptr(ptr), _deleter(deleter) {}
    ~UniquePtr() { reset(); }

    // 배타적 소유 - 복사 금지(공유 소유권으로 새는 걸 컴파일 타임에 차단).
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    // 이동만 허용 - 소유권이 한 곳에서 다른 곳으로 옮겨갈 뿐, 원자
    // 연산이 전혀 필요 없다(같은 순간 소유자가 정확히 하나뿐이므로
    // 경합 자체가 성립하지 않는다). 삭제자도 함께 옮긴다.
    UniquePtr(UniquePtr&& other) noexcept : _ptr(other._ptr), _deleter(other._deleter) { other._ptr = nullptr; }
    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            reset();
            _ptr = other._ptr;
            _deleter = other._deleter;
            other._ptr = nullptr;
        }
        return *this;
    }

    T* get() const { return _ptr; }
    T* operator->() const { return _ptr; }
    T& operator*() const { return *_ptr; }
    explicit operator bool() const { return _ptr != nullptr; }

    // 소유권을 포기하고 raw 포인터로 반환 - 이후 정리 책임은 호출자.
    T* release() {
        T* p = _ptr;
        _ptr = nullptr;
        return p;
    }

    void reset(T* newPtr = nullptr) {
        if (_ptr) {
            _deleter(_ptr);
        }
        _ptr = newPtr;
    }

private:
    T* _ptr = nullptr;
    Deleter _deleter = &kDestroyAndFree<T>;
};

template <typename T>
UniquePtr<T> kMakeUnique(T* preConstructed) {
    return UniquePtr<T>(preConstructed);
}

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_SHARED_PTR_H
