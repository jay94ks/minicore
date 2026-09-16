#ifndef MINICORE_LIBS_LIBKENV_TYPE_TRAITS_H
#define MINICORE_LIBS_LIBKENV_TYPE_TRAITS_H

// libkenv: 표준 <type_traits> 대체(PN-3F2D88FF, SP-201238BB §2) - 이
// 프로젝트의 베어메탈 타겟(x86_64-unknown-none-elf)에는 <type_traits>
// 자체가 없다(실측 확인: clang++로 `#include <type_traits>`만 컴파일
// 시도해도 "file not found" - libkenv/coroutine.h가 <coroutine>에 대해
// 이미 남겨 둔 것과 같은 종류의 제약, RM-23F4B687에 "표준 헤더는 실제
// #include 확인 전까지 가정하지 않는다"로 기록됨). <type_traits> 부재는
// SharedPtr(PN-68871BC9)만의 문제가 아니라 프로젝트 전체 제약이라, 원래
// shared_ptr.h 안에 1회성으로 박아 뒀던 kIsBaseOf를 이 헤더로 이설해
// 다른 설계도 재사용할 수 있게 한다. 범위는 지금 당장 필요한 것
// (is_same/is_base_of)과 그 최소 기반 골격(IntegralConstant)으로
// 한정한다 - 쓰지 않을 trait을 미리 만들어 두지 않는다(RM-23F4B687 §4).
// 실제로 필요해지는 다른 표준 trait은 그때 같은 패턴(SFINAE 관용구로
// 자체 구현)으로 이 헤더에 추가한다.

namespace kernel {

// std::integral_constant 계열 최소 골격 - 다른 trait들이 결과값을
// 컴파일 타임 상수 타입으로 감싸는 공통 자리.
template <typename T, T Value>
struct IntegralConstant {
    static constexpr T value = Value;
    using ValueType = T;
};

using TrueType = IntegralConstant<bool, true>;
using FalseType = IntegralConstant<bool, false>;

// std::is_same 대체 - 두 타입이 정확히 같은 타입인지(cv-한정자/참조
// 차이도 다른 타입으로 취급, std::is_same과 동일한 의미).
template <typename A, typename B>
struct IsSame : FalseType {};

template <typename A>
struct IsSame<A, A> : TrueType {};

template <typename A, typename B>
constexpr bool kIsSame = IsSame<A, B>::value;

namespace detail {

// [freestanding 대체, 원래 PN-68871BC9의 shared_ptr.h에서 이설]
// `std::is_base_of`가 필요했으나 위 문서 주석의 이유로 표준 헤더를
// 쓸 수 없다 - C++11 이전부터 널리 쓰인 표준 SFINAE 관용구(Loki/Boost
// 등)를 그대로 옮겼다: "..." 오버로드는 언어 규칙상 다른 모든 표준
// 변환보다 항상 순위가 낮으므로, Derived*가 Base*로 실제로 암시적
// 변환 가능할 때만(=Derived가 Base의 public이고 명확한 파생 클래스일
// 때만) 첫 번째 오버로드가 선택된다. sizeof는 피연산자를 실행하지
// 않으므로 두 오버로드 모두 실제로 호출되지 않는다(선언만 필요, 정의
// 불필요).
// **주의(실측으로 발견한 초안의 버그, PN-68871BC9)**: 이 두 오버로드를
// 자유 함수 템플릿으로 두고 호출부에서 `kIsBaseOfProbe<Base>(...)`처럼
// 명시적 템플릿 인자를 주면, 그 호출은 "명시적 인자가 붙을 수 있는"
// 템플릿 오버로드 하나만 고려 대상에 넣고 비템플릿 "..." 오버로드는
// 애초에 후보에서 빠진다(명시적 템플릿 인자 목록은 비템플릿 함수에
// 적용할 수 없다는 언어 규칙) - 그러면 상속 관계가 아닐 때 대체 후보가
// 하나도 안 남아 컴파일 자체가 깨진다. 그래서 반드시 클래스 템플릿의
// 정적 멤버 함수 두 개로 만들어야 한다 - `Base`/`Derived`가 이미 클래스
// 인스턴스화 시점에 고정된 구체 타입이 되므로, `value`를 계산하는
// 호출은 명시적 템플릿 인자 없이 평범한 오버로드 해석만으로 두 후보
// (`test(const volatile Base*)`/`test(const volatile void*)`) 중
// 하나를 고른다. `Derived*`가 `Base*`로 실제 변환 가능하면(=Derived가
// Base의 public이고 명확한 파생 클래스) 표준이 "B*->A*는 B*->void*보다
// 우선"이라고 명시적으로 규정해 둔 tie-break 규칙에 따라 첫 번째가
// 선택된다 - 그 외에는 항상 두 번째로 떨어진다.
template <typename Base, typename Derived>
struct IsBaseOfImpl {
    static char test(const volatile Base*);
    static long test(const volatile void*);
    static constexpr bool value = sizeof(test(static_cast<Derived*>(nullptr))) == sizeof(char);
};

}  // namespace detail

template <typename Base, typename Derived>
struct IsBaseOf : IntegralConstant<bool, detail::IsBaseOfImpl<Base, Derived>::value> {};

template <typename Base, typename Derived>
constexpr bool kIsBaseOf = detail::IsBaseOfImpl<Base, Derived>::value;

// [신규, 2026-09-17, PN-B41D8C0E, SP-1DB13F61] `std::remove_reference`/
// `std::forward` 대체 - `<utility>`도 이 프로젝트의 freestanding 제약
// 대상이라(위 문서 주석과 동일한 이유) `kMakeSharedNew<T>(Args&&...)`
// (shared_ptr.h)의 완벽 전달(perfect forwarding)에 필요한 최소한만
// 옮겨 왔다 - 표준 구현과 동일한 관용구(참조 축소 규칙 그대로).
template <typename T>
struct RemoveReference {
    using Type = T;
};
template <typename T>
struct RemoveReference<T&> {
    using Type = T;
};
template <typename T>
struct RemoveReference<T&&> {
    using Type = T;
};

template <typename T>
constexpr T&& kForward(typename RemoveReference<T>::Type& arg) noexcept {
    return static_cast<T&&>(arg);
}
template <typename T>
constexpr T&& kForward(typename RemoveReference<T>::Type&& arg) noexcept {
    return static_cast<T&&>(arg);
}

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_TYPE_TRAITS_H
