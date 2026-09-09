// servers/procsrv/main.cpp — 프로세스 서버의 M12 골격
// (docs/plan/system-servers-bringup.md §M12, docs/spec/procsrv.md).
//
// M12는 procsrv.md가 정의하는 전체 프로세스 테이블·fd 진실 공급원·
// 계정 모델을 구현하지 않는다(계획 §M12 §구현2가 이미 "프로토콜
// 골격만"으로 scope했다) — 이 마일스톤의 QEMU 검증 목표는 딱 하나,
// "procsrv가 실제 디스크(virtio-blk+cpio)에서 읽혀 sys_process_spawn
// 으로 기동된 뒤, 자기 자신을 fork/exec해 실제 두 번째 완전한 유저
// 프로세스를 만들어내는 것"이다(kernel_main.cpp가 initrun에 대해
// 이미 검증한 것과 같은 절차를 procsrv 자신에게 반복하는 것 —
// ADR-149가 그 self_info 배선을 procsrv도 받을 수 있도록 일반화해
// 둬서 가능해졌다).
//
// initrun과 다른 점 — "나는 initrun이 방금 스폰한 원본인가, 아니면
// fork/exec으로 만들어진 사본인가"를 구분할 방법이 initrun의
// boot_info(항상 비어있지 않은 포인터)와 다르다: procsrv는 일반
// sys_process_spawn으로 만들어지므로 RDI는 그냥 argv 블록 주소(또는
// 인자가 없으면 0)다. 그래서 이 규약을 쓴다 — **initrun이 procsrv를
// 처음 스폰할 때만 argv를 비워두지 않고(예: "-" 한 글자짜리 블록),
// procsrv 자신의 self-exec 호출은 항상 argv 없이(0) 한다** — initrun의
// boot_info-vs-null 구분과 정확히 같은 비대칭을 argv 유무로 재현한다.
//
// M13(system-servers-bringup.md §M13, docs/spec/fs-protocol.md) — 여기
// procsrv를 "VFS를 실제로 쓰는 M12의 두 번째 프로세스"로 그대로
// 재사용한다(계획 §M13 §목표가 명시한 표현 그대로) — vfs에 파일을
// 열고 쓰고 다시 읽어 내용이 일치하는지 확인한다. procsrv.md의 실제
// 프로토콜(§2~9)은 여전히 구현하지 않는다 — 이 라운드트립은 그것과
// 무관한, fs-protocol.md 클라이언트 역할의 최소 검증일 뿐이다.
#include <uapi.hpp>

namespace {

// sys_process_spawn(create_endpoint=true)이 handle 1을 이 프로세스의
// endpoint로 채운다(ADR-152) — procsrv는 아직 아무도 이 endpoint에
// 걸지 않으므로 미사용. handle 2는 initrun이 스폰 시점에 넣어 준
// vfs endpoint 프록시(lib/*.ini의 `depends=vfs`).
constexpr uint32_t k_vfs_handle = 2;

constexpr uint32_t k_op_open = 1;
constexpr uint32_t k_op_write = 2;
constexpr uint32_t k_op_read = 3;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

[[noreturn]] void quiet_exit() {
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}

void debug_log(const char* msg, uint64_t len) {
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(msg), len, 0);
}

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

// fs-protocol.md §1 — M13은 pages[]를 안 쓰고 regs[]에 짧은 바이트를
// 그대로 눌러 담는다. path는 regs[0..3](32바이트) 전체, 데이터는
// regs[2..3](16바이트)만 — dst_bytes로 정확히 그 폭만 채운다(그
// 이상/이하로 쓰면 이웃 필드를 침범하거나 남은 바이트가 안 지워진다).
void pack_bytes(void* dst, uint64_t dst_bytes, const char* data, uint64_t len) {
    auto* d = static_cast<uint8_t*>(dst);
    for (uint64_t i = 0; i < dst_bytes; ++i) {
        d[i] = (i < len) ? static_cast<uint8_t>(data[i]) : 0;
    }
}

bool bytes_equal(const void* a, const void* b, uint64_t len) {
    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    for (uint64_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

// vfs→memfs로 파일을 열고, 그 응답으로 위임받은 memfs 핸들에 직접
// 쓰고 다시 읽어 내용이 일치하는지 확인한다(fs-protocol.md §2). 결과는
// sys_debug_log로만 관찰 가능하다(klog가 유저에 노출된 적이 없어서 —
// 이 파일 상단 주석 참고).
void run_vfs_roundtrip_test() {
    const char* path = "test.txt";
    const char* payload = "hello vfs";
    uint64_t payload_len = cstr_len(payload);

    uapi::message open_req{};
    open_req.label = k_op_open;
    pack_bytes(open_req.regs, sizeof(open_req.regs), path, cstr_len(path));
    uapi::message open_reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_vfs_handle, reinterpret_cast<uint64_t>(&open_req),
               reinterpret_cast<uint64_t>(&open_reply));

    bool open_ok = (open_reply.regs[1] == 0) && (open_reply.handle_count == 1);
    if (!open_ok) {
        debug_log("[procsrv] vfs open failed\n", cstr_len("[procsrv] vfs open failed\n"));
        return;
    }
    uint64_t open_file_id = open_reply.regs[0];
    uint32_t memfs_handle = open_reply.handles[0].src_handle;

    uapi::message write_req{};
    write_req.label = k_op_write;
    write_req.regs[0] = open_file_id;
    write_req.regs[1] = payload_len;
    pack_bytes(&write_req.regs[2], 2 * sizeof(uint64_t), payload, payload_len);  // regs[2..3]만.
    uapi::message write_reply{};
    do_syscall(uapi::k_syscall_ipc_call, memfs_handle, reinterpret_cast<uint64_t>(&write_req),
               reinterpret_cast<uint64_t>(&write_reply));
    bool write_ok = (write_reply.regs[1] == 0) && (write_reply.regs[0] == payload_len);

    // fs-protocol.md v2 §2.3(ADR-155 §2/ADR-159/ADR-161) — OP_READ의
    // 응답은 이제 pages[]로 온다. 커널이 sys_call이 돌아오기 전에
    // 이미 이 프로세스의 고정 슬롯에 매핑을 마쳐 뒀으므로,
    // read_reply.pages[0].vaddr을 그냥 읽으면 된다(별도 매핑/해제
    // 호출 불필요).
    uapi::message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = open_file_id;
    read_req.regs[1] = payload_len;
    uapi::message read_reply{};
    do_syscall(uapi::k_syscall_ipc_call, memfs_handle, reinterpret_cast<uint64_t>(&read_req),
               reinterpret_cast<uint64_t>(&read_reply));
    bool read_ok = (read_reply.regs[1] == 0) && (read_reply.regs[0] == payload_len) &&
                    read_reply.page_count == 1 &&
                    bytes_equal(reinterpret_cast<const void*>(read_reply.pages[0].vaddr), payload,
                                payload_len);

    if (write_ok && read_ok) {
        const char* msg = "[procsrv] vfs write/read roundtrip ok=1\n";
        debug_log(msg, cstr_len(msg));
    } else {
        const char* msg = "[procsrv] vfs write/read roundtrip ok=0\n";
        debug_log(msg, cstr_len(msg));
    }
}

// M16(fs-protocol.md v2, ADR-057/129) — vfs의 마운트 테이블을 거쳐
// fat32/ext4 서버가 실제로 마운트한 이미지에서 파일을 열어 읽는다.
// tools/make-fs-test-images.sh가 각 이미지의 루트에 hello.txt를
// 미리 심어 두므로(내용은 fat32/ext4가 서로 다름), 그 내용이 그대로
// 읽히는지 확인한다 — memfs 경로와 달리 여기는 **호스트가 이미
// 써 둔 내용을 게스트가 처음 읽는** 시나리오다(FAT32/ext4 v1은
// 읽기전용이라 OP_WRITE가 없다).
void run_mounted_fs_read_test(const char* mount_path, const char* expected,
                               const char* log_prefix) {
    uapi::message open_req{};
    open_req.label = k_op_open;
    pack_bytes(open_req.regs, sizeof(open_req.regs), mount_path, cstr_len(mount_path));
    uapi::message open_reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_vfs_handle, reinterpret_cast<uint64_t>(&open_req),
               reinterpret_cast<uint64_t>(&open_reply));

    bool open_ok = (open_reply.regs[1] == 0) && (open_reply.handle_count == 1);
    if (!open_ok) {
        debug_log(log_prefix, cstr_len(log_prefix));
        const char* msg = " open failed\n";
        debug_log(msg, cstr_len(msg));
        return;
    }
    uint64_t open_file_id = open_reply.regs[0];
    uint32_t fs_handle = open_reply.handles[0].src_handle;

    uapi::message read_req{};
    read_req.label = k_op_read;
    read_req.regs[0] = open_file_id;
    read_req.regs[1] = 4096;
    uapi::message read_reply{};
    do_syscall(uapi::k_syscall_ipc_call, fs_handle, reinterpret_cast<uint64_t>(&read_req),
               reinterpret_cast<uint64_t>(&read_reply));

    uint64_t expected_len = cstr_len(expected);
    bool read_ok = (read_reply.regs[1] == 0) && (read_reply.page_count == 1) &&
                    (read_reply.regs[0] == expected_len) &&
                    bytes_equal(reinterpret_cast<const void*>(read_reply.pages[0].vaddr), expected,
                                expected_len);

    debug_log(log_prefix, cstr_len(log_prefix));
    if (read_ok) {
        const char* msg = " read ok=1\n";
        debug_log(msg, cstr_len(msg));
    } else {
        const char* msg = " read ok=0\n";
        debug_log(msg, cstr_len(msg));
    }
}

}  // namespace

extern "C" [[noreturn]] void _start(const void* argv_or_null) {
    if (argv_or_null == nullptr) {
        quiet_exit();  // sys_fork+sys_exec으로 만들어진 사본.
    }

    const auto* self_info =
        reinterpret_cast<const uapi::m12_self_info*>(uapi::k_m12_self_info_user_vaddr);

    uint64_t fork_ret = do_syscall(uapi::k_syscall_fork, 0, 0, 0);
    if (fork_ret == 0) {
        // 자식 — sys_exec으로 자기 자신을 다시 실행(argv 없음 = 0,
        // 위 상단 주석의 규약대로 이 경로가 다음번 _start에서 사본으로
        // 식별된다).
        uapi::exec_request exec_req{};
        exec_req.elf_data = self_info->elf_addr;
        exec_req.elf_size = self_info->elf_size;
        do_syscall(uapi::k_syscall_exec, reinterpret_cast<uint64_t>(&exec_req), 0, 0);
        quiet_exit();  // exec 실패 시에만 도달.
    }

    // 부모 — fork/exec 왕복이 성공했다는 사실 자체가 M12의 검증
    // 목표다(커널의 process_ops.cpp가 이미 "[process] fork ok"/
    // "[process] exec ok"를 로그로 남긴다 — 어느 프로세스가 호출했는지
    // 구분하지 않는 일반 로그이므로 procsrv가 호출해도 그대로 관찰
    // 가능하다). M13의 검증 목표(VFS 경유 memfs 왕복)는 여기서 이어서
    // 확인한다.
    run_vfs_roundtrip_test();

    // M16 — tools/make-fs-test-images.sh가 심어 둔 내용과 정확히
    // 일치해야 한다(그 스크립트의 FAT32_CONTENT/EXT4_CONTENT).
    run_mounted_fs_read_test("/mnt/fat32/hello.txt", "hello fat32 world\n", "[procsrv] fat32");
    run_mounted_fs_read_test("/mnt/ext4/hello.txt", "hello ext4 world\n", "[procsrv] ext4");

    quiet_exit();
}
