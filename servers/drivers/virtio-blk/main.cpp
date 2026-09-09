// servers/drivers/virtio-blk/main.cpp — virtio-blk 드라이버
// (docs/plan/system-servers-bringup.md §M15, boot-and-drivers.md
// ADR-043 1순위 — 첫 실제 유저 드라이버).
//
// init/initrun/virtio_blk.hpp/.cpp(ADR-131/147)와는 의도적으로
// 완전히 별개다 — 그건 부트스트랩 1회성(읽기 전용, 커널이 이미
// 찾아 배정해 둔 BAR를 그대로 씀)이고, 이건 devmgr에 정식으로
// 등록해 BAR를 받는 "진짜" 드라이버 자리를 보인다(읽기+쓰기 모두
// 다룬다). 같은 레거시 virtio-blk 레지스터 프로토콜을 다시 구현하는
// 이유는 두 코드가 근본적으로 다른 신뢰 경계·생명주기를 가져서다
// (ADR-131 §근거와 같은 정신 — 지금은 libmc가 없어 실제로도 공유할
// 방법이 없다).
//
// M15 목표: devmgr에게 등록해(vendor:device=0x1af4:0x1001) 위임받은
// I/O 포트로 알려진 패턴을 한 섹터에 쓰고 다시 읽어 내용이 일치하는지
// 확인한다 — 이 시점엔 initrun의 부트 목적(cpio 아카이브에서 서비스
// 바이너리를 읽는 것)이 이미 끝나 있어, 어느 섹터에 써도 안전하다.
#include <uapi.hpp>

namespace {

constexpr uint32_t k_devmgr_handle = 2;  // depends=devmgr(lib/*.ini).

constexpr uint32_t k_op_register_driver = 1;
constexpr uint64_t k_match_mode_vendor_device = 0;
constexpr uint64_t k_status_ok = 0;
constexpr uint64_t k_is_io_bit = 1ull << 32;  // devmgr/main.cpp의 regs[3] 인코딩과 일치.

constexpr uint64_t k_virtio_vendor_device = (0x1AF4ull << 16) | 0x1001ull;

// --- legacy virtio-blk 레지스터 오프셋(io_base 기준, VirtIO 1.0
// §4.1.4.8) — init/initrun/virtio_blk.cpp와 동일한 레이아웃. ---
constexpr uint16_t k_reg_host_features = 0x00;
constexpr uint16_t k_reg_guest_features = 0x04;
constexpr uint16_t k_reg_queue_address = 0x08;
constexpr uint16_t k_reg_queue_size = 0x0C;
constexpr uint16_t k_reg_queue_select = 0x0E;
constexpr uint16_t k_reg_queue_notify = 0x10;
constexpr uint16_t k_reg_device_status = 0x12;

constexpr uint8_t k_status_acknowledge = 1;
constexpr uint8_t k_status_driver = 2;
constexpr uint8_t k_status_driver_ok = 4;

constexpr uint16_t k_desc_flag_next = 1;
constexpr uint16_t k_desc_flag_write = 2;  // 디바이스가 이 디스크립터에 쓴다.

constexpr uint32_t k_virtio_blk_t_in = 0;   // 읽기.
constexpr uint32_t k_virtio_blk_t_out = 1;  // 쓰기.

constexpr uint64_t k_sector_size = 512;
constexpr uint64_t k_test_sector = 10;  // 부트 디스크 용량 안의 임의 섹터(이 파일 상단 주석 참고).

// vring(16KiB)+요청 헤더(16바이트)+데이터(섹터 하나, 512바이트)+상태
// (1바이트) — DMA 버퍼 하나에 전부 담는다(초기화 시 한 번 확보).
constexpr uint64_t k_vring_region_size = 0x4000;
constexpr uint64_t k_req_header_offset = k_vring_region_size;
constexpr uint64_t k_req_header_size = 16;
constexpr uint64_t k_data_offset = k_req_header_offset + k_req_header_size;
constexpr uint64_t k_status_offset = k_data_offset + k_sector_size;
constexpr uint32_t k_dma_buffer_order = 5;  // 4KiB<<5 = 128KiB, 위 전부를 여유 있게 담는다.

// avail_idx/used_idx는 큐가 살아있는 동안 계속 증가하는 카운터다
// (mod queue_size로 링 슬롯을 고른다) — 매 요청마다 1로 되돌리면
// 디바이스가 "이미 처리한 인덱스"라고 보고 새 요청을 무시한다(이
// 드라이버가 처음에 겪은 실제 버그, 2026-09-09). init()이 채운다.
uint16_t g_queue_size = 0;
uint16_t g_avail_idx = 0;

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
void debug_log(const char* msg) {
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(msg), cstr_len(msg), 0);
}
void debug_log_hex(const char* prefix, uint64_t value) {
    char buf[96];
    uint64_t i = 0;
    for (; prefix[i] != '\0' && i < 60; ++i) {
        buf[i] = prefix[i];
    }
    buf[i++] = '0';
    buf[i++] = 'x';
    bool leading = true;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xF);
        if (nibble != 0 || !leading || shift == 0) {
            leading = false;
            buf[i++] = static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
        }
    }
    buf[i++] = '\n';
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(buf), i, 0);
}

void out8(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
uint16_t in16(uint16_t port) {
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void out16(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
uint32_t in32(uint16_t port) {
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
void out32(uint16_t port, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }

constexpr uint32_t k_page_size = 4096;
uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct vring_layout {
    uint64_t desc_off;
    uint64_t avail_off;
    uint64_t used_off;
    uint64_t total_size;
};

vring_layout compute_layout(uint16_t num) {
    vring_layout l;
    l.desc_off = 0;
    uint64_t desc_len = 16ull * num;
    l.avail_off = desc_len;
    uint64_t avail_len = 2ull * (3 + num);
    l.used_off = align_up(desc_len + avail_len, k_page_size);
    uint64_t used_len = 2ull * 3 + 8ull * num;
    l.total_size = l.used_off + used_len;
    return l;
}

bool init(uint16_t io_base, uint8_t* dma_virt, uint64_t dma_phys, vring_layout& out_layout) {
    out8(io_base + k_reg_device_status, 0);
    out8(io_base + k_reg_device_status, k_status_acknowledge);
    out8(io_base + k_reg_device_status, k_status_acknowledge | k_status_driver);

    (void)in32(io_base + k_reg_host_features);
    out32(io_base + k_reg_guest_features, 0);

    out16(io_base + k_reg_queue_select, 0);
    uint16_t queue_size = in16(io_base + k_reg_queue_size);
    if (queue_size == 0) {
        return false;
    }
    vring_layout layout = compute_layout(queue_size);
    if (layout.total_size > k_vring_region_size) {
        return false;
    }
    for (uint64_t i = 0; i < k_vring_region_size; ++i) {
        dma_virt[i] = 0;
    }

    out32(io_base + k_reg_queue_address, static_cast<uint32_t>(dma_phys / k_page_size));
    out8(io_base + k_reg_device_status,
         k_status_acknowledge | k_status_driver | k_status_driver_ok);

    out_layout = layout;
    g_queue_size = queue_size;
    g_avail_idx = 0;
    return true;
}

// 섹터 하나(512바이트)를 읽거나 쓴다 — is_write=false면 sector에서
// dma_virt+k_data_offset로 읽어 오고(호출자가 그 뒤 확인), is_write=
// true면 dma_virt+k_data_offset에 이미 채워 둔 512바이트를 sector에
// 쓴다.
bool do_request(uint16_t io_base, uint8_t* dma_virt, uint64_t dma_phys,
                 const vring_layout& layout, uint64_t sector, bool is_write) {
    auto* req_type = reinterpret_cast<uint32_t*>(dma_virt + k_req_header_offset);
    req_type[0] = is_write ? k_virtio_blk_t_out : k_virtio_blk_t_in;
    req_type[1] = 0;
    auto* req_sector = reinterpret_cast<uint64_t*>(dma_virt + k_req_header_offset + 8);
    *req_sector = sector;

    dma_virt[k_status_offset] = 0xFF;

    struct desc_entry {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    };
    auto* desc = reinterpret_cast<desc_entry*>(dma_virt + layout.desc_off);
    // 쓰기 요청의 데이터 디스크립터는 "디바이스가 읽는다"(k_desc_flag_write
    // 없음) — 읽기 요청은 반대로 디바이스가 우리 버퍼에 쓴다.
    uint16_t data_flags = k_desc_flag_next | (is_write ? 0 : k_desc_flag_write);
    desc[0] = {dma_phys + k_req_header_offset, static_cast<uint32_t>(k_req_header_size),
               k_desc_flag_next, 1};
    desc[1] = {dma_phys + k_data_offset, static_cast<uint32_t>(k_sector_size), data_flags, 2};
    desc[2] = {dma_phys + k_status_offset, 1, k_desc_flag_write, 0};

    // avail_idx는 큐가 살아있는 동안 계속 증가하는 카운터다(mod
    // queue_size로 슬롯을 고른다) — 매번 1로 되돌리면 디바이스가
    // "이미 처리했다"고 보고 새 요청을 무시한다(이 드라이버가 처음에
    // 겪은 실제 버그 — g_avail_idx/g_queue_size 상단 주석 참고).
    auto* avail_flags = reinterpret_cast<uint16_t*>(dma_virt + layout.avail_off);
    auto* avail_idx_ptr = avail_flags + 1;
    auto* avail_ring = avail_flags + 2;
    avail_flags[0] = 0;
    uint16_t slot = static_cast<uint16_t>(g_avail_idx % g_queue_size);
    avail_ring[slot] = 0;  // head descriptor index(항상 0 — 디스크립터 3개를 매번 재사용).
    ++g_avail_idx;
    *avail_idx_ptr = g_avail_idx;

    auto* used_idx_ptr = reinterpret_cast<volatile uint16_t*>(dma_virt + layout.used_off + 2);
    uint16_t used_idx_before = *used_idx_ptr;

    out16(io_base + k_reg_queue_notify, 0);

    constexpr uint64_t k_poll_iterations = 100'000'000ull;
    bool completed = false;
    for (uint64_t i = 0; i < k_poll_iterations; ++i) {
        if (*used_idx_ptr != used_idx_before) {
            completed = true;
            break;
        }
    }
    if (!completed) {
        return false;
    }
    return dma_virt[k_status_offset] == 0;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    uapi::message req{};
    req.label = k_op_register_driver;
    req.regs[0] = k_match_mode_vendor_device;
    req.regs[1] = k_virtio_vendor_device;
    req.regs[2] = 0;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_devmgr_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));

    if (reply.regs[0] != k_status_ok || (reply.regs[3] & k_is_io_bit) == 0) {
        debug_log("[virtio-blk] no I/O-BAR virtio-blk device registered by devmgr\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    auto io_base = static_cast<uint16_t>(reply.regs[1]);
    debug_log_hex("[virtio-blk] io_base=", io_base);

    do_syscall(uapi::k_syscall_io_activate, io_base, 0x20, 0);

    uapi::dma_buffer_result dma{};
    uint64_t alloc_err = do_syscall(uapi::k_syscall_alloc_dma_buffer,
                                     reinterpret_cast<uint64_t>(&dma), k_dma_buffer_order, 0);
    if (alloc_err != 0) {
        debug_log("[virtio-blk] alloc_dma_buffer failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    auto* dma_virt = reinterpret_cast<uint8_t*>(dma.virt_addr);

    vring_layout layout;
    if (!init(io_base, dma_virt, dma.phys_addr, layout)) {
        debug_log("[virtio-blk] init failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    debug_log("[virtio-blk] init ok\n");

    // 알려진 패턴(주소를 바이트로 흩뿌린 값)을 섹터 버퍼에 채운다.
    uint8_t* data = dma_virt + k_data_offset;
    for (uint64_t i = 0; i < k_sector_size; ++i) {
        data[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }

    bool write_ok = do_request(io_base, dma_virt, dma.phys_addr, layout, k_test_sector, true);
    debug_log_hex("[virtio-blk] write_ok=", write_ok ? 1 : 0);

    // 읽기 전에 데이터 영역을 오염시켜 둔다 — 그대로 남아 있는 값을
    // "성공"으로 오인하지 않도록(진짜로 디바이스가 다시 채웠는지 확인).
    for (uint64_t i = 0; i < k_sector_size; ++i) {
        data[i] = 0xAA;
    }

    bool read_ok = do_request(io_base, dma_virt, dma.phys_addr, layout, k_test_sector, false);
    debug_log_hex("[virtio-blk] read_ok=", read_ok ? 1 : 0);

    bool content_matches = true;
    for (uint64_t i = 0; i < k_sector_size; ++i) {
        if (data[i] != static_cast<uint8_t>((i * 37 + 11) & 0xFF)) {
            content_matches = false;
            break;
        }
    }

    if (write_ok && read_ok && content_matches) {
        debug_log("[virtio-blk] write/read roundtrip ok=1\n");
    } else {
        debug_log("[virtio-blk] write/read roundtrip ok=0\n");
    }

    do_syscall(uapi::k_syscall_io_deactivate, 0, 0, 0);
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}
