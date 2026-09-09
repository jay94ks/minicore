// init/initrun/main.cpp — 커널이 유저모드로 띄우는 최초 프로세스의 최소
// 데모 바이너리 (docs/plan/kernel-bootstrap.md M8, docs/spec/boot.md §4/§6).
//
// 이 마일스톤에는 콘솔·파일시스템 등 유저모드 I/O 경로가 전혀 없다 —
// initrun의 유일한 "출력"은 커널에 보내는 IPC Call 그 자체다. 커널
// 쪽 수신 스레드(kernel_main.cpp의 thread_initrun_boot_server_entry)가
// 이 호출을 받으면 klog로 "부팅 성공"을 기록한다 — 관찰은 커널 시리얼
// 로그를 통해서만 가능하다. RDI로 boot_info 포인터를 규약대로(ADR-030/
// boot.md §6) 받지만, 이 데모는 아직 그 내용을 읽지 않는다(procsrv 등
// 실제로 부팅 정보가 필요한 이후 서버가 등장할 때 의미가 생긴다).
#include "cpio_reader.hpp"
#include "ini_parser.hpp"

#include <uapi.hpp>

namespace {

// cpio(newc) 엔트리 하나를 buf에 써 넣고 쓴 바이트 수를 돌려준다 —
// 8자리 16진수 필드를 손으로 인코딩하는 이 함수 자체가
// cpio_reader.cpp의 디코딩과 대칭을 이룬다(테스트 목적: 실제
// 아카이브를 만드는 tools/mkbootdisk.py 없이도 이 파서를 QEMU에서
// 실제로 실행해 검증할 수 있게 한다).
void write_hex8(uint8_t* out, uint32_t v) {
    for (int i = 7; i >= 0; --i) {
        uint32_t digit = v & 0xF;
        out[i] = static_cast<uint8_t>(digit < 10 ? ('0' + digit) : ('a' + digit - 10));
        v >>= 4;
    }
}

uint64_t write_cpio_entry(uint8_t* buf, const char* name, const uint8_t* data,
                           uint32_t filesize) {
    uint64_t off = 0;
    buf[off++] = '0';
    buf[off++] = '7';
    buf[off++] = '0';
    buf[off++] = '7';
    buf[off++] = '0';
    buf[off++] = '1';
    uint32_t namesize = 0;
    while (name[namesize] != '\0') {
        ++namesize;
    }
    ++namesize;  // NUL 포함.
    for (int field = 0; field < 13; ++field) {
        uint32_t v = 0;
        if (field == 6) {
            v = filesize;
        } else if (field == 11) {
            v = namesize;
        }
        write_hex8(buf + off, v);
        off += 8;
    }
    for (uint32_t i = 0; i < namesize; ++i) {
        buf[off + i] = static_cast<uint8_t>(name[i]);
    }
    off += namesize;
    while (off % 4 != 0) {
        buf[off++] = 0;
    }
    for (uint32_t i = 0; i < filesize; ++i) {
        buf[off + i] = data[i];
    }
    off += filesize;
    while (off % 4 != 0) {
        buf[off++] = 0;
    }
    return off;
}

bool span_equals(const char* data, uint64_t size, const char* expect) {
    uint64_t i = 0;
    for (; expect[i] != '\0'; ++i) {
        if (i >= size || data[i] != expect[i]) {
            return false;
        }
    }
    return i == size;
}

// cpio_reader/ini_parser를 실제로 실행해 검증한다(호스트 단위 테스트가
// 없는 대신, 이 프로젝트의 mcpack/elf_loader와 같은 방식 — QEMU에서
// 커널 스레드/유저 스레드 데모로 왕복 확인). 손으로 만든 최소 cpio
// 아카이브(파일 하나 + TRAILER) + INI 텍스트 하나로 두 파서 모두
// 한 번에 확인한다.
bool test_cpio_and_ini() {
    static uint8_t archive[256];
    const uint8_t file_data[] = {'h', 'i'};
    uint64_t off = write_cpio_entry(archive, "hello.txt", file_data, sizeof(file_data));
    off += write_cpio_entry(archive + off, "TRAILER!!!", nullptr, 0);

    auto found = cpio::find_entry(archive, off, "hello.txt");
    if (!found.is_ok() || found.value().size != 2 || found.value().data[0] != 'h' ||
        found.value().data[1] != 'i') {
        return false;
    }
    auto missing = cpio::find_entry(archive, off, "nope.txt");
    if (missing.is_ok()) {
        return false;
    }

    static const char ini_text[] = "[svc]\nexec=bin/svc\nargs=--foo bar\n; comment\nexec2=x\n";
    constexpr uint64_t ini_size = sizeof(ini_text) - 1;
    auto exec_val = ini::find_value(reinterpret_cast<const uint8_t*>(ini_text), ini_size, "exec");
    if (!exec_val.is_ok() || !span_equals(exec_val.value().data(), exec_val.value().size(),
                                          "bin/svc")) {
        return false;
    }
    auto args_val = ini::find_value(reinterpret_cast<const uint8_t*>(ini_text), ini_size, "args");
    if (!args_val.is_ok() || !span_equals(args_val.value().data(), args_val.value().size(),
                                          "--foo bar")) {
        return false;
    }
    return true;
}

// 커널이 이 프로세스의 handle_table을 만들 때 이 IPC 호출용 endpoint
// 프록시 핸들을 항상 가장 먼저(그리고 유일하게) 만들어 넣는다는 M8
// 한정 약속(kernel_main.cpp의 setup_initrun_process 참고) — 진짜
// 프로세스 환경(procsrv 이후)이라면 이런 고정 번호 대신 boot_info나
// 시작 인자로 전달해야 한다. handle 0은 항상 무효(k_invalid_handle)로
// 정해져 있으므로(objects.md) 테이블의 첫 핸들은 반드시 1이다.
constexpr uint32_t k_boot_endpoint_handle = 1;
constexpr uint32_t k_boot_label = 0xB007;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

// M12(system-servers-bringup.md §M12, ADR-142) — 실제 procsrv/cpio가
// 아직 없어(OPEN-53~57), sys_fork/sys_process_spawn/sys_exec 커널
// 프리미티브를 검증할 유일한 방법은 initrun 자신이 "자기 자신을
// 다시 spawn/exec"해 보는 것이다. kernel_main.cpp::setup_initrun_process가
// 이 목적으로 initrun 자신의 원본 ELF 바이트를 uapi::k_m12_self_elf_user_vaddr
// 에 미리 복사해 두고, 그 주소/크기를 uapi::k_m12_self_info_user_vaddr의
// uapi::m12_self_info로 알려 준다 — 이 배선은 procsrv/cpio가 실제로
// 생기면 통째로 사라질 임시 다리다.
//
// 흐름(RDI로 받는 값이 진짜 boot_info인지 아닌지로 "나는 원본인가
// 사본인가"를 구분한다 — process_spawn/exec으로 만들어진 사본은
// argv를 안 줬으므로 항상 nullptr을 받는다):
//   원본: sys_fork() → 자식은 sys_exec()으로 자기 자신을 다시 실행
//         (fork+exec 둘 다 실제로 동작했다는 증거가 커널 로그에 남는다) →
//         부모는 기존 M8 boot IPC call을 그대로 보낸 뒤 sys_process_spawn()
//         으로 또 다른 사본을 새 프로세스로 띄우고 → sys_thread_exit()으로
//         물러나 협조적 스케줄러가 자식/사본에게 기회를 준다.
//   사본: 조용히 sys_thread_exit().
[[noreturn]] void quiet_exit() {
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    // sys_thread_exit은 절대 반환하지 않는다 — 도달하면 커널 쪽 버그.
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace

extern "C" [[noreturn]] void _start(const void* boot_info_or_null) {
    if (boot_info_or_null == nullptr) {
        quiet_exit();  // fork/process_spawn/exec으로 만들어진 사본.
    }

    const auto* self_info =
        reinterpret_cast<const uapi::m12_self_info*>(uapi::k_m12_self_info_user_vaddr);

    uint64_t fork_ret = do_syscall(uapi::k_syscall_fork, 0, 0, 0);
    if (fork_ret == 0) {
        // 자식 — sys_exec()으로 자기 자신을 다시 실행한다. 성공하면
        // 이 호출은 반환하지 않는다(새 이미지가 arg0=nullptr로
        // _start부터 다시 시작 — quiet_exit 분기를 탄다).
        uapi::exec_request exec_req{};
        exec_req.elf_data = self_info->elf_addr;
        exec_req.elf_size = self_info->elf_size;
        do_syscall(uapi::k_syscall_exec, reinterpret_cast<uint64_t>(&exec_req), 0, 0);
        quiet_exit();  // exec 실패 시에만 도달.
    }

    // 부모 — 기존 M8 boot IPC call. regs[0]에 cpio/INI 파서 자체
    // 검증 결과(1=통과)를 실어 커널 로그로 확인한다(호스트 단위
    // 테스트가 없는 이 종류의 파서에 대한 이 프로젝트의 관례 —
    // mcpack/elf_loader와 마찬가지로 QEMU 왕복으로 검증).
    uapi::message out{};
    out.label = k_boot_label;
    out.regs[0] = test_cpio_and_ini() ? 1 : 0;
    uapi::message in{};
    do_syscall(uapi::k_syscall_ipc_call, k_boot_endpoint_handle,
               reinterpret_cast<uint64_t>(&out), reinterpret_cast<uint64_t>(&in));

    // sys_process_spawn() — 자기 자신의 또 다른 사본을 새 프로세스로.
    uapi::process_spawn_request spawn_req{};
    spawn_req.elf_data = self_info->elf_addr;
    spawn_req.elf_size = self_info->elf_size;
    do_syscall(uapi::k_syscall_process_spawn, reinterpret_cast<uint64_t>(&spawn_req), 0, 0);

    quiet_exit();
}
