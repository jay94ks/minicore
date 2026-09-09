// libmc/include/mc/heap.h — 최소 malloc/free (docs/plan/
// general-purpose-completion.md §M24, docs/design/kernel-memory.md
// ADR-180). sys_brk 위에 얹은 순수 범프(bump) 할당자 — free()는
// 회수하지 않는다(이번 라운드는 "malloc으로 버퍼를 할당해 쓰는
// 왕복"만 검증 목표, YAGNI). 실제 회수가 필요해지면(예: free-list)
// 이 두 함수의 시그니처는 그대로 두고 구현만 바꾸면 된다.
#pragma once

#include <stdint.h>

void* mc_malloc(uint64_t size);
void mc_free(void* ptr);
