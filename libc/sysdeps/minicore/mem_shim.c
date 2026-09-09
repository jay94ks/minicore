// libc/sysdeps/minicore/mem_shim.c — musl의 malloc/free/calloc/realloc을
// libmc의 mc_malloc/mc_free(sys_brk 위의 범프 할당자, ADR-180)로 연결하는
// 얇은 어댑터(docs/design/repo-layout.md가 이미 이 자리를 예약해 둔 것
// — "libc 내부 훅을 libmc의 mc_* 호출로 연결"). third_party/musl의
// src/string/strdup.c가 malloc()을 직접 부른다(ADR-022 — musl 원본은
// 무수정으로 유지하고, 이 파일처럼 minicore 쪽 어댑터로 필요한 심볼을
// 채운다).
#include <mc/heap.h>
#include <stddef.h>

void* malloc(size_t n) { return mc_malloc((uint64_t)n); }

void free(void* p) { mc_free(p); }

void* calloc(size_t nmemb, size_t size) {
    uint64_t total = (uint64_t)nmemb * (uint64_t)size;
    void* p = mc_malloc(total);
    if (p != NULL) {
        uint8_t* bytes = (uint8_t*)p;
        for (uint64_t i = 0; i < total; ++i) {
            bytes[i] = 0;
        }
    }
    return p;
}

// M26(general-purpose-completion.md §M26) — mc_malloc은 순수 범프
// 할당자라 할당 크기를 따로 기록하지 않는다(libmc/src/mem/heap.c) —
// 그래서 이 realloc은 "원본 내용을 보존한 채 자유롭게 이동"을 할 수
// 없다(원본이 몇 바이트였는지 알 방법이 없다). 이번 라운드의 유일한
// 소비자(strdup)는 malloc만 쓰고 realloc은 전혀 호출하지 않는다 —
// 그래도 심볼 자체는 있어야 musl의 다른 파일이 나중에 이걸 참조해도
// 링크가 깨지지 않으므로, "실패하지 않는" 최소 동작만 제공한다
// (알려진 단순화 — 실제 재할당이 필요해지면 mc_malloc 쪽에 크기
// 추적을 먼저 추가해야 한다).
void* realloc(void* ptr, size_t size) {
    (void)ptr;
    return mc_malloc((uint64_t)size);
}
