// userland/ls/main.c — docs/plan/musl-userland-porting.md §M52
// (ADR-221). 진짜 musl 프로그램이지만, 디렉터리 목록만은 musl의
// 표준 경로(readdir 등)가 아니라 libmc의 fs-protocol v4 클라이언트
// (mc_vfs_open/mc_fs_list)를 직접 부른다 — musl 프로그램도 이미
// minicore_libmc를 함께 링크하고 있어(freestanding_mem.c 제공용,
// M50) 이 함수들을 그대로 호출할 수 있다. 이 저장소에 새 Linux
// syscall(getdents64 등)을 하나 더 만드는 대신, 이미 있는
// userland/shell(ADR-170)의 빌트인 ls가 쓰는 것과 똑같은 요령
// (아무 파일이나 하나 열어 그 fs_handle로 OP_LIST를 부른다,
// memfs는 평평한 네임스페이스라 서브디렉터리 개념이 없다)을 그대로
// 재사용한다 — "/bin/echo"는 procsrv가 부팅 시 이미 VFS에 심어
// 둔다는 것이 보장돼 있어(servers/procsrv/main.cpp) 이 부트스트랩
// 대상으로 쓴다. GNU ls의 아주 좁은 부분집합(옵션 없음, 정렬 없음).
#include <mc/fs_client.h>
#include <mc/util.h>
#include <mc/vfs_client.h>
#include <unistd.h>

// servers/CMakeLists.txt의 --depends=ls:vfs 순서 그대로(다른 모든
// 서비스가 vfs를 받을 때와 같은 관례, handle 1은 own endpoint —
// 이 프로그램은 만들지 않으므로 실제로는 안 쓰지만 관례상 handle
// 2부터 시작).
#define MC_VFS_HANDLE 2

int main(void) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    if (mc_vfs_open(MC_VFS_HANDLE, "/bin/echo", 0, &open_file_id, &fs_handle) !=
        MC_FS_STATUS_OK) {
        write(1, "ls: vfs error\n", 14);
        return 1;
    }
    static uint8_t blob[4096];
    uint32_t count = 0;
    if (mc_fs_list(fs_handle, blob, sizeof(blob), &count) != MC_FS_STATUS_OK) {
        write(1, "ls: list error\n", 15);
        return 1;
    }
    uint64_t off = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = (const char*)&blob[off];
        uint64_t len = mc_cstr_len(name);
        write(1, name, len);
        write(1, "\n", 1);
        off += len + 1;
    }
    return 0;
}
