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
