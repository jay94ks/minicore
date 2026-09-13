#ifndef MINICORE_LIBS_LIBKENV_MEM_H
#define MINICORE_LIBS_LIBKENV_MEM_H

#include <stddef.h>

// libkenv: 프리스탠딩 환경엔 libc가 없지만, 컴파일러가 구조체 대입/
// 배열 초기화 등을 최적화하면서 memset/memcpy/memmove/memcmp 호출을
// 암묵적으로 만들어낼 수 있다 - 표준 이름 그대로 정의해야 그 호출들이
// 링크된다(우리 커널 네이밍 규칙의 예외 - 이 네 이름은 툴체인이
// 정하는 것이지 우리가 짓는 게 아니다).
extern "C" {

void* memset(void* dest, int value, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int memcmp(const void* lhs, const void* rhs, size_t count);

}  // extern "C"

#endif  // MINICORE_LIBS_LIBKENV_MEM_H
