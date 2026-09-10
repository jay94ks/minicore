// servers/cfgsrv/main.cpp — 설정 리포지터리(레지스트리) 서버
// (docs/plan/system-servers-bringup.md §M19, docs/spec/registry.md,
// registry-decisions.md ADR-060~064/169).
//
// VFS와 완전히 분리된 전용 IPC 프로토콜(registry.md §5)로 접근하는
// "@스키마/A/B/table" 형태의 계층적 타입-값 저장소다. registry.md의
// 프로토콜 9종(open/create/delete_table, list_children, get/set/
// delete_value, list_values, set_permissions)을 전부 구현한다.
//
// ADR-169가 사용자 확인을 거쳐 정한 이번 라운드의 범위:
//  1. group 권한 비트는 검증하지 않는다 — reg_permissions 구조체는
//     그대로 두되, 모든 테이블의 group_rwx는 0으로 두고 절대 매치
//     시키지 않는다(owner/other 두 경우만 실제로 행사).
//  2. 자신의 상태를 VFS 경로(/sys/etc/registry.dat, 기본 라우팅으로
//     memfs)에 실제로 쓰고 읽어 영속화한다. 단, 이 프로젝트엔 아직
//     쓰기 가능한 디스크 백엔드 FS 서버가 없다(fat32/ext4는 M16에서
//     읽기전용으로 범위가 좁혀졌고, memfs는 순수 인메모리라 재부팅을
//     넘어서는 진짜 내구성은 없다) — 그래도 "VFS로 실제 쓰고 다시
//     읽는" 코드 경로 자체는 진짜로 동작한다(로더 검증과 같은 정신,
//     ADR-167).
//  3. registry.md §5의 9개 오퍼레이션을 전부 구현한다.
//  4. open_table/create_table이 반환하는 "핸들"은 진짜 커널
//     object_kind::reg_table 객체가 아니라 cfgsrv 자신이 관리하는
//     프로토콜-레벨 정수다(servers/fs/memfs의 open_file_id와 같은
//     선례 — 살아있는 유저 프로세스가 런타임에 새 커널 객체를 만드는
//     syscall이 아직 없다).
#include <mc/syscall.h>

namespace kernsrv::cfgsrv {

namespace {

// sys_process_spawn(create_endpoint=true)가 handle 1을 이 프로세스의
// endpoint로 채운다(ADR-152). handle 2는 initrun이 스폰 시점에 넣어
// 준 vfs endpoint 프록시(lib/*.ini의 depends=vfs) — 자신의 상태를
// 영속화하려면 VFS가 있어야 한다.
constexpr uint32_t k_own_endpoint_handle = 1;
constexpr uint32_t k_vfs_handle = 2;

// fs-protocol.md v3 — VFS/memfs를 그대로 클라이언트로 호출한다
// (servers/procsrv/main.cpp의 vfs_open/write_elf_to_vfs/
// read_elf_from_vfs와 같은 패턴).
constexpr uint32_t k_fs_op_open = 1;
constexpr uint32_t k_fs_op_write = 2;
constexpr uint32_t k_fs_op_read = 3;
constexpr uint32_t k_fs_path_budget = 3 * sizeof(uint64_t);
constexpr uint64_t k_fs_status_ok = 0;
constexpr uint64_t k_page_size = 4096;
// 문자열 리터럴을 가리키는 전역 포인터 변수로 두면 ld.lld가
// .data.rel.ro에 그 포인터 값을 담을 별도의 작은 PT_LOAD 세그먼트를
// 만드는데, 그 세그먼트가 .rodata 세그먼트와 같은 4KiB 페이지를
// 공유하면 kernel/arch/x86_64/elf_loader.cpp가 세그먼트별로 독립적으로
// map_page를 호출해 같은 페이지를 두 번 매핑하려다 already_mapped로
// 실패한다(이 파일이 그 경계를 처음 넘겼다) — 함수로 두면 리터럴의
// 주소가 호출 지점에서 직접 계산돼(rip-relative lea) 저장되는 전역
// 포인터 자체가 생기지 않는다.
const char* k_persist_path() { return "/sys/etc/registry.dat"; }

// registry.md §5 — reg_op(label로 구분).
constexpr uint32_t k_op_open_table = 1;
constexpr uint32_t k_op_create_table = 2;
constexpr uint32_t k_op_delete_table = 3;
constexpr uint32_t k_op_list_children = 4;
constexpr uint32_t k_op_get_value = 5;
constexpr uint32_t k_op_set_value = 6;
constexpr uint32_t k_op_delete_value = 7;
constexpr uint32_t k_op_list_values = 8;
constexpr uint32_t k_op_set_permissions = 9;

// registry.md §5 — reg_error.
constexpr uint64_t k_err_ok = 0;
constexpr uint64_t k_err_not_found = 1;
constexpr uint64_t k_err_permission_denied = 2;
constexpr uint64_t k_err_already_exists = 3;
constexpr uint64_t k_err_invalid_path = 4;

// registry.md §2 — reg_value_type.
constexpr uint8_t k_type_string = 0;
constexpr uint8_t k_type_int64 = 1;
constexpr uint8_t k_type_boolean = 2;
constexpr uint8_t k_type_binary = 3;

constexpr uint32_t k_max_tables = 8;
constexpr uint32_t k_max_values_per_table = 8;
constexpr uint32_t k_max_path_len = 96;
constexpr uint32_t k_max_key_len = 24;
// M41(user-service-manager.md §M41) 실행 중 발견 — 256바이트로는
// docs/design/boot-and-drivers.md ADR-196 §결정2가 정한
// mc_svcmgr_service_unit(name[32]+exec_path[256]+args[192]+...,
// 약 616바이트) 하나도 못 담아 svcmgr의 get_value가 매번 잘린 값을
// 돌려줬다(길이가 안 맞아 호출자가 실패로 인식) — 원래 256이던 값을
// 4배로 올렸다. k_max_values_per_table(8)×k_max_tables(8) 기준
// 최대 64KiB 정적 배열이라(1024*8*8) 이 정도 여유는 비용이 없다.
constexpr uint32_t k_max_value_len = 1024;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

void pack_bytes(void* dst, uint64_t dst_bytes, const char* data, uint64_t len) {
    auto* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < dst_bytes; ++i) {
        d[i] = (i < len) ? static_cast<uint8_t>(data[i]) : 0;
    }
}

void debug_log(const char* msg) {
    do_syscall(MC_SYSCALL_DEBUG_LOG, reinterpret_cast<uint64_t>(msg), cstr_len(msg), 0);
}

// 가변 길이 __builtin_memset/memcpy는 이 freestanding 빌드에서 실제
// libc 심볼 호출로 낮춰져 링크에 실패한다(servers/vfs/main.cpp와 같은
// 이유) — message.regs[1]에 실린 8바이트 사용자명을 손으로 바이트
// 단위 추출한다(x86_64는 LE라 pack_bytes가 쓴 순서와 정확히 대응).
void unpack_username_reg(char* dst9, uint64_t reg) {
    for (int i = 0; i < 8; ++i) {
        dst9[i] = static_cast<char>((reg >> (8 * i)) & 0xFF);
    }
    dst9[8] = '\0';
}

void zero_bytes(void* dst, uint64_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < n; ++i) {
        d[i] = 0;
    }
}

// ---------- kv/테이블 저장소 ----------

struct kv_entry {
    bool used = false;
    char key[k_max_key_len] = {};
    uint8_t type = 0;
    uint32_t len = 0;
    uint8_t data[k_max_value_len] = {};
};

struct table_entry {
    bool used = false;
    char path[k_max_path_len] = {};  // 정규화된 "schema/A/B/table" (선행 '@' 없음).
    uint32_t owner_uid = 0;
    uint32_t group_gid = 0;   // ADR-169 §결정1 — 저장만 하고 검증엔 안 쓴다.
    uint8_t owner_rwx = 0;
    uint8_t group_rwx = 0;    // 항상 0(§결정1) — 매치 대상에서 제외.
    uint8_t other_rwx = 0;
    uint8_t special_bits = 0;
    kv_entry values[k_max_values_per_table];
};

table_entry g_tables[k_max_tables];

// 구조체를 통째로 `= table_entry{}`/`= kv_entry{}`로 재대입하면
// 컴파일러가 memset+memcpy 호출로 낮춰 링크에 실패한다(위 주석과
// 같은 이유) — 필드별로 손으로 초기화한다.
void reset_kv(kv_entry& e) {
    e.used = false;
    zero_bytes(e.key, k_max_key_len);
    e.type = 0;
    e.len = 0;
    zero_bytes(e.data, k_max_value_len);
}

void reset_table(table_entry& t) {
    t.used = false;
    zero_bytes(t.path, k_max_path_len);
    t.owner_uid = 0;
    t.group_gid = 0;
    t.owner_rwx = 0;
    t.group_rwx = 0;
    t.other_rwx = 0;
    t.special_bits = 0;
    for (uint32_t i = 0; i < k_max_values_per_table; ++i) {
        reset_kv(t.values[i]);
    }
}

bool path_equals(const char* a, const char* b) {
    for (uint32_t i = 0; i < k_max_path_len; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
    return true;
}

// table_id는 1부터 시작하는 g_tables 인덱스+1이다(0은 항상 무효 —
// kernel handle 관례와 같은 정신, ADR-169 §결정4).
table_entry* table_by_id(uint64_t table_id) {
    if (table_id == 0 || table_id > k_max_tables) {
        return nullptr;
    }
    table_entry& t = g_tables[table_id - 1];
    return t.used ? &t : nullptr;
}

table_entry* find_table_by_path(const char* path) {
    for (uint32_t i = 0; i < k_max_tables; ++i) {
        if (g_tables[i].used && path_equals(g_tables[i].path, path)) {
            return &g_tables[i];
        }
    }
    return nullptr;
}

uint64_t table_id_of(const table_entry* t) {
    return static_cast<uint64_t>(t - g_tables) + 1;
}

// registry.md §1 — "@스키마/A/B/table" 또는 "A/B/table"(스키마 생략
// 시 caller_username이 스키마가 된다)을 "schema/rest"로 정규화한다.
// rest가 비어 있으면(스키마만 있고 leaf가 없음) invalid.
bool normalize_path(const char* raw, const char* caller_username, char* out, uint32_t out_cap) {
    const char* schema = caller_username;
    uint64_t schema_len = cstr_len(caller_username);
    const char* rest = raw;
    if (raw[0] == '@') {
        schema = raw + 1;
        uint64_t i = 0;
        while (schema[i] != '\0' && schema[i] != '/') {
            ++i;
        }
        schema_len = i;
        rest = (schema[i] == '/') ? schema + i + 1 : schema + i;
    }
    uint64_t rest_len = cstr_len(rest);
    if (schema_len == 0 || rest_len == 0) {
        return false;
    }
    if (schema_len + 1 + rest_len + 1 > out_cap) {
        return false;
    }
    uint32_t o = 0;
    for (uint64_t i = 0; i < schema_len; ++i) {
        out[o++] = schema[i];
    }
    out[o++] = '/';
    for (uint64_t i = 0; i < rest_len; ++i) {
        out[o++] = rest[i];
    }
    out[o] = '\0';
    return true;
}

// caller_username == 이 테이블 경로의 스키마 세그먼트인가(자기
// 스키마에서의 create_table 허용 판정용).
bool schema_matches(const char* normalized_path, const char* caller_username) {
    uint64_t ulen = cstr_len(caller_username);
    for (uint64_t i = 0; i < ulen; ++i) {
        if (normalized_path[i] != caller_username[i]) {
            return false;
        }
    }
    return normalized_path[ulen] == '/';
}

// registry.md §3 — R/W/X 비트(4/2/1). X는 테이블에서 "모든 CRUD"라
// R/W 판정에도 포함시킨다. group은 ADR-169 §결정1로 제외.
uint8_t effective_rwx(const table_entry& t, uint32_t caller_uid) {
    return (caller_uid == t.owner_uid) ? t.owner_rwx : t.other_rwx;
}
bool can_read(const table_entry& t, uint32_t caller_uid) {
    if (caller_uid == 0) {
        return true;  // security-model.md ADR-075 — ROOT 전권.
    }
    uint8_t rwx = effective_rwx(t, caller_uid);
    return (rwx & 0b100) != 0 || (rwx & 0b001) != 0;
}
bool can_write(const table_entry& t, uint32_t caller_uid) {
    if (caller_uid == 0) {
        return true;
    }
    uint8_t rwx = effective_rwx(t, caller_uid);
    return (rwx & 0b010) != 0 || (rwx & 0b001) != 0;
}
bool can_all(const table_entry& t, uint32_t caller_uid) {
    if (caller_uid == 0) {
        return true;
    }
    return (effective_rwx(t, caller_uid) & 0b001) != 0;
}
bool any_access(const table_entry& t, uint32_t caller_uid) {
    if (caller_uid == 0) {
        return true;
    }
    return effective_rwx(t, caller_uid) != 0;
}

// ---------- VFS 클라이언트(영속화 전용, servers/procsrv/main.cpp와 같은 패턴) ----------

alignas(k_page_size) uint8_t g_io_scratch[k_page_size] = {};
// M41(user-service-manager.md §M41) 실행 중 발견 — k_max_value_len을
// 256에서 1024로 올린 뒤(위 주석 참고) 8개 테이블×8개 값이 전부
// 거의 최대 크기면 이론상 66KiB 가까이 필요한데, 이 버퍼는 여전히
// 8KiB(2*k_page_size)뿐이었다 — `persist_save()`의 `w_bytes` 등이
// 경계 검사 없이 그냥 계속 쓰기만 해서, 실제로 겪은 대로 조용히
// 이 버퍼 뒤의 다른 정적 변수를 덮어쓸 수 있었다(진짜 메모리
// 손상 버그, "[shell] cat ok=0" 회귀로 처음 드러남 — 원인을 여기까지
// 추적). 8배(32KiB)로 늘리고, persist_save() 자신에도 실제로 쓰기
// 전에 전체 필요 크기를 먼저 계산해 넘치면 아예 쓰지 않고 실패
// 처리하는 방어 코드를 추가했다(아래 참고) — 버퍼를 키우는 것만으로는
// "더 큰 상황에서 또 넘칠 수 있다"는 근본 문제를 안 없앤다.
alignas(k_page_size) uint8_t g_persist_buf[8 * k_page_size] = {};

void vfs_open(const char* path, uint64_t& out_open_file_id, uint32_t& out_fs_handle) {
    mc_message req{};
    req.label = k_fs_op_open;
    pack_bytes(req.regs, k_fs_path_budget, path, cstr_len(path));
    req.regs[3] = 0;  // cfgsrv 자신은 guest/jail이 아니다(fs-protocol.md v3 §2.1).
    mc_message reply{};
    do_syscall(MC_SYSCALL_IPC_CALL, k_vfs_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    if (reply.regs[1] != k_fs_status_ok || reply.handle_count != 1) {
        out_fs_handle = 0;
        return;
    }
    out_open_file_id = reply.regs[0];
    out_fs_handle = reply.handles[0].src_handle;
}

bool persist_write_all(const uint8_t* data, uint64_t size) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    vfs_open(k_persist_path(), open_file_id, fs_handle);
    if (fs_handle == 0) {
        return false;
    }
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t chunk = size - offset;
        if (chunk > k_page_size) {
            chunk = k_page_size;
        }
        for (uint64_t i = 0; i < k_page_size; ++i) {
            g_io_scratch[i] = (i < chunk) ? data[offset + i] : 0;
        }
        mc_message req{};
        req.label = k_fs_op_write;
        req.regs[0] = open_file_id;
        req.regs[1] = chunk;
        req.page_count = 1;
        req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_io_scratch);
        req.pages[0].length = k_page_size;
        req.pages[0].mode = MC_TRANSFER_COPY;
        mc_message reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, fs_handle, reinterpret_cast<uint64_t>(&req),
                   reinterpret_cast<uint64_t>(&reply));
        if (reply.regs[1] != k_fs_status_ok || reply.regs[0] != chunk) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

// EOF까지 순차 읽어 out_buf에 담는다. 반환값은 실제로 읽은 바이트 수
// (파일이 없으면 vfs_open이 실패해 0을 반환 — 최초 부팅에는 항상
// 이 경로다).
uint64_t persist_read_all(uint8_t* out_buf, uint64_t max_len) {
    uint64_t open_file_id = 0;
    uint32_t fs_handle = 0;
    vfs_open(k_persist_path(), open_file_id, fs_handle);
    if (fs_handle == 0) {
        return 0;
    }
    uint64_t total = 0;
    for (;;) {
        mc_message req{};
        req.label = k_fs_op_read;
        req.regs[0] = open_file_id;
        req.regs[1] = k_page_size;
        mc_message reply{};
        do_syscall(MC_SYSCALL_IPC_CALL, fs_handle, reinterpret_cast<uint64_t>(&req),
                   reinterpret_cast<uint64_t>(&reply));
        if (reply.regs[1] != k_fs_status_ok || reply.page_count != 1) {
            break;
        }
        uint64_t n = reply.regs[0];
        if (n == 0) {
            break;
        }
        if (total + n > max_len) {
            n = max_len - total;
        }
        const auto* src = reinterpret_cast<const uint8_t*>(reply.pages[0].vaddr);
        for (uint64_t i = 0; i < n; ++i) {
            out_buf[total + i] = src[i];
        }
        total += n;
        if (n < k_page_size || total >= max_len) {
            break;
        }
    }
    return total;
}

// ---------- 직렬화(registry.md §7 M19 절 — cfgsrv 내부 전용 포맷) ----------

constexpr uint32_t k_persist_magic = 0x52454731;  // "REG1".
constexpr uint32_t k_persist_version = 1;

void w_u8(uint8_t*& p, uint8_t v) { *p++ = v; }
void w_u16(uint8_t*& p, uint16_t v) {
    *p++ = static_cast<uint8_t>(v & 0xFF);
    *p++ = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void w_u32(uint8_t*& p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        *p++ = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}
void w_u64(uint8_t*& p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        *p++ = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}
void w_bytes(uint8_t*& p, const void* src, uint64_t n) {
    const auto* s = static_cast<const uint8_t*>(src);
    for (uint64_t i = 0; i < n; ++i) {
        *p++ = s[i];
    }
}
uint8_t r_u8(const uint8_t*& p) { return *p++; }
uint16_t r_u16(const uint8_t*& p) {
    uint16_t v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
    p += 2;
    return v;
}
uint32_t r_u32(const uint8_t*& p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(p[i]) << (8 * i);
    }
    p += 4;
    return v;
}
uint64_t r_u64(const uint8_t*& p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    p += 8;
    return v;
}
void r_bytes(const uint8_t*& p, void* dst, uint64_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < n; ++i) {
        d[i] = *p++;
    }
    (void)d;
}

// 전체 테이블 상태를 g_persist_buf에 직렬화해 VFS로 쓴다. 실패해도
// 프로토콜 응답 자체는 계속 진행한다(영속화는 내부 최선노력 —
// registry.md §7이 프로토콜 사용자에게 비가시라고 정한 그대로).
void persist_save() {
    // M41 실행 중 발견(g_persist_buf 주석 참고) — 실제로 쓰기
    // **전에** 필요한 전체 크기를 먼저 계산해, 버퍼를 넘기면 아예
    // 아무것도 쓰지 않고 실패 처리한다(w_bytes 등에 경계 검사를
    // 넣는 대신 이 방법을 골랐다 — 그 함수들은 다른 자리에서도
    // "이미 공간이 있다고 보장된 채" 쓰는 저수준 유틸이라, 경계
    // 검사를 여기저기 흩어 두는 대신 호출 전에 한 번 계산해 두는
    // 쪽이 더 명확하다).
    uint64_t needed = 16;  // 헤더(매직4+버전4+길이8).
    needed += 4;           // table_count.
    for (uint32_t i = 0; i < k_max_tables; ++i) {
        table_entry& t = g_tables[i];
        if (!t.used) {
            continue;
        }
        needed += 2 + cstr_len(t.path) + 4 + 4 + 1 + 1 + 1 + 1 + 4;
        for (uint32_t v = 0; v < k_max_values_per_table; ++v) {
            kv_entry& e = t.values[v];
            if (!e.used) {
                continue;
            }
            needed += 2 + cstr_len(e.key) + 1 + 4 + e.len;
        }
    }
    if (needed > sizeof(g_persist_buf)) {
        debug_log("[cfgsrv] persist write ok=0 (payload too large)\n");
        return;
    }

    uint8_t* p = g_persist_buf + 16;  // 헤더(매직4+버전4+길이8) 자리는 나중에 채운다.
    uint32_t table_count = 0;
    for (uint32_t i = 0; i < k_max_tables; ++i) {
        if (g_tables[i].used) {
            ++table_count;
        }
    }
    uint8_t* count_pos = p;
    w_u32(p, table_count);
    (void)count_pos;
    for (uint32_t i = 0; i < k_max_tables; ++i) {
        table_entry& t = g_tables[i];
        if (!t.used) {
            continue;
        }
        uint16_t path_len = static_cast<uint16_t>(cstr_len(t.path));
        w_u16(p, path_len);
        w_bytes(p, t.path, path_len);
        w_u32(p, t.owner_uid);
        w_u32(p, t.group_gid);
        w_u8(p, t.owner_rwx);
        w_u8(p, t.group_rwx);
        w_u8(p, t.other_rwx);
        w_u8(p, t.special_bits);
        uint32_t value_count = 0;
        for (uint32_t v = 0; v < k_max_values_per_table; ++v) {
            if (t.values[v].used) {
                ++value_count;
            }
        }
        w_u32(p, value_count);
        for (uint32_t v = 0; v < k_max_values_per_table; ++v) {
            kv_entry& e = t.values[v];
            if (!e.used) {
                continue;
            }
            uint16_t key_len = static_cast<uint16_t>(cstr_len(e.key));
            w_u16(p, key_len);
            w_bytes(p, e.key, key_len);
            w_u8(p, e.type);
            w_u32(p, e.len);
            w_bytes(p, e.data, e.len);
        }
    }
    uint64_t payload_len = static_cast<uint64_t>(p - (g_persist_buf + 16));
    uint8_t* header = g_persist_buf;
    w_u32(header, k_persist_magic);
    w_u32(header, k_persist_version);
    w_u64(header, payload_len);
    bool ok = persist_write_all(g_persist_buf, 16 + payload_len);
    debug_log(ok ? "[cfgsrv] persist write ok=1\n" : "[cfgsrv] persist write ok=0\n");
}

// 부팅 시 1회 — 파일이 없으면(최초 부팅) 그냥 빈 상태로 시작한다.
void persist_load() {
    uint64_t total = persist_read_all(g_persist_buf, sizeof(g_persist_buf));
    if (total < 16) {
        debug_log("[cfgsrv] persist load found=0\n");
        return;
    }
    const uint8_t* p = g_persist_buf;
    uint32_t magic = r_u32(p);
    uint32_t version = r_u32(p);
    uint64_t payload_len = r_u64(p);
    if (magic != k_persist_magic || version != k_persist_version || 16 + payload_len > total) {
        debug_log("[cfgsrv] persist load found=0\n");
        return;
    }
    uint32_t table_count = r_u32(p);
    for (uint32_t i = 0; i < table_count && i < k_max_tables; ++i) {
        table_entry& t = g_tables[i];
        t.used = true;
        uint16_t path_len = r_u16(p);
        r_bytes(p, t.path, path_len);
        t.path[path_len] = '\0';
        t.owner_uid = r_u32(p);
        t.group_gid = r_u32(p);
        t.owner_rwx = r_u8(p);
        t.group_rwx = r_u8(p);
        t.other_rwx = r_u8(p);
        t.special_bits = r_u8(p);
        uint32_t value_count = r_u32(p);
        for (uint32_t v = 0; v < value_count && v < k_max_values_per_table; ++v) {
            kv_entry& e = t.values[v];
            e.used = true;
            uint16_t key_len = r_u16(p);
            r_bytes(p, e.key, key_len);
            e.key[key_len] = '\0';
            e.type = r_u8(p);
            e.len = r_u32(p);
            r_bytes(p, e.data, e.len);
        }
    }
    debug_log("[cfgsrv] persist load found=1\n");
}

// ---------- 프로토콜 핸들러 ----------

void handle_open_or_create(const mc_message& in, mc_message& out, bool create) {
    uint32_t caller_uid = static_cast<uint32_t>(in.regs[0]);
    char caller_username[9];
    unpack_username_reg(caller_username, in.regs[1]);

    char raw_path[k_max_path_len];
    if (in.page_count != 1) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    const char* src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    uint32_t i = 0;
    for (; i < k_max_path_len - 1 && src[i] != '\0'; ++i) {
        raw_path[i] = src[i];
    }
    raw_path[i] = '\0';

    char normalized[k_max_path_len];
    if (!normalize_path(raw_path, caller_username, normalized, k_max_path_len)) {
        out.regs[0] = k_err_invalid_path;
        return;
    }

    table_entry* existing = find_table_by_path(normalized);
    if (create) {
        if (existing != nullptr) {
            out.regs[0] = k_err_already_exists;
            return;
        }
        // ADR-169 §결정4 영향 — create_table 허용 판정: 자기 스키마 또는 root(uid 0).
        if (caller_uid != 0 && !schema_matches(normalized, caller_username)) {
            out.regs[0] = k_err_permission_denied;
            return;
        }
        table_entry* slot = nullptr;
        for (uint32_t s = 0; s < k_max_tables; ++s) {
            if (!g_tables[s].used) {
                slot = &g_tables[s];
                break;
            }
        }
        if (slot == nullptr) {
            out.regs[0] = k_err_invalid_path;  // 테이블 슬롯 소진 — registry.md에 별도 코드 없음.
            return;
        }
        reset_table(*slot);
        slot->used = true;
        pack_bytes(slot->path, k_max_path_len, normalized, cstr_len(normalized));
        slot->owner_uid = caller_uid;
        slot->group_gid = 0;
        slot->owner_rwx = 0b111;  // 소유자 기본 전권.
        slot->group_rwx = 0;
        slot->other_rwx = 0;  // 기본 비공개.
        slot->special_bits = 0;
        persist_save();
        out.regs[0] = k_err_ok;
        out.regs[1] = table_id_of(slot);
        return;
    }

    if (existing == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    if (!any_access(*existing, caller_uid)) {
        out.regs[0] = k_err_permission_denied;
        return;
    }
    out.regs[0] = k_err_ok;
    out.regs[1] = table_id_of(existing);
}

void handle_delete_table(const mc_message& in, mc_message& out) {
    uint32_t caller_uid = static_cast<uint32_t>(in.regs[0]);
    char caller_username[9];
    unpack_username_reg(caller_username, in.regs[1]);

    if (in.page_count != 1) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    char raw_path[k_max_path_len];
    const char* src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    uint32_t i = 0;
    for (; i < k_max_path_len - 1 && src[i] != '\0'; ++i) {
        raw_path[i] = src[i];
    }
    raw_path[i] = '\0';

    char normalized[k_max_path_len];
    if (!normalize_path(raw_path, caller_username, normalized, k_max_path_len)) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    table_entry* t = find_table_by_path(normalized);
    if (t == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    if (!can_all(*t, caller_uid)) {
        out.regs[0] = k_err_permission_denied;
        return;
    }
    reset_table(*t);
    persist_save();
    out.regs[0] = k_err_ok;
}

// ADR-169 §결정1 — 중간 경로에는 별도 권한 메타데이터를 두지 않는다
// (registry.md §3 "아직 정하지 않은 것"과 같은 유예) — 항상 허용하고,
// 주어진 중간 경로 바로 아래 세그먼트(스키마 내 다음 계층)만 나열한다.
void handle_list_children(const mc_message& in, mc_message& out) {
    char caller_username[9];
    unpack_username_reg(caller_username, in.regs[1]);

    if (in.page_count != 1) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    char raw_path[k_max_path_len];
    const char* src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    uint32_t i = 0;
    for (; i < k_max_path_len - 1 && src[i] != '\0'; ++i) {
        raw_path[i] = src[i];
    }
    raw_path[i] = '\0';

    char normalized[k_max_path_len];
    // list_children의 대상은 leaf 테이블이 없어도(중간 경로만) 되므로
    // 여기서는 normalize_path 대신 직접 스키마/prefix를 만든다.
    const char* schema = caller_username;
    uint64_t schema_len = cstr_len(caller_username);
    const char* rest = raw_path;
    if (raw_path[0] == '@') {
        schema = raw_path + 1;
        uint64_t j = 0;
        while (schema[j] != '\0' && schema[j] != '/') {
            ++j;
        }
        schema_len = j;
        rest = (schema[j] == '/') ? schema + j + 1 : schema + j;
    }
    uint64_t rest_len = cstr_len(rest);
    uint32_t o = 0;
    for (uint64_t k = 0; k < schema_len; ++k) {
        normalized[o++] = schema[k];
    }
    normalized[o++] = '/';
    for (uint64_t k = 0; k < rest_len; ++k) {
        normalized[o++] = rest[k];
    }
    if (o > 0 && normalized[o - 1] != '/') {
        normalized[o++] = '/';
    }
    normalized[o] = '\0';
    uint64_t prefix_len = cstr_len(normalized);

    zero_bytes(g_io_scratch, k_page_size);
    uint32_t out_off = 0;
    uint32_t count = 0;
    for (uint32_t idx = 0; idx < k_max_tables; ++idx) {
        table_entry& t = g_tables[idx];
        if (!t.used) {
            continue;
        }
        uint64_t plen = cstr_len(t.path);
        if (plen <= prefix_len) {
            continue;
        }
        bool matches = true;
        for (uint64_t k = 0; k < prefix_len; ++k) {
            if (t.path[k] != normalized[k]) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        // prefix 다음 세그먼트만(그 다음 '/' 전까지).
        const char* rest2 = t.path + prefix_len;
        uint64_t seg_len = 0;
        while (rest2[seg_len] != '\0' && rest2[seg_len] != '/') {
            ++seg_len;
        }
        if (out_off + seg_len + 1 < k_page_size) {
            for (uint64_t k = 0; k < seg_len; ++k) {
                g_io_scratch[out_off++] = static_cast<uint8_t>(rest2[k]);
            }
            g_io_scratch[out_off++] = '\0';
            ++count;
        }
    }
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_io_scratch);
    out.pages[0].length = k_page_size;
    out.pages[0].mode = MC_TRANSFER_COPY;
    out.regs[0] = k_err_ok;
    out.regs[1] = count;
}

bool key_equals(const char* a, const char* b) {
    for (uint32_t i = 0; i < k_max_key_len; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
    return true;
}

kv_entry* find_value(table_entry& t, const char* key) {
    for (uint32_t i = 0; i < k_max_values_per_table; ++i) {
        if (t.values[i].used && key_equals(t.values[i].key, key)) {
            return &t.values[i];
        }
    }
    return nullptr;
}

void handle_get_value(const mc_message& in, mc_message& out) {
    uint32_t caller_uid = static_cast<uint32_t>(in.regs[0]);
    table_entry* t = table_by_id(in.regs[1]);
    if (t == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    if (!can_read(*t, caller_uid)) {
        out.regs[0] = k_err_permission_denied;
        return;
    }
    if (in.page_count != 1) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    char key[k_max_key_len];
    const char* src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    uint32_t i = 0;
    for (; i < k_max_key_len - 1 && src[i] != '\0'; ++i) {
        key[i] = src[i];
    }
    key[i] = '\0';

    kv_entry* e = find_value(*t, key);
    if (e == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    for (uint64_t k = 0; k < k_page_size; ++k) {
        g_io_scratch[k] = (k < e->len) ? e->data[k] : 0;
    }
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_io_scratch);
    out.pages[0].length = k_page_size;
    out.pages[0].mode = MC_TRANSFER_COPY;
    out.regs[0] = k_err_ok;
    out.regs[1] = e->type;
    out.regs[2] = e->len;
}

void handle_set_value(const mc_message& in, mc_message& out) {
    uint32_t caller_uid = static_cast<uint32_t>(in.regs[0]);
    table_entry* t = table_by_id(in.regs[1]);
    if (t == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    if (!can_write(*t, caller_uid)) {
        out.regs[0] = k_err_permission_denied;
        return;
    }
    uint8_t value_type = static_cast<uint8_t>(in.regs[2]);
    uint64_t value_len = in.regs[3];
    if (value_len > k_max_value_len) {
        value_len = k_max_value_len;
    }
    if (in.page_count != 2) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    char key[k_max_key_len];
    const char* key_src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    uint32_t i = 0;
    for (; i < k_max_key_len - 1 && key_src[i] != '\0'; ++i) {
        key[i] = key_src[i];
    }
    key[i] = '\0';
    const auto* value_src = reinterpret_cast<const uint8_t*>(in.pages[1].vaddr);

    kv_entry* e = find_value(*t, key);
    if (e == nullptr) {
        for (uint32_t s = 0; s < k_max_values_per_table; ++s) {
            if (!t->values[s].used) {
                e = &t->values[s];
                break;
            }
        }
        if (e == nullptr) {
            out.regs[0] = k_err_invalid_path;  // 값 슬롯 소진.
            return;
        }
    }
    e->used = true;
    pack_bytes(e->key, k_max_key_len, key, cstr_len(key));
    e->type = value_type;
    e->len = static_cast<uint32_t>(value_len);
    for (uint64_t k = 0; k < k_max_value_len; ++k) {
        e->data[k] = (k < value_len) ? value_src[k] : 0;
    }
    (void)k_type_string;
    (void)k_type_int64;
    (void)k_type_boolean;
    (void)k_type_binary;
    persist_save();
    out.regs[0] = k_err_ok;
}

void handle_delete_value(const mc_message& in, mc_message& out) {
    uint32_t caller_uid = static_cast<uint32_t>(in.regs[0]);
    table_entry* t = table_by_id(in.regs[1]);
    if (t == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    if (!can_write(*t, caller_uid)) {
        out.regs[0] = k_err_permission_denied;
        return;
    }
    if (in.page_count != 1) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    char key[k_max_key_len];
    const char* src = reinterpret_cast<const char*>(in.pages[0].vaddr);
    uint32_t i = 0;
    for (; i < k_max_key_len - 1 && src[i] != '\0'; ++i) {
        key[i] = src[i];
    }
    key[i] = '\0';
    kv_entry* e = find_value(*t, key);
    if (e == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    reset_kv(*e);
    persist_save();
    out.regs[0] = k_err_ok;
}

void handle_list_values(const mc_message& in, mc_message& out) {
    uint32_t caller_uid = static_cast<uint32_t>(in.regs[0]);
    table_entry* t = table_by_id(in.regs[1]);
    if (t == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    if (!can_read(*t, caller_uid)) {
        out.regs[0] = k_err_permission_denied;
        return;
    }
    zero_bytes(g_io_scratch, k_page_size);
    uint32_t out_off = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < k_max_values_per_table; ++i) {
        if (!t->values[i].used) {
            continue;
        }
        uint64_t klen = cstr_len(t->values[i].key);
        if (out_off + klen + 1 < k_page_size) {
            for (uint64_t k = 0; k < klen; ++k) {
                g_io_scratch[out_off++] = static_cast<uint8_t>(t->values[i].key[k]);
            }
            g_io_scratch[out_off++] = '\0';
            ++count;
        }
    }
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_io_scratch);
    out.pages[0].length = k_page_size;
    out.pages[0].mode = MC_TRANSFER_COPY;
    out.regs[0] = k_err_ok;
    out.regs[1] = count;
}

void handle_set_permissions(const mc_message& in, mc_message& out) {
    uint32_t caller_uid = static_cast<uint32_t>(in.regs[0]);
    table_entry* t = table_by_id(in.regs[1]);
    if (t == nullptr) {
        out.regs[0] = k_err_not_found;
        return;
    }
    // registry.md §5 — "소유자만"(root는 ADR-075 전권으로 예외).
    if (caller_uid != 0 && caller_uid != t->owner_uid) {
        out.regs[0] = k_err_permission_denied;
        return;
    }
    if (in.page_count != 1) {
        out.regs[0] = k_err_invalid_path;
        return;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(in.pages[0].vaddr);
    // registry.md §3 reg_permissions 원시 바이트: owner_uid(4)+group_gid(4)+
    // owner_rwx(1)+group_rwx(1)+other_rwx(1)+special_bits(1).
    uint32_t new_owner_uid = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                              (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    t->owner_uid = new_owner_uid;
    t->owner_rwx = p[8];
    t->group_rwx = 0;  // ADR-169 §결정1 — group 비트는 항상 무시.
    t->other_rwx = p[10];
    t->special_bits = p[11];
    persist_save();
    out.regs[0] = k_err_ok;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    persist_load();

    for (;;) {
        mc_message in{};
        uint64_t recv_err = do_syscall(MC_SYSCALL_IPC_RECV, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        mc_message out{};
        if (recv_err == 0) {
            out.label = in.label;
            switch (in.label) {
                case k_op_open_table:
                    handle_open_or_create(in, out, /*create=*/false);
                    break;
                case k_op_create_table:
                    handle_open_or_create(in, out, /*create=*/true);
                    break;
                case k_op_delete_table:
                    handle_delete_table(in, out);
                    break;
                case k_op_list_children:
                    handle_list_children(in, out);
                    break;
                case k_op_get_value:
                    handle_get_value(in, out);
                    break;
                case k_op_set_value:
                    handle_set_value(in, out);
                    break;
                case k_op_delete_value:
                    handle_delete_value(in, out);
                    break;
                case k_op_list_values:
                    handle_list_values(in, out);
                    break;
                case k_op_set_permissions:
                    handle_set_permissions(in, out);
                    break;
                default:
                    out.regs[0] = k_err_invalid_path;
                    break;
            }
        }
        do_syscall(MC_SYSCALL_IPC_REPLY, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}

}  // namespace kernsrv::cfgsrv
