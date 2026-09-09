// init/initrun/main.cpp — 커널이 유저모드로 띄우는 최초 프로세스
// (docs/plan/kernel-bootstrap.md M8, docs/plan/system-servers-bringup.md
// §M12, docs/spec/boot.md §4/§6, ADR-131).
//
// M8~M11 동안은 콘솔·파일시스템이 전혀 없어 커널에 보내는 IPC Call
// 하나가 유일한 "출력"이었다(관찰은 kernel_main.cpp의
// thread_initrun_boot_server_entry가 klog로 남기는 로그를 통해서만
// 가능 — 지금도 그 경로를 그대로 쓴다). M12부터는 그 위에 실제 역할이
// 추가된다: RDI로 받는 boot_info(ADR-030/boot.md §6)의
// boot_device_descriptor로 부트 디바이스(virtio-blk)를 직접 마운트해
// (임베디드 최소 virtio-blk 클라이언트, virtio_blk.hpp) cpio(newc)
// 아카이브를 읽고, 그 안 `lib/*.ini`가 가리키는 서비스(M12는 procsrv
// 하나)를 `sys_process_spawn`으로 띄운다. M8~M11이 검증에 쓰던
// "initrun 자신을 fork/exec/spawn하는 self-test 데모"는 이제
// procsrv가 실제 디스크에서 읽혀 스폰된 뒤 자기 자신을 fork/exec하는
// 것으로 대체됐다(kernel/arch/x86_64/process_ops.cpp가 호출자를
// 구분하지 않고 남기는 같은 "[process] fork/exec/spawn ok" 로그이므로
// 같은 스모크 테스트 어써션이 이제는 이 실제 경로로 충족된다) —
// uapi.hpp::m12_self_info 주석이 예고한 대로 이 자리에서 그 임시
// 다리를 걷어냈다.
#include "cpio_reader.hpp"
#include "ini_parser.hpp"
#include "virtio_blk.hpp"

#include <boot_info.hpp>
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

[[noreturn]] void quiet_exit() {
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    // sys_thread_exit은 절대 반환하지 않는다 — 도달하면 커널 쪽 버그.
    for (;;) {
        asm volatile("pause");
    }
}

bool starts_with(const char* name, const char* prefix) {
    while (*prefix != '\0') {
        if (*name != *prefix) {
            return false;
        }
        ++name;
        ++prefix;
    }
    return true;
}

bool ends_with_ini(const char* name) {
    uint64_t len = 0;
    while (name[len] != '\0') {
        ++len;
    }
    return len >= 4 && name[len - 4] == '.' && name[len - 3] == 'i' && name[len - 2] == 'n' &&
           name[len - 1] == 'i';
}

// "lib/NNN-이름.ini"(tools/mkbootdisk.py가 쓰는 형식)에서 "이름"만
// 뽑아낸다 — ADR-152의 서비스 이름→endpoint 프록시 핸들 레지스트리
// (spawn_ctx::registry)의 키로 쓴다. ini_name은 이미 starts_with("lib/")
// && ends_with_ini()를 통과했다고 가정한다.
bool extract_service_name(const char* ini_name, char* out, uint64_t out_size) {
    const char* p = ini_name + 4;  // "lib/" 건너뜀.
    while (*p >= '0' && *p <= '9') {
        ++p;
    }
    if (*p == '-') {
        ++p;
    }
    uint64_t i = 0;
    while (p[i] != '\0') {
        if (p[i] == '.' && p[i + 1] == 'i' && p[i + 2] == 'n' && p[i + 3] == 'i' &&
            p[i + 4] == '\0') {
            break;
        }
        if (i + 1 >= out_size) {
            return false;
        }
        out[i] = p[i];
        ++i;
    }
    out[i] = '\0';
    return true;
}

bool cstr_equals(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

// ini::find_value()가 돌려주는 span은 원본 아카이브 버퍼를 그대로
// 가리킬 뿐 NUL로 끝나지 않는다 — cpio::find_entry()는 NUL 종료
// 문자열을 요구하므로 고정 버퍼에 복사해 NUL을 붙인다.
bool copy_span_to_cstr(span<const char> s, char* buf, uint64_t buf_size) {
    if (s.size() + 1 > buf_size) {
        return false;
    }
    for (uint64_t i = 0; i < s.size(); ++i) {
        buf[i] = s.data()[i];
    }
    buf[s.size()] = '\0';
    return true;
}

// procsrv(M12 QEMU 검증 대상)가 "나는 initrun이 방금 스폰한 원본인가,
// 아니면 sys_fork+sys_exec으로 만들어진 사본인가"를 구분하는 데
// 쓴다(servers/procsrv/main.cpp 상단 주석의 규약) — initrun의
// boot_info(항상 비어있지 않은 포인터) vs null 구분과 같은 비대칭을
// argv 유무로 재현한 것이라, 내용 자체는 의미가 없고 그냥 비어있지만
// 않으면 된다.
constexpr uint8_t k_service_argv_marker[] = {'x'};

// M13(ADR-152) — 서비스 이름→(initrun 자신의 테이블에 있는) endpoint
// 프록시 핸들 레지스트리. 등록/탐색 서비스가 없어(OPEN-59) initrun이
// 스폰 순서대로 직접 기록해 뒀다가, 그 뒤에 스폰하는 다른 서비스의
// `depends=`가 이 이름을 가리키면 그 핸들을 inherited_handles로
// 넘긴다.
constexpr uint32_t k_max_registered_services = 8;

struct service_registry_entry {
    char name[32] = {};
    uint32_t endpoint_proxy_handle = 0;
};

struct spawn_ctx {
    const uint8_t* archive;
    uint64_t archive_size;
    uint32_t spawned_count = 0;
    service_registry_entry registry[k_max_registered_services];
    uint32_t registry_count = 0;
};

uint32_t find_registered_handle(const spawn_ctx& ctx, const char* name) {
    for (uint32_t i = 0; i < ctx.registry_count; ++i) {
        if (cstr_equals(ctx.registry[i].name, name)) {
            return ctx.registry[i].endpoint_proxy_handle;
        }
    }
    return 0;
}

// cpio::for_each_entry()가 아카이브에 기록된 순서(=lib/*.ini 파일명
// 순서, tools/mkbootdisk.py가 그렇게 써 둔다, ADR-131 §결정5)대로
// 각 엔트리에 대해 호출한다 — lib/*.ini가 아닌 엔트리(bin/* 자체,
// disk.cfg 등)는 건너뛴다. 이 순서가 그대로 의존성 해결 순서이기도
// 하다(ADR-152) — `depends=`가 가리키는 서비스는 반드시 그보다
// 먼저 나열돼 있어야 한다.
void spawn_visit(void* ctx_raw, const char* name, const uint8_t* data, uint64_t size) {
    if (!starts_with(name, "lib/") || !ends_with_ini(name)) {
        return;
    }
    auto exec_val = ini::find_value(data, size, "exec");
    if (!exec_val.is_ok()) {
        return;
    }
    char exec_path[64];
    if (!copy_span_to_cstr(exec_val.value(), exec_path, sizeof(exec_path))) {
        return;
    }
    char service_name[32];
    if (!extract_service_name(name, service_name, sizeof(service_name))) {
        return;
    }

    auto* ctx = static_cast<spawn_ctx*>(ctx_raw);
    auto elf_entry = cpio::find_entry(ctx->archive, ctx->archive_size, exec_path);
    if (!elf_entry.is_ok()) {
        return;
    }

    uapi::process_spawn_request req{};
    req.elf_data = reinterpret_cast<uint64_t>(elf_entry.value().data);
    req.elf_size = elf_entry.value().size;
    req.argv_blob = reinterpret_cast<uint64_t>(k_service_argv_marker);
    req.argv_size = sizeof(k_service_argv_marker);
    req.grant_trusted = false;  // M12/M13의 서비스는 하드웨어를 직접 다루지 않는다.
    req.create_endpoint = true;  // M13(ADR-152) — 모든 서비스가 handle 1로 자기 endpoint를 받는다.

    auto depends_val = ini::find_value(data, size, "depends");
    if (depends_val.is_ok()) {
        char dep_name[32];
        if (copy_span_to_cstr(depends_val.value(), dep_name, sizeof(dep_name))) {
            uint32_t dep_handle = find_registered_handle(*ctx, dep_name);
            if (dep_handle != 0) {
                req.inherited_handle_count = 1;
                req.inherited_handles[0].src_handle = dep_handle;
                req.inherited_handles[0].rights_mask = uapi::k_right_can_send;
            }
        }
    }

    do_syscall(uapi::k_syscall_process_spawn, reinterpret_cast<uint64_t>(&req), 0, 0);
    ++ctx->spawned_count;

    if (req.out_endpoint_proxy_handle != 0 && ctx->registry_count < k_max_registered_services) {
        service_registry_entry& entry = ctx->registry[ctx->registry_count];
        uint64_t i = 0;
        for (; i + 1 < sizeof(entry.name) && service_name[i] != '\0'; ++i) {
            entry.name[i] = service_name[i];
        }
        entry.name[i] = '\0';
        entry.endpoint_proxy_handle = req.out_endpoint_proxy_handle;
        ++ctx->registry_count;
    }
}

// ADR-131 §결정1~5/§근거, ADR-147 — boot_info.boot_device로 알려진
// virtio-blk 부트 디바이스를 임베디드 클라이언트로 마운트해 그 안의
// cpio(newc) 아카이브를 읽고, lib/*.ini가 가리키는 서비스들을
// sys_process_spawn한다. 디바이스가 없거나(io_port_ok==0) 어느
// 단계에서든 실패하면 그냥 false — M12는 ADR-131 §결정3의 PCIe
// 폴백 스캔을 구현하지 않으므로 이 경우 그대로 부팅을 포기한다.
bool mount_boot_device_and_spawn_services(const boot::boot_info& bi) {
    if (bi.boot_device.valid == 0 || bi.boot_device.io_port_ok == 0) {
        return false;
    }

    // order=10 → 4MiB. vring(최대 16KiB 예약, virtio_blk.cpp) + 부트
    // 디스크 내용(M12는 procsrv 하나뿐이라 수십 KiB) 전부를 넉넉히
    // 담는다 — 앞으로 서비스가 늘어도 3MiB 상한(k_max_boot_disk_bytes)
    // 안에서는 그대로 재사용 가능하다.
    constexpr uint32_t k_dma_buffer_order = 10;
    uapi::dma_buffer_result dma{};
    uint64_t alloc_err = do_syscall(uapi::k_syscall_alloc_dma_buffer,
                                     reinterpret_cast<uint64_t>(&dma), k_dma_buffer_order, 0);
    if (alloc_err != 0) {
        return false;
    }

    auto* dma_virt = reinterpret_cast<uint8_t*>(dma.virt_addr);
    auto io_base = static_cast<uint16_t>(bi.boot_device.io_port_base);

    // M14(ADR-154) — 커널이 더 이상 부팅 중에 무조건 열어 주지 않는다
    // (스레드별 IOPB로 바뀌면서, "필요한 순간에 직접 활성화"가 원칙이
    // 됐다) — virtio-blk 레지스터에 실제로 접근하기 직전에 이 스레드
    // 자신이 활성화한다. 0x20(가정한 레지스터 범위, ADR-147과 동일).
    do_syscall(uapi::k_syscall_io_activate, io_base, 0x20, 0);

    if (!virtio_blk::init(io_base, dma_virt, dma.phys_addr)) {
        return false;
    }

    const uint8_t* archive = nullptr;
    uint64_t archive_len = 0;
    constexpr uint64_t k_max_boot_disk_bytes = 3ull * 1024 * 1024;
    if (!virtio_blk::read_all(io_base, dma_virt, dma.phys_addr, k_max_boot_disk_bytes, &archive,
                               &archive_len)) {
        return false;
    }

    spawn_ctx ctx;
    ctx.archive = archive;
    ctx.archive_size = archive_len;
    auto walked = cpio::for_each_entry(archive, archive_len, &spawn_visit, &ctx);
    return walked.is_ok() && ctx.spawned_count > 0;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void* boot_info_or_null) {
    if (boot_info_or_null == nullptr) {
        quiet_exit();  // sys_process_spawn/sys_exec으로 만들어진 사본(이 프로세스는 원래 initrun 자신을 다시 만들 일이 없다 — 방어적으로만 남겨 둔다).
    }

    const auto& bi = *reinterpret_cast<const boot::boot_info*>(boot_info_or_null);

    // 기존 M8 boot IPC call. regs[0]에 cpio/INI 파서 자체 검증 결과
    // (1=통과)를 실어 커널 로그로 확인한다(호스트 단위 테스트가 없는
    // 이 종류의 파서에 대한 이 프로젝트의 관례 — mcpack/elf_loader와
    // 마찬가지로 QEMU 왕복으로 검증). 이 자체 테스트는 실제 부트
    // 디바이스와 무관하게 항상 돈다.
    uapi::message out{};
    out.label = k_boot_label;
    out.regs[0] = test_cpio_and_ini() ? 1 : 0;
    uapi::message in{};
    do_syscall(uapi::k_syscall_ipc_call, k_boot_endpoint_handle,
               reinterpret_cast<uint64_t>(&out), reinterpret_cast<uint64_t>(&in));

    // M12(ADR-131) — 실제 부트 디바이스 마운트 + 서비스 스폰. 이후
    // 하나 이상의 서비스가 스케줄될 기회를 얻으려면(협조적 스케줄러라
    // sys_yield가 없다, uapi.hpp 참고) initrun 자신이 물러나야 한다 —
    // 그래서 성공/실패와 무관하게 곧바로 quiet_exit()한다. OPEN-51
    // ("systemd류 초기 프로세스"의 정체성)은 아직 미해결이라 M12는
    // 스폰된 서비스가 곧 initrun 이후의 유일한 프로세스다.
    mount_boot_device_and_spawn_services(bi);

    quiet_exit();
}
