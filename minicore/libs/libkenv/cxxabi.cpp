#include <stddef.h>

// libkenv: 전역 operator new/delete - 이 커널은 -nostdlib이라 표준
// 라이브러리가 이걸 제공하지 않는다. 이 커널은 `new`/`delete` 식으로
// 객체를 할당하지 않지만(GenericSlabAllocator::alloc/free를 직접
// 쓴다), **가상 소멸자를 가진 클래스**(예: kernel::AsyncTaskHandler)의
// 구체 타입을 하나라도 정의하면, 컴파일러가 그 vtable에 넣는
// "delete하는 소멸자"(Itanium ABI의 D0)가 항상 `operator delete`
// 심볼을 참조한다 - 실제로 `delete`를 호출하는 코드가 전혀 없어도
// 링크 타임에는 이 심볼이 존재해야 한다. 이 커널 코드베이스는 절대
// 폴리모픽 포인터에 `delete`를 쓰지 않으므로(그 대신 프레임워크가
// 소유물을 직접 GenericSlabAllocator::free로 반납) 아래 정의는
// 링크만 통과시키는 용도이고, 실제로 호출될 일은 없다.
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}

// [신규, 2026-09-17, PN-E2A114C1 실측 링크 중 발견] `__cxa_atexit` -
// 비trivial 소멸자를 가진 전역/정적 변수를 하나라도 정의하면(예:
// kmain.cpp의 `SharedPtr<Process> gInitProcess`), 컴파일러가 프로그램
// 종료/공유 라이브러리 언로드 시 그 소멸자를 불러 달라고 이 Itanium
// C++ ABI 함수에 등록하는 코드를 자동으로 끼워 넣는다(`__cxx_global_
// var_init`) - 실제로 그 등록을 써먹을 일이 있든 없든 링크 타임에는
// 이 심볼이 존재해야 한다. 이 커널은 "프로그램 종료"라는 개념 자체가
// 없어(전원이 꺼지거나 재부팅될 뿐, 정상적으로 return하는 kMain이
// 없음 - 항상 hlt 루프) 이 등록된 소멸자가 실행될 일이 영원히 없다 -
// `operator delete`와 정확히 같은 이유로, 링크만 통과시키는 순수
// no-op을 둔다(실제로 소멸자를 호출/기억할 필요가 없다 - "그 등록
// 자체를 그냥 잊어버리는" 게 이 환경에서 올바른 동작).
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

// 위 `__cxa_atexit`의 세 번째 인자(dso_handle)로 컴파일러가 이 심볼의
// 주소를 그대로 넘긴다 - 링커가 "어느 공유 객체가 등록했는지" 구분하는
// 용도인데, 이 커널엔 애초에 "공유 객체"라는 개념이 없다(단일 정적
// 링크 이미지) - 그래도 심볼 자체는 존재해야 링크가 통과한다.
extern "C" void* __dso_handle = nullptr;

// [신규, 2026-09-17, PN-B41D8C0E, SP-1DB13F61 실측 링크 중 발견]
// `__cxa_pure_virtual` - 추상 클래스(예: kernel::Waitable, 순수 가상
// `cancel()`)를 실제로 상속하는 구체 타입(WaitQueue)이 실제 생성자를
// 거쳐 vtable이 진짜로 링크에 포함되면(SP-1DB13F61의 kMakeSharedNew가
// 이 프로젝트에서 처음으로 그런 경로를 만든다), 컴파일러가 그 추상
// 클래스 자신의 vtable에도 "아직 오버라이드되지 않았다면 여기로
// 떨어진다"는 placeholder로 이 Itanium C++ ABI 심볼을 채워 넣는다 -
// 실제로 오버라이드되지 않은 순수 가상 함수를 호출하는 코드는 이
// 프로젝트에 있을 수 없으므로(있다면 그 자체가 심각한 버그) 정상
// 실행 중에는 절대 불릴 일이 없다. 그래도 링크 타임에는 이 심볼이
// 존재해야 하고, 위 operator delete/__cxa_atexit와 달리 "혹시라도
// 실제로 불렸다면 그건 메모리 손상 등 심각한 버그"라는 뜻이라 조용히
// 반환하는 대신 확실히 멈춰 세운다(디버거로 봤을 때 "설계상 불가능한
// 지점에서 멈췄다"는 걸 바로 알 수 있게).
extern "C" void __cxa_pure_virtual() {
    for (;;) {
    }
}
