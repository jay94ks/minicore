#ifndef MINICORE_LIBS_LIBKENV_COROUTINE_H
#define MINICORE_LIBS_LIBKENV_COROUTINE_H

// libkenv: C++20 코루틴을 freestanding 환경에서 쓰기 위한 최소
// <coroutine> 대체(SP-F682B889 §7, 설계자 지시, 2026-09-14 -
// "Freestanding 환경에서 C++20의 coroutine을 활용할 방안을 찾아서
// 설계 제안에 반영하라"). 표준 <coroutine> 헤더는 이 프로젝트의
// 베어메탈 타겟(x86_64-unknown-none-elf)에 C++ 표준 라이브러리
// (libstdc++/libc++)가 아예 없어 그대로 include할 수 없다(실측 확인,
// 2026-09-14 - "file not found").
//
// **네이밍 - 표준 이름을 유지하는 건 아래 두 개뿐이다**(설계자 지시,
// 2026-09-14 - "코루틴 관련 freestanding 구현은 이름이 달라도
// 참조되는데 문제가 없을거라, 최소한의 호환 네이밍만 유지하고 모두
// 자체 컨벤션으로 변경하라"):
// - `std::coroutine_traits` - 컴파일러가 코루틴 반환 타입의
//   promise_type을 찾을 때 이 정확한 이름(네임스페이스 std)으로
//   조회한다(언어 규칙 자체가 이 이름을 요구 - 바꿀 수 없음).
// - `std::coroutine_handle` - `get_return_object`/`await_suspend`
//   시그니처 등 컴파일러가 생성하는 코드 여러 곳에서 이 정확한
//   타입을 직접 참조한다(마찬가지로 언어/컴파일러 내부 구현이
//   하드코딩하는 이름 - clang/gcc 둘 다 동일).
// promise_type의 멤버 함수 이름(`get_return_object`/
// `initial_suspend`/`final_suspend`/`return_void`/`return_value`/
// `unhandled_exception`/`operator new`/`operator delete`)도
// 컴파일러가 정확한 철자로 찾으므로 이 역시 우리가 바꿀 수 있는
// "라이브러리 네이밍"이 아니라 언어 프로토콜이다 - 위 "표준 이름
// 유지" 대상이 아니라 애초에 선택의 여지가 없는 부분.
//
// 반면 `std::suspend_always`/`std::suspend_never`는 순수 편의
// 타입이다 - 컴파일러는 이 이름을 전혀 찾지 않고, `await_ready`/
// `await_suspend`/`await_resume` 세 메서드(awaiter 프로토콜)만
// 만족하면 이름이 무엇이든 동일하게 동작한다 - 그래서 이 프로젝트
// 컨벤션(RM-23F4B687, PascalCase)에 맞춰 `kernel::SuspendAlways`/
// `kernel::SuspendNever`로 새로 이름 붙였다.
//
// 실측(2026-09-14, clang 18.1.3, 이 프로젝트와 동일한 -ffreestanding
// -fno-exceptions -fno-rtti -mcmodel=kernel 플래그) - 이 헤더
// 하나로 정상 컴파일되고, 실제 코루틴 ramp/resume/destroy/cleanup
// 함수가 생성됨을 오브젝트 파일 심볼로 확인, 커널 부팅 중 실제
// suspend/resume도 검증(관계도 참고).
//
// promise_type이 operator new/delete를 직접 제공하면 코루틴 프레임
// 할당이 그쪽으로 간다 - 전역 operator new가 없는(-nostdlib) 이
// 커널에서는 **반드시** 모든 promise_type이 operator new/delete를
// 정의해야 한다(SP-D7013B26의 GenericSlabAllocator로 연결하는 것을
// 권장 - 코루틴 프레임도 결국 고정 크기에 가까운 객체).

namespace std {

template <typename Promise = void>
struct coroutine_handle;

template <>
struct coroutine_handle<void> {
    coroutine_handle() noexcept = default;
    coroutine_handle(decltype(nullptr)) noexcept {}

    static coroutine_handle from_address(void* addr) noexcept {
        coroutine_handle h;
        h._frame = addr;
        return h;
    }
    void* address() const noexcept { return _frame; }

    void operator()() const { resume(); }
    void resume() const { __builtin_coro_resume(_frame); }
    void destroy() const { __builtin_coro_destroy(_frame); }
    bool done() const { return __builtin_coro_done(_frame); }
    explicit operator bool() const noexcept { return _frame != nullptr; }

    void* _frame = nullptr;
};

template <typename Promise>
struct coroutine_handle {
    coroutine_handle() noexcept = default;
    coroutine_handle(decltype(nullptr)) noexcept {}

    static coroutine_handle from_address(void* addr) noexcept {
        coroutine_handle h;
        h._frame = addr;
        return h;
    }
    void* address() const noexcept { return _frame; }

    static coroutine_handle from_promise(Promise& p) noexcept {
        coroutine_handle h;
        h._frame = __builtin_coro_promise(&p, 0, true);
        return h;
    }

    operator coroutine_handle<>() const noexcept { return coroutine_handle<>::from_address(_frame); }

    void resume() const { __builtin_coro_resume(_frame); }
    void destroy() const { __builtin_coro_destroy(_frame); }
    bool done() const { return __builtin_coro_done(_frame); }
    explicit operator bool() const noexcept { return _frame != nullptr; }

    Promise& promise() const {
        return *reinterpret_cast<Promise*>(__builtin_coro_promise(_frame, 0, false));
    }

    void* _frame = nullptr;
};

template <typename Ret, typename... Args>
struct coroutine_traits {
    using promise_type = typename Ret::promise_type;
};

}  // namespace std

namespace kernel {

// std::suspend_always에 대응하는 이 프로젝트 이름(설계자 지시 -
// 컴파일러가 이름으로 찾지 않는 순수 편의 타입이라 자유롭게 개명).
struct SuspendAlways {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

// std::suspend_never에 대응.
struct SuspendNever {
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_COROUTINE_H
