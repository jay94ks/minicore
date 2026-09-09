// libmc/src/mem/heap.c — mc/heap.h 구현. mc/heap.h 상단 주석 참고 —
// sys_brk(kernel/include/uapi.hpp::brk_request, ADR-180) 위에 얹은
// 순수 범프 할당자다.
#include <mc/heap.h>
#include <mc/syscall.h>

#define MC_MALLOC_ALIGN 16u

// 매 malloc마다 정확히 필요한 만큼만 sys_brk하지 않는다 — syscall
// 왕복을 줄이기 위해 한 번에 이보다 크게 확보해 두고 다음 호출들이
// 그 여유를 나눠 쓴다(보정 없는 잠정치, YAGNI — mc/heap.h가 이미
// 명시한 것처럼 이 라운드의 목표는 malloc 왕복 자체의 증명이다).
#define MC_HEAP_MIN_GROW (4096u * 4u)

static uint64_t g_heap_cursor = 0;  // 다음 malloc이 내줄 위치.
static uint64_t g_heap_limit = 0;   // sys_brk로 이미 확보해 둔 끝.

void* mc_malloc(uint64_t size) {
    if (size == 0) {
        return 0;
    }
    uint64_t aligned = (size + (MC_MALLOC_ALIGN - 1)) & ~(uint64_t)(MC_MALLOC_ALIGN - 1);

    if (g_heap_cursor == 0) {
        // 최초 호출 — increment=0(조회)으로 이 프로세스의 heap_top을
        // 확정한다(커널이 지연 초기화한 값).
        mc_brk_request query;
        query.increment = 0;
        query.out_old_top = 0;
        if (mc_brk(&query) != 0) {
            return 0;
        }
        g_heap_cursor = query.out_old_top;
        g_heap_limit = query.out_old_top;
    }

    if (g_heap_cursor + aligned > g_heap_limit) {
        uint64_t grow = aligned;
        if (grow < MC_HEAP_MIN_GROW) {
            grow = MC_HEAP_MIN_GROW;
        }
        mc_brk_request req;
        req.increment = (int64_t)grow;
        req.out_old_top = 0;
        if (mc_brk(&req) != 0) {
            return 0;
        }
        g_heap_limit = req.out_old_top + grow;
    }

    void* result = (void*)(uintptr_t)g_heap_cursor;
    g_heap_cursor += aligned;
    return result;
}

void mc_free(void* ptr) {
    (void)ptr;
}
