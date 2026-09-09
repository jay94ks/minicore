// userland/shell/main.c — 로그인 후 셸 (docs/plan/system-servers-bringup.md
// §M20, docs/design/foundations.md ADR-170, security-model.md ADR-171).
//
// 실제 서드파티 셸 포팅이 아니라 minicore 전용으로 새로 쓴 최소
// 대체 구현이다(ADR-049) — libc 없이 libmc(순수 C)만 링크한다.
// ls/cat은 별도 프로세스가 아니라 이 프로세스 안의 빌트인이다 —
// ADR-008이 이 프로젝트의 "가장 어려운 문제"로 지적한 fork
// 의미론(procsrv가 여러 서버에 흩어진 fd를 자식에게 상속시키는
// 문제)을 이번 라운드에서 실제로 풀지 않고 명시적으로 피한다.
//
// initrun이 다른 서비스와 똑같이 부팅 시 직접 스폰한다(depends=vfs,
// console,ps2 → handle 2/3/4). 자기 자신의 endpoint(handle 1)에서
// procsrv가 보내는 OP_START(ADR-171)를 기다린 뒤에야 프롬프트를
// 낸다 — 로그인 전에는 화면에 아무것도 보이지 않는다.
//
// 실제 키 입력이 QEMU 자동화 환경에 주입되지 않는 문제(ps2/login이
// 이미 겪은 것과 같은 제약)는 같은 해법을 재사용한다 — 첫 줄
// 읽기에서 키가 하나도 안 오면 내장 자체 테스트 명령(ls, cat
// test.txt)을 결정적으로 실행해 검증한다.
#include <mc/console_client.h>
#include <mc/fs_client.h>
#include <mc/heap.h>
#include <mc/ps2_client.h>
#include <mc/syscall.h>
#include <mc/util.h>
#include <mc/vfs_client.h>

// M26(general-purpose-completion.md §M26, foundations.md ADR-182) —
// third_party/musl(원본 소스 그대로, 재구현 아님)의 문자열 함수
// 부분집합. libc/CMakeLists.txt가 이 헤더가 선언하는 함수들의 실제
// 구현을 minicore_libc로 빌드해 이 실행파일에 링크한다.
#include <string.h>

// sys_process_spawn(create_endpoint=true)이 handle 1을 채운다
// (ADR-152). handle 2/3/4는 depends=vfs,console,ps2 나열 순서 그대로.
#define OWN_ENDPOINT_HANDLE 1u
#define VFS_HANDLE 2u
#define CONSOLE_HANDLE 3u
#define PS2_HANDLE 4u

#define OP_START 1u  // security-model.md ADR-171.

#define MAX_LINE_LEN 64u
#define MAX_CAT_LEN 512u
#define MAX_CONSECUTIVE_MISSES 10u

#define ASCII_BACKSPACE 0x08
#define ASCII_ENTER 0x0D

static void debug(const char* s) {
    mc_debug_log(s, mc_cstr_len(s));
}

typedef struct {
    uint32_t length;
    int got_any_key;
} read_line_result;

static read_line_result read_line(char* buf, uint32_t max_len) {
    read_line_result r;
    r.length = 0;
    r.got_any_key = 0;
    uint32_t misses = 0;
    while (misses < MAX_CONSECUTIVE_MISSES) {
        mc_ps2_key key = mc_ps2_read_key(PS2_HANDLE);
        if (!key.got_key) {
            ++misses;
            continue;
        }
        misses = 0;
        r.got_any_key = 1;
        if (key.ascii == ASCII_ENTER) {
            break;
        }
        if (key.ascii == ASCII_BACKSPACE) {
            if (r.length > 0) {
                --r.length;
                mc_console_print_char(CONSOLE_HANDLE, ASCII_BACKSPACE);
            }
            continue;
        }
        if (r.length + 1 < max_len) {
            buf[r.length++] = (char)key.ascii;
            mc_console_print_char(CONSOLE_HANDLE, (char)key.ascii);
        }
    }
    buf[r.length] = '\0';
    return r;
}

// 빌트인 ls — memfs에 이미 존재하는 파일 아무 것이나 하나 열어(그
// 응답의 handles[0]로) memfs 프록시 핸들을 얻은 뒤 OP_LIST를 부른다
// (fs-protocol.md v4). test.txt는 procsrv의 M13 자체 테스트가 이미
// 만들어 둔 것을 재사용한다.
static int builtin_ls(void) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    if (mc_vfs_open(VFS_HANDLE, "test.txt", 0, &open_file_id, &fs_handle) != MC_FS_STATUS_OK) {
        mc_console_print_str(CONSOLE_HANDLE, "ls: vfs error\n");
        return 0;
    }
    static uint8_t blob[4096];
    uint32_t count = 0;
    if (mc_fs_list(fs_handle, blob, sizeof(blob), &count) != MC_FS_STATUS_OK) {
        mc_console_print_str(CONSOLE_HANDLE, "ls: list error\n");
        return 0;
    }
    uint64_t off = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = (const char*)&blob[off];
        mc_console_print_str(CONSOLE_HANDLE, name);
        mc_console_print_str(CONSOLE_HANDLE, "\n");
        off += mc_cstr_len(name) + 1;
    }
    return 1;
}

// 빌트인 cat <path> — VFS로 열고 EOF까지 읽어 그대로 출력한다.
static int builtin_cat(const char* path, char* out_content, uint64_t out_cap, uint64_t* out_len) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    if (mc_vfs_open(VFS_HANDLE, path, 0, &open_file_id, &fs_handle) != MC_FS_STATUS_OK) {
        return 0;
    }
    *out_len = mc_fs_read_all(fs_handle, open_file_id, (uint8_t*)out_content, out_cap);
    return 1;
}

static void dispatch(const char* line) {
    uint32_t i = 0;
    while (line[i] != '\0' && line[i] != ' ') {
        ++i;
    }
    if (i == 2 && line[0] == 'l' && line[1] == 's') {
        builtin_ls();
        return;
    }
    if (i == 3 && line[0] == 'c' && line[1] == 'a' && line[2] == 't') {
        const char* arg = (line[i] == ' ') ? line + i + 1 : line + i;
        if (mc_cstr_len(arg) == 0) {
            mc_console_print_str(CONSOLE_HANDLE, "cat: 경로가 필요합니다\n");
            return;
        }
        char content[MAX_CAT_LEN];
        uint64_t len = 0;
        if (!builtin_cat(arg, content, sizeof(content) - 1, &len)) {
            mc_console_print_str(CONSOLE_HANDLE, "cat: 열 수 없습니다\n");
            return;
        }
        content[len] = '\0';
        mc_console_print_str(CONSOLE_HANDLE, content);
        mc_console_print_str(CONSOLE_HANDLE, "\n");
        return;
    }
    if (i == 4 && mc_bytes_equal(line, "exit", 4)) {
        return;  // 호출자(mc_main)가 line 자체로 종료를 판단한다.
    }
    if (mc_cstr_len(line) == 0) {
        return;
    }
    mc_console_print_str(CONSOLE_HANDLE, "알 수 없는 명령\n");
}

// 자동화 환경(키 입력이 전혀 오지 않음) 전용 — 결정적으로 ls/cat을
// 한 번씩 행사해 검증한다.
static void run_self_test(void) {
    debug("[shell] no keyboard input, running self-test commands\n");

    int ls_ok = builtin_ls();
    debug(ls_ok ? "[shell] ls ok=1\n" : "[shell] ls ok=0\n");

    // M24(general-purpose-completion.md §M24, ADR-180) — cat의 버퍼를
    // 스택 배열이 아니라 mc_malloc()(sys_brk 위의 최소 범프 할당자,
    // libmc/include/mc/heap.h)으로 얻어 실제로 쓴다 — 유저랜드
    // 동적 메모리 왕복 자체를 이 기존 경로 위에서 증명한다.
    char* content = (char*)mc_malloc(MAX_CAT_LEN);
    debug(content != 0 ? "[shell] malloc buffer ok=1\n" : "[shell] malloc buffer ok=0\n");

    uint64_t len = 0;
    int cat_opened =
        content != 0 && builtin_cat("test.txt", content, MAX_CAT_LEN - 1, &len);
    if (content != 0) {
        content[len] = '\0';
    }
    int cat_ok = cat_opened && mc_bytes_equal(content, "hello vfs", mc_cstr_len("hello vfs")) &&
                 len == mc_cstr_len("hello vfs");
    debug(cat_ok ? "[shell] cat ok=1\n" : "[shell] cat ok=0\n");

    // M26(general-purpose-completion.md §M26, foundations.md ADR-182) —
    // third_party/musl(원본 소스 그대로 빌드, minicore가 재구현한
    // mc_cstr_len/mc_bytes_equal이 아니다)의 strcpy/strcat/strlen/
    // memcmp/strdup이 서로 협력해 정확히 동작하는지 확인한다. strdup
    // 이 libc/sysdeps/minicore/mem_shim.c를 거쳐 mc_malloc(ADR-180)
    // 까지 왕복해야 성공한다.
    char libc_buf[32];
    strcpy(libc_buf, "hello");
    strcat(libc_buf, " ");
    strcat(libc_buf, "vfs");
    char* libc_dup = strdup(libc_buf);
    int libc_ok =
        libc_dup != 0 && strlen(libc_dup) == 9 && memcmp(libc_dup, "hello vfs", 9) == 0;
    debug(libc_ok ? "[shell] libc strcpy/strcat/strdup ok=1\n"
                   : "[shell] libc strcpy/strcat/strdup ok=0\n");

    debug("[shell] self-test done\n");
}

void mc_main(const void* argv_or_null) {
    (void)argv_or_null;

    // security-model.md ADR-171 §결정1~3 — procsrv가 로그인 성공 후
    // OP_START를 보내기 전까지는 아무것도 하지 않고 블록한다.
    mc_message in;
    mc_zero_bytes(&in, sizeof(in));
    mc_ipc_recv(OWN_ENDPOINT_HANDLE, &in);
    mc_message out;
    mc_zero_bytes(&out, sizeof(out));
    out.label = in.label;
    mc_ipc_reply(&out);
    debug("[shell] session started\n");

    mc_console_print_str(CONSOLE_HANDLE, "\nminicore$ ");
    char line[MAX_LINE_LEN];
    read_line_result first = read_line(line, sizeof(line));
    if (!first.got_any_key) {
        run_self_test();
        return;
    }
    mc_console_print_str(CONSOLE_HANDLE, "\n");

    for (;;) {
        if (mc_bytes_equal(line, "exit", 4) && line[4] == '\0') {
            break;
        }
        dispatch(line);
        mc_console_print_str(CONSOLE_HANDLE, "minicore$ ");
        read_line_result r = read_line(line, sizeof(line));
        mc_console_print_str(CONSOLE_HANDLE, "\n");
        if (!r.got_any_key) {
            break;
        }
    }
}
