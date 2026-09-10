// libc/sysdeps/minicore/malloc_shim.c — musl의 malloc/src/free.c가
// 요구하는 __libc_free(real-libc-syscall-layer.md §M30). third_party/
// musl/src/malloc/lite_malloc.c(§M30이 실제 malloc()으로 채택)는
// 순수 범프 할당자라 free를 아예 지원하지 않는다 — M24/M26의
// mc_free()/realloc()이 이미 받아들인 "알려진 단순화"와 같은
// 정신으로, 여기서도 free를 조용히 no-op으로 둔다(실제 회수는
// 이 라운드 범위 밖).
void __libc_free(void* p) {
    (void)p;
}
