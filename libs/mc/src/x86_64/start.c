// libmc/src/x86_64/start.c — libmc를 링크하는 네이티브 앱의 진입점
// (docs/design/foundations.md ADR-132/170).
//
// ADR-132가 구상한 "argv를 파싱해 int main(argc, argv, boot_info)로
// 넘긴다"는 이번 라운드의 유일한 클라이언트(userland/shell)가 argv를
// 쓰지 않아 아직 하지 않는다 — servers/*가 이미 쓰는 최소 관례
// (`extern "C" [[noreturn]] void _start(const void* argv_or_null)`와
// 동일한 스펙)를 그대로 유지하고, 그 raw 포인터를 그대로 mc_main에
// 넘긴다. 실제 argc/argv 파싱이 필요해지면 이 파일에서 채운다.
#include <mc/syscall.h>

extern void mc_main(const void* argv_or_null);

void _start(const void* argv_or_null) {
    mc_main(argv_or_null);
    mc_thread_exit();
}
