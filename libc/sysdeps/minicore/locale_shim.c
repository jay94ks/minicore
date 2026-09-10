// libc/sysdeps/minicore/locale_shim.c — musl의 __map_file(원본
// third_party/musl/src/time/__map_file.c)의 단일 스레드/무-로케일
// 전용 대체(real-libc-syscall-layer.md §M35, ADR-188). 원본은 실제
// 로케일 아카이브 파일을 열어(open+fstat+mmap) 매핑하는데, 이
// 프로젝트의 musl 프로그램은 envp가 항상 비어 있어(M28)
// getenv("MUSL_LOCPATH")가 항상 NULL을 반환한다 — locale_map.c::
// __get_locale()이 이 함수를 부르는 유일한 경로(MUSL_LOCPATH 탐색
// 루프)가 실제로는 절대 실행되지 않는다. 그 경로를 실제로 타게
// 만들려면 open()의 fd(int)↔fs_handle 대응표(syscall_shim.c)와
// SYS_fstat(M31부터 항상 -ENOSYS)까지 다시 손대야 하는데, 실행되지
// 않을 코드를 위해 그 의존성을 끌어올 이유가 없다 — lock_shim.c/
// malloc_shim.c와 같은 이유의 같은 패턴.
#include <stddef.h>

const unsigned char* __map_file(const char* pathname, size_t* size) {
    (void)pathname;
    (void)size;
    return 0;
}
