#ifndef USERLAND_LIBS_LIBCENV_MEM_H
#define USERLAND_LIBS_LIBCENV_MEM_H

#include <stddef.h>

// minicore/libs/libkenv/mem.h(커널 쪽)와 완전히 같은 이유의 유저랜드
// 짝 - 이 프로젝트의 프리스탠딩 유저랜드 툴체인에는 <string.h>가 없어,
// 컴파일러가 구조체 대입/값초기화(예: `T t{};`, `*p = T{};`)를 위해
// 암묵적으로 호출하는 memcpy/memset이 링크 시점에 없어 실패한다
// (PN-185406F6 항목4, pubreg의 연결/등록 테이블 코드에서 처음 발견 -
// 그 전까지의 유저랜드 프로그램은 이 정도로 큰 구조체 대입이 없었다).
// v1은 커널 쪽과 동일하게 바이트 단위 단순 구현으로 정확성만 확보.
extern "C" {

void* memset(void* dest, int value, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int memcmp(const void* lhs, const void* rhs, size_t count);

}  // extern "C"

#endif  // USERLAND_LIBS_LIBCENV_MEM_H
