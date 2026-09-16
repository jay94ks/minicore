#ifndef MINICORE_LIBS_LIBKENV_SHARED_PTR_H
#define MINICORE_LIBS_LIBKENV_SHARED_PTR_H

#include "libkenv/spinlock.h"
#include "libkenv/type_traits.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"

// [신규, 2026-09-17, PN-B41D8C0E, SP-1DB13F61] 표준 배치(placement)
// `operator new` - 보통 `<new>`가 선언해 주지만 이 프로젝트의
// freestanding 타겟(x86_64-unknown-none-elf)엔 `<new>` 자체가 없다
// (실측 확인: `#include <new>`만 컴파일 시도해도 "file not found" -
// `<type_traits>`/`<coroutine>` 부재와 같은 종류의 제약, RM-23F4B687
// "표준 헤더는 실제 #include 확인 전까지 가정하지 않는다"). 이 선언이
// 없으면 아래 `kMakeSharedNew`의 `new (rawMem) T(...)` 구문 자체가
// 컴파일되지 않는다 - 실제로 메모리를 할당하지 않고 넘겨받은 포인터를
// 그대로 돌려주기만 하는(표준이 보장하는 배치 new의 정확한 의미)
// 자명한 구현이라 cxxabi.cpp의 `operator delete`/`__cxa_atexit`
// 스텁과 같은 성격 - 호출부(new 표현식)에서 보여야 하므로 여기
// 헤더에 inline으로 둔다(그 둘은 정의만 있으면 되는 링크 타임
// 심볼이라 .cpp에 둔 것과 차이).
inline void* operator new(kernel::uint64_t, void* ptr) noexcept {
    return ptr;
}

// libkenv: 커널용 lock-free 공유/약한/배타적 소유 포인터 템플릿
// (SP-201238BB, PN-68871BC9 착수 2번째 증분, PN-5FC484DF 2차 재설계) -
// 설계자 지시("lock-free 기반으로 커널용 공유 포인터 템플릿들을
// 설계해봐")에 따라 AsyncTaskWeakRef(async_task.h/.cpp, PN-D01B7D07)가
// 이미 실제로 검증해 둔 "대상 수명과 컨트롤 블록 수명을 분리"하는
// 패턴을 임의 타입 T로 일반화한다. §2-B(IntrusiveControlBlock 등
// 침습적 변형)는 PN-68871BC9 자신이 "v1 적용 대상 미정이라 범위 밖"
// 으로 명시해 둔 항목이라 이 파일에는 포함하지 않는다.
//
// **[2차 재설계, 2026-09-16, PN-5FC484DF, QU-4E449C65 답변("(B)
// SharedPtr 별칭 생성자 추가")]** 컨트롤 블록의 참조 카운팅 부분을
// 타입 소거된 `ControlBlockBase`로 분리했다(표준 std::shared_ptr의
// 실제 내부 구조와 동일한 접근) - Task::blockedOn(Waitable*)을
// WeakPtr<Waitable>로 바꾸려는데, 실제 Waitable 구현체(WaitQueue)가
// Mutex/Semaphore에 임베디드라 컨테이너의 컨트롤 블록을 공유하면서
// 멤버 하나만 가리키는 SharedPtr/WeakPtr(별칭, aliasing)이 필요해진
// 것이 계기. `SharedPtr<T,Deleter>`/`WeakPtr<T,Deleter>`는 이제
// `ControlBlockBase* _block`(타입 소거) + `T* _ptr`(이 핸들이 실제로
// 가리키는 대상) 두 필드를 든다 - 인스턴스 크기가 8바이트에서
// 16바이트(x86_64)로 늘었지만, `get()`이 `_block->target()` 간접
// 호출 없이 `_ptr`을 바로 반환해 오히려 한 단계 빨라진다. 공개 API
// (`get()`/`operator->`/`reset()`/`lock()` 등)는 리팩터 전과 동작이
// 완전히 동일해야 한다는 게 이 재설계의 제약이었다(PN-68871BC9
// 2번째 증분에서 이미 검증한 8개 항목 전부 재검증 필요).
//
// **소멸 관례**: 이 커널은 절대다수의 T에 대해 placement new/실제
// 소멸자 호출을 쓰지 않고(chunked_list.h 등 기존 관례) 명시적
// init()/destroy() 메서드 쌍을 쓴다 - kDestroyAndFree<T>의 기본
// 구현이 이 관례를 그대로 가정한다(T::destroy() 호출 후 슬랩 반납).
// 이 관례를 안 따르는 T가 있다면 kMakeShared/kMakeUnique에 커스텀
// deleter를 넘기면 된다(템플릿 파라미터라 기본값을 오버라이드하는
// 게 자연스러운 확장점).
//
// **[예외, 2026-09-17, PN-B41D8C0E, SP-1DB13F61] 가상 함수(vtable)가
// 있는 T는 이 관례를 따를 수 없다** - memset(0)만으로는 vtable
// 포인터가 설치되지 않아(Mutex/Semaphore 안의 WaitQueue가 최초
// 사례, QU-1D089097 실측 발견) 가상 호출이 즉시 크래시한다. 이런
// T는 위 kDestroyAndFree 기본 경로 대신 아래 kMakeSharedNew(실제
// placement new + ~T() 소멸)를 쓴다 - 두 관례가 공존하며, 어느 쪽을
// 쓸지는 T가 가상 함수를 갖는지로 결정된다(대부분의 T는 여전히
// 기존 관례 그대로).

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

// [신규, PN-5FC484DF] 참조 카운팅만 아는 타입 소거된 기반 클래스 -
// `T`를 몰라도 카운트 증감/CAS 루프는 그대로 할 수 있다. 실제
// 소멸/반납은 파생 클래스(`ControlBlock<T,Deleter>`)가 `init()`에서
// 심어 둔 함수 포인터 트램폴린을 통해서만 한다(가상 함수 대신 -
// `Deleter`와 같은 함수 포인터 타입 소거 패턴, vtable 오버헤드 없음).
// 이 분리 덕분에 `SharedPtr<T,Deleter>`가 `ControlBlockBase*`만
// 들면(=T를 몰라도) 되고, 실제로 가리키는 대상(`T* _ptr`)은 별도
// 필드로 독립시킬 수 있어 별칭(aliasing) 생성자가 가능해진다.
class ControlBlockBase {
public:
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

    // SharedPtr 복사 생성자 전용 - 호출자가 이미 강한 참조 1개를 쥔
    // 채로 부르므로(카운트가 0일 수 없음) tryAddStrongRef()의 CAS
    // 루프 없이 단순 fetchAdd로 충분하다.
    void addStrongRefUnchecked() { _strongCount.fetchAdd(1); }
    void addWeakRef() { _weakCount.fetchAdd(1); }

    void releaseStrong() {
        if (_strongCount.fetchSub(1) == 1) {
            _destroyOwned(this);  // 원래 소유 객체 소멸(파생 타입이 세팅한 트램폴린)
            releaseWeak();        // 강한 참조 전부 사라짐 - 암묵적 weak 1개도 해제
        }
    }
    void releaseWeak() {
        if (_weakCount.fetchSub(1) == 1) {
            _freeSelf(this);  // 컨트롤 블록 자신의 메모리 반납(파생 타입의 실제 크기를 아는 트램폴린)
        }
    }

protected:
    AtomicU32 _strongCount;
    AtomicU32 _weakCount;
    void (*_destroyOwned)(ControlBlockBase*) = nullptr;  // init()이 채움
    void (*_freeSelf)(ControlBlockBase*) = nullptr;      // init()이 채움
};

// [SP-201238BB §2] `Deleter`를 템플릿 파라미터로 받는다(SharedPtr에도
// 커스텀 삭제자를 템플릿 파라미터화하라는 설계자 Opinion 반영 -
// ControlBlock/SharedPtr/WeakPtr/kMakeShared 전부 같은 Deleter로
// 맞춰야 한다). AsyncTaskWeakRef::init()과 동일한 관례로 raw 슬랩
// 메모리 위에 명시적으로 초기화한다(placement new 안 씀). `_target`/
// `_deleter`만 그대로 갖는다 - 소멸 시 무엇을 해제할지는 컨트롤
// 블록이 계속 안다(별칭은 "누가 가리키는지"만 분리할 뿐 "누가
// 해제되는지"는 그대로 원래 소유 객체다).
template <typename T, typename Deleter = void (*)(T*)>
class ControlBlock : public ControlBlockBase {
public:
    void init(T* target, Deleter deleter = &kDestroyAndFree<T>) {
        _target.store(target);
        _strongCount.store(1);  // 최초 SharedPtr 생성자 몫
        _weakCount.store(1);    // 강한 참조가 하나라도 있는 동안의 암묵적 몫(표준 shared_ptr 관례)
        _deleter = deleter;
        _destroyOwned = &kDestroyOwnedTrampoline;
        _freeSelf = &kFreeSelfTrampoline;
    }

    // kMakeShared가 초기 SharedPtr::_ptr 값을 얻는 데만 쓴다 - 별칭
    // 생성자가 만든 SharedPtr은 이 값을 안 거치고 자기 _ptr을 직접 든다.
    T* target() const { return _target.load(); }

private:
    static void kDestroyOwnedTrampoline(ControlBlockBase* base) {
        auto* self = static_cast<ControlBlock<T, Deleter>*>(base);
        T* t = self->_target.load();
        if (t) {
            self->_deleter(t);
        }
        self->_target.store(nullptr);
    }
    static void kFreeSelfTrampoline(ControlBlockBase* base) {
        GenericSlabAllocator::free(base, sizeof(ControlBlock<T, Deleter>));
    }

    AtomicPtr<T> _target;
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

    SharedPtr(const SharedPtr& other) : _block(other._block), _ptr(other._ptr) {
        if (_block) _block->addStrongRefUnchecked();
    }
    SharedPtr(SharedPtr&& other) noexcept : _block(other._block), _ptr(other._ptr) {
        other._block = nullptr;
        other._ptr = nullptr;
    }

    SharedPtr& operator=(const SharedPtr& other) {
        if (this != &other) {
            reset();
            _block = other._block;
            _ptr = other._ptr;
            if (_block) _block->addStrongRefUnchecked();
        }
        return *this;
    }
    SharedPtr& operator=(SharedPtr&& other) noexcept {
        if (this != &other) {
            reset();
            _block = other._block;
            _ptr = other._ptr;
            other._block = nullptr;
            other._ptr = nullptr;
        }
        return *this;
    }

    // [신규, PN-5FC484DF, QU-4E449C65 답변] 별칭(aliasing) 생성자 -
    // owner의 컨트롤 블록을 공유(강한 참조 +1)하면서, 그 컨트롤
    // 블록이 실제로 소유한 타입(U)과 무관한 대상(T*, 대개 U의 멤버
    // 하나)을 가리키는 SharedPtr을 만든다. owner가 살아있는 동안은
    // aliasedPtr도 안전하다(표준 std::shared_ptr 별칭 생성자와 동일한
    // 계약 - 예: Mutex에 임베디드된 Waitable을 가리키면서 Mutex
    // 자신의 컨트롤 블록 참조 카운트를 공유).
    template <typename U, typename UDeleter>
    SharedPtr(const SharedPtr<U, UDeleter>& owner, T* aliasedPtr) : _block(owner._block), _ptr(aliasedPtr) {
        if (_block) _block->addStrongRefUnchecked();
    }

    T* get() const { return _ptr; }
    T* operator->() const { return _ptr; }
    T& operator*() const { return *_ptr; }
    explicit operator bool() const { return _ptr != nullptr; }

    void reset() {
        if (_block) {
            _block->releaseStrong();  // 컨트롤 블록이 자기 몫 삭제자를 이미 안다
            _block = nullptr;
        }
        _ptr = nullptr;
    }

private:
    // [신규, PN-5FC484DF] 별칭 생성자가 다른 SharedPtr<U,D>/
    // WeakPtr<U,D>의 _block을 읽을 수 있도록 - 타입이 다르면 서로
    // private 멤버에 접근 못 하는 C++ 기본 규칙을 이 템플릿 friend로
    // 풀어 준다(표준 shared_ptr 구현체들도 동일하게 이 패턴을 쓴다).
    template <typename U, typename D>
    friend class SharedPtr;
    template <typename U, typename D>
    friend class WeakPtr;
    // [버그 수정, 2026-09-16, PN-68871BC9 착수 2번째 증분 실측 컴파일 중
    // 발견] 이 friend 선언이 원안에 빠져 있었다 - kMakeShared가 이 아래
    // private 생성자를 직접 부르는데, EnableSharedFromThis<T> 쪽엔 같은
    // friend 선언이 있었으면서 SharedPtr 자신엔 없어 실제로 컴파일해
    // 보기 전까지 안 드러났다("private 생성자 호출" 컴파일 에러).
    template <typename U, typename D>
    friend SharedPtr<U, D> kMakeShared(U*, D);
    explicit SharedPtr(ControlBlockBase* block, T* ptr) : _block(block), _ptr(ptr) {}
    ControlBlockBase* _block = nullptr;
    T* _ptr = nullptr;  // [신규, PN-5FC484DF] 이 핸들이 실제로 가리키는 대상 - 별칭 지원을 위해 컨트롤 블록에서 분리
};

// 약한 참조 - AsyncTaskWeakRef와 동일한 역할이지만 임의 T에 대해.
// SharedPtr<T, Deleter>와 같은 Deleter로 맞춰야 같은 ControlBlock을
// 가리킬 수 있다(lock()이 그 타입의 SharedPtr을 반환).
template <typename T, typename Deleter = void (*)(T*)>
class WeakPtr {
public:
    WeakPtr() = default;
    explicit WeakPtr(const SharedPtr<T, Deleter>& shared) : _block(shared._block), _ptr(shared._ptr) {
        if (_block) _block->addWeakRef();
    }
    ~WeakPtr() { if (_block) _block->releaseWeak(); }

    WeakPtr(const WeakPtr& other) : _block(other._block), _ptr(other._ptr) {
        if (_block) _block->addWeakRef();
    }
    WeakPtr(WeakPtr&& other) noexcept : _block(other._block), _ptr(other._ptr) {
        other._block = nullptr;
        other._ptr = nullptr;
    }
    WeakPtr& operator=(const WeakPtr& other) {
        if (this != &other) {
            if (_block) _block->releaseWeak();
            _block = other._block;
            _ptr = other._ptr;
            if (_block) _block->addWeakRef();
        }
        return *this;
    }
    WeakPtr& operator=(WeakPtr&& other) noexcept {
        if (this != &other) {
            if (_block) _block->releaseWeak();
            _block = other._block;
            _ptr = other._ptr;
            other._block = nullptr;
            other._ptr = nullptr;
        }
        return *this;
    }

    // [신규, PN-5FC484DF, QU-4E449C65 답변] SharedPtr과 동일한 별칭
    // 생성자 - weak 쪽. Task::blockedOn처럼 "컨테이너가 살아있는 동안만
    // 그 임베디드 멤버를 관찰"하려는 용도가 바로 이 생성자를 쓴다.
    template <typename U, typename UDeleter>
    WeakPtr(const SharedPtr<U, UDeleter>& owner, T* aliasedPtr) : _block(owner._block), _ptr(aliasedPtr) {
        if (_block) _block->addWeakRef();
    }

    // AsyncTaskWeakRef::lock()과 동일한 역할이지만, "대상을 살려
    // 두는" SharedPtr을 반환한다(표준 weak_ptr::lock() 관례) - 대상이
    // 이미 파괴됐으면(strongCount==0) 빈 SharedPtr.
    SharedPtr<T, Deleter> lock() const {
        if (_block && _block->tryAddStrongRef()) {
            return SharedPtr<T, Deleter>(_block, _ptr);
        }
        return SharedPtr<T, Deleter>();
    }

private:
    template <typename U, typename D>
    friend class SharedPtr;
    template <typename U, typename D>
    friend class WeakPtr;
    ControlBlockBase* _block = nullptr;
    T* _ptr = nullptr;
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

    // [신규, 2026-09-17, PN-E2A114C1] `sharedFromThis()`의 WeakPtr
    // 버전(표준 C++17 `enable_shared_from_this::weak_from_this()`와
    // 같은 역할) - `this`를 강하게 붙잡지 않고 관찰만 하고 싶은
    // 멤버 함수용(예: `Process::execImage()`가 `UserThread::process`
    // 에 심을 값을 만들 때 - 그 필드 자체가 WeakPtr이므로 굳이
    // `sharedFromThis().lock()`을 거쳐 강한 참조를 잠깐 만들었다
    // 버릴 이유가 없다). `sharedFromThis()`와 동일한 전제(kMakeShared로
    // 만들어지지 않았으면 빈 WeakPtr).
    WeakPtr<T> weakFromThis() { return _weakThis; }

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
    // [버그 수정, 2026-09-16, PN-5FC484DF 실측 컴파일 중 발견] SP-201238BB
    // §2 원안의 이 줄은 `SharedPtr<T, Deleter> result(block);`(1개 인자)
    // 였다 - 재설계로 SharedPtr의 private 생성자가 `(ControlBlockBase*,
    // T*)` 2개 인자로 바뀌었는데 이 호출부만 갱신이 안 된 원안의
    // 오탈자였다(문서 자체가 리팩터 도중 스스로 낸 불일치 - 실제
    // 컴파일 전까지는 안 드러남, PN-68871BC9 2번째 증분의 friend 선언
    // 누락과 같은 종류의 발견).
    SharedPtr<T, Deleter> result(block, preConstructed);
    // T가 EnableSharedFromThis<T>를 상속하면 _weakThis를 채운다 -
    // if constexpr로 컴파일 타임 분기(런타임 비용 0, T가 상속 안
    // 했으면 이 분기 자체가 인스턴스화되지 않는다). kIsBaseOf는
    // libkenv/type_traits.h의 freestanding 대체(<type_traits> 없음).
    if constexpr (kIsBaseOf<EnableSharedFromThis<T>, T>) {
        preConstructed->_weakThis = WeakPtr<T>(result);
    }
    return result;
}

// [신규, 2026-09-17, PN-B41D8C0E, SP-1DB13F61 §3] `kDestroyAndFree<T>`
// (명시적 destroy()/init() 관례 전용)와 짝을 이루는 `kMakeSharedNew`
// 전용 삭제자 - `kMakeSharedNew`가 placement new로 만든 T는 실제
// 소멸자(`~T()`)로 반납해야 한다(생성자를 실제로 거쳤으므로 - RAII
// 하위 객체가 있다면 그 소멸자 체인도 여기서 자동으로 실행된다).
template <typename T>
void kDestroyCtorAndFree(T* ptr) {
    ptr->~T();
    GenericSlabAllocator::free(ptr, sizeof(T));
}

// [신규, 2026-09-17, PN-B41D8C0E, SP-1DB13F61] 가상 함수(vtable)가
// 있는(또는 그런 타입을 서브오브젝트로 임베디드하는) T 전용 대안
// 팩토리 - `kMakeShared(preConstructed, deleter)`의 기존 관례("이미
// memset(0)+init()으로 준비된 포인터를 받는다, 실제 생성자는 안 거침")
// 로는 T(또는 그 서브오브젝트)의 vtable 포인터가 영원히 설치되지
// 않는다(QU-1D089097 실측 발견 - Mutex/Semaphore 안의 WaitQueue가
// 최초 사례). `GenericSlabAllocator`에서 받은 원시 메모리 위에 실제
// placement new로 생성자를 돌린 뒤, 나머지(컨트롤 블록 초기화/
// `EnableSharedFromThis` 자동 배선)는 기존 `kMakeShared`에 그대로
// 위임한다 - 바뀌는 건 "T*를 어떻게 준비했는가"와 "소멸 시 무엇을
// 부르는가"(`~T()` vs `destroy()`) 두 가지뿐이다. T가 가상 함수가
// 없는 절대다수 타입(Process/RingBuffer/Task 등)이면 이 함수를 쓸
// 이유가 없다 - 그 타입들은 기존 `kMakeShared(preConstructed, deleter)`
// 경로를 그대로 쓴다(변경 없음).
template <typename T, typename... Args>
SharedPtr<T, void (*)(T*)> kMakeSharedNew(Args&&... args) {
    void* rawMem = GenericSlabAllocator::alloc(sizeof(T));
    if (!rawMem) return SharedPtr<T, void (*)(T*)>();  // 할당 실패 - 빈 SharedPtr(예외 없음, 이 프로젝트 관례)
    T* target = new (rawMem) T(kForward<Args>(args)...);
    return kMakeShared<T>(target, &kDestroyCtorAndFree<T>);
}

// [SP-201238BB §2-A] 배타적 소유 - move-only, 원자 연산/컨트롤
// 블록 없음(zero-cost). Deleter를 템플릿 파라미터로 받는다(SharedPtr
// 과 다른 선택 - 표준 unique_ptr<T, Deleter>와 동일한 이유: 컨트롤
// 블록이 없어 타입 소거로 숨길 자리가 없고, 소유자가 정확히 하나뿐인
// 배타적 소유는 애초에 "다른 삭제자로 만들어진 UniquePtr과 같은
// 타입이어야 할" 이유도 없다). PN-5FC484DF의 별칭/타입 소거 재설계는
// SharedPtr/WeakPtr 전용이다 - UniquePtr은 컨트롤 블록 자체가 없어
// 영향을 받지 않는다.
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
    // [신규, PN-21C2D4E9] 버퍼(예: RingBuffer::data)를 가리키는
    // UniquePtr<uint8_t>류 용도 - std::unique_ptr<T[]>처럼 별도
    // 배열 특수화를 두지 않고, 이 하나의 템플릿에 첨자 접근만
    // 더했다(포인터처럼 쓰는 기존 관례의 자연스러운 확장).
    T& operator[](uint64_t index) const { return _ptr[index]; }

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

    // [신규, PN-21C2D4E9 실측 중 발견] raw 슬랩 메모리 위에
    // reinterpret_cast로 앉혀진 뒤 실제 생성자를 거치지 않은 인스턴스
    // (예: RingBuffer가 BridgePipe 슬랩 메모리 위에 놓이는 기존 관례,
    // channel.h) 전용 - `operator=`/`reset()`과 달리 **기존 `_ptr`을
    // 절대 읽거나 해제하지 않는다**. 이 인스턴스가 정말 raw 메모리에서
    // 막 나왔다는 걸 호출부가 보장할 때만 안전하다(진짜 살아있는
    // 소유권을 덮어쓰면 그 옛 대상이 그대로 샌다) - ChunkedList::
    // ensureAllocator/AsyncTask::init()과 같은 "raw 메모리 재확립"
    // 관례의 UniquePtr 버전.
    void initRaw(T* ptr, Deleter deleter) {
        _ptr = ptr;
        _deleter = deleter;
    }

private:
    T* _ptr = nullptr;
    // [수정, 2026-09-16, PN-21C2D4E9 실측 컴파일 중 발견] 원래
    // `= &kDestroyAndFree<T>`였다 - 이 NSDMI는 기본 템플릿 인자
    // (`Deleter = void (*)(T*)`)를 그대로 쓰는 경우에만 유효한 값이라,
    // RingBufferDeleter처럼 함수 포인터로 변환 불가능한 **상태 있는
    // 커스텀 삭제자 타입**을 명시하면 이 대입식 자체가 컴파일 에러가
    // 난다(§2-A가 원래 약속한 "상태 있는 함수 객체도 Deleter로 가능"
    // 이 실제로는 막혀 있었던 셈 - 실제 그런 타입을 처음 써 보기
    // 전까지는 안 드러나는 종류의 결함, 이 세션에서 반복된 패턴과
    // 동일). 값 초기화(`{}`)로 바꾸면 함수 포인터 Deleter는 여전히
    // nullptr로, 상태 있는 구조체 Deleter는 그 타입의 기본 생성자로
    // 초기화된다 - 어느 쪽이든 `_ptr`이 nullptr인 기본 생성 상태에서만
    // 의미가 있는 값이라(살아있는 포인터가 있는 인스턴스는 항상 명시적
    // 생성자/initRaw가 `_deleter`도 함께 세팅) 동작 변화가 없다.
    Deleter _deleter{};
};

template <typename T>
UniquePtr<T> kMakeUnique(T* preConstructed) {
    return UniquePtr<T>(preConstructed);
}

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_SHARED_PTR_H
