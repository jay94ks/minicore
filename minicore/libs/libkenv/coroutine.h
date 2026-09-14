#ifndef MINICORE_LIBS_LIBKENV_COROUTINE_H
#define MINICORE_LIBS_LIBKENV_COROUTINE_H

// libkenv: C++20 코루틴을 freestanding 환경에서 쓰기 위한 최소
// <coroutine> 대체(SP-F682B889, 설계자 지시, 2026-09-14 - "Freestanding
// 환경에서 C++20의 coroutine을 활용할 방안을 찾아서 설계 제안에
// 반영하라"). 표준 <coroutine> 헤더는 이 프로젝트의 베어메탈 타겟
// (x86_64-unknown-none-elf)에 C++ 표준 라이브러리(libstdc++/libc++)가
// 아예 없어 그대로 include할 수 없다(실측 확인, 2026-09-14 - "file
// not found") - 하지만 컴파일러의 코루틴 변환(co_await/co_yield/
// co_return)이 실제로 필요로 하는 건 std::coroutine_handle/
// std::coroutine_traits/std::suspend_always/std::suspend_never라는
// "이름과 인터페이스"뿐이고, 별도 런타임 라이브러리 구현체는 필요
// 없다(전부 컴파일러가 생성하는 상태 머신 + 각 promise_type이 직접
// 제공) - 그래서 이 최소 대체 헤더 하나로 충분하다. 실측(2026-09-14,
// clang 18.1.3, 이 프로젝트와 동일한 -ffreestanding -fno-exceptions
// -fno-rtti -mcmodel=kernel 플래그) - 정상적으로 컴파일되고, 실제
// 코루틴 ramp/resume/destroy/cleanup 함수가 생성됨을 오브젝트 파일
// 심볼로 확인, 커널 부팅 중 실제 suspend/resume도 검증(관계도 참고).
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

struct suspend_always {
    bool await_ready() const noexcept { return false; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

struct suspend_never {
    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

template <typename Ret, typename... Args>
struct coroutine_traits {
    using promise_type = typename Ret::promise_type;
};

}  // namespace std

#endif  // MINICORE_LIBS_LIBKENV_COROUTINE_H
