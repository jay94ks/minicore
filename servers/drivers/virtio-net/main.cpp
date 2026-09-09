// servers/drivers/virtio-net/main.cpp — virtio-net 드라이버
// (docs/plan/general-purpose-completion.md §M25,
// docs/design/boot-and-drivers.md ADR-181).
//
// servers/drivers/virtio-blk/main.cpp와 완전히 같은 legacy virtio
// I/O 포트 레지스터 레이아웃을 재사용한다(레지스터 오프셋이 모든
// legacy virtio 디바이스에 공통이다, VirtIO 1.0 §4.1.4.8) — 다른
// 점은 큐가 두 개(0=RX, 1=TX)이고 디바이스별 설정 공간(MAC 주소)이
// 있다는 것뿐이다. blk와 코드를 공유하지 않는 이유도 blk와 같다
// (서로 다른 신뢰 경계, 실제로 공유할 방법도 없다).
//
// 이 드라이버는 순수 하드웨어 계층이다 — 이더넷 프레임을 그대로
// 보내고 받을 뿐, ARP/IP/UDP 같은 프로토콜은 전혀 모른다(그건
// servers/netsrv 소관, ADR-002/006/008과 같은 계층 분리 정신).
#include <uapi.hpp>

namespace {

constexpr uint32_t k_devmgr_handle = 2;    // depends=devmgr(lib/*.ini).
constexpr uint32_t k_own_endpoint_handle = 1;

constexpr uint32_t k_op_register_driver = 1;
constexpr uint64_t k_match_mode_vendor_device = 0;
constexpr uint64_t k_status_ok = 0;
constexpr uint64_t k_is_io_bit = 1ull << 32;

// legacy virtio-net(VirtIO 1.0 §5.1 — transitional device, device
// ID 0x1000). virtio-blk는 0x1001이었다.
constexpr uint64_t k_virtio_net_vendor_device = (0x1AF4ull << 16) | 0x1000ull;

// --- legacy virtio 공통 레지스터(virtio-blk/main.cpp와 동일). ---
constexpr uint16_t k_reg_host_features = 0x00;
constexpr uint16_t k_reg_guest_features = 0x04;
constexpr uint16_t k_reg_queue_address = 0x08;
constexpr uint16_t k_reg_queue_size = 0x0C;
constexpr uint16_t k_reg_queue_select = 0x0E;
constexpr uint16_t k_reg_queue_notify = 0x10;
constexpr uint16_t k_reg_device_status = 0x12;
// 디바이스별 설정 공간 — MSI-X 없이(이 드라이버는 안 쓴다) 공통
// 레지스터(0x00~0x13, 20바이트) 바로 뒤. virtio-net legacy §5.1.4:
// mac[6]가 이 오프셋부터 시작한다(VIRTIO_NET_F_MAC을 협상하지
// 않아도 QEMU는 항상 이 필드를 채운다 — 실기 여부에 좌우되지 않는
// QEMU 에뮬레이션 전제, 알려진 단순화).
constexpr uint16_t k_reg_net_config_mac = 0x14;

constexpr uint8_t k_status_acknowledge = 1;
constexpr uint8_t k_status_driver = 2;
constexpr uint8_t k_status_driver_ok = 4;

constexpr uint16_t k_desc_flag_write = 2;  // 디바이스가 이 디스크립터에 쓴다.

constexpr uint16_t k_queue_rx = 0;
constexpr uint16_t k_queue_tx = 1;

// --- netsrv와의 IPC 프로토콜(이 드라이버가 서버). ---
constexpr uint32_t k_op_send_frame = 1;  // regs[0]=길이, pages[0]=프레임(4096 고정, 앞 길이만 유효). 응답 regs[0]=상태(0=성공).
constexpr uint32_t k_op_recv_frame = 2;  // 인자 없음. 응답 regs[0]=상태(0=수신, 1=타임아웃), regs[1]=길이, pages[0]=프레임.
constexpr uint32_t k_op_get_mac = 3;     // 응답 regs[0]의 하위 6바이트=MAC(리틀엔디안 memcpy).

constexpr uint64_t k_page_size = 4096;

constexpr uint64_t k_net_hdr_size = 10;      // virtio_net_hdr(legacy, MRG_RXBUF/CSUM 미협상 — 전부 0).
constexpr uint64_t k_max_frame_size = 1514;  // 표준 이더넷 프레임(FCS 제외).
constexpr uint64_t k_page_size_c = 4096;
// 2048(1536이 아니라) — RX 버퍼 영역 전체(count*size)가 4096의
// 배수여야 그 다음에 오는 TX vring이 페이지 정렬된 주소에서
// 시작한다(virtio QueueAddress 레지스터는 페이지 프레임 번호,
// 즉 물리주소/4096을 저장하므로 정렬이 깨지면 잘린 값이 완전히
// 다른 위치를 가리킨다 — 실제로 이 값 때문에 TX가 조용히
// 멈추는 버그를 겪었다, 2026-09-10). k_max_frame_size+k_net_hdr_size
// =1524는 2048에 여유 있게 들어간다.
constexpr uint64_t k_buffer_size = 2048;
constexpr uint32_t k_rx_buffer_count = 4;  // 왕복 하나에 필요한 것보다 넉넉히(방송 잡음 대비).

constexpr uint64_t k_vring_budget = 0x4000;  // virtio-blk와 같은 여유(queue_size<=256 가정), 이미 4096의 배수.

// DMA 레이아웃(하나의 sys_alloc_dma_buffer 안에 RX/TX 전부) —
// [rx_vring][rx_buffers x k_rx_buffer_count][tx_vring][tx_buffer].
// vring 두 개(rx_vring_off/tx_vring_off) 모두 반드시 4096의 배수
// 오프셋에서 시작해야 한다(위 k_buffer_size 주석 참고) — 아래
// static_assert가 이 라인 전체가 실수로 다시 깨지는 것을 막는다.
constexpr uint64_t k_rx_vring_off = 0;
constexpr uint64_t k_rx_buffers_off = k_rx_vring_off + k_vring_budget;
constexpr uint64_t k_tx_vring_off = k_rx_buffers_off + k_rx_buffer_count * k_buffer_size;
constexpr uint64_t k_tx_buffer_off = k_tx_vring_off + k_vring_budget;
constexpr uint64_t k_dma_total_size = k_tx_buffer_off + k_buffer_size;
constexpr uint32_t k_dma_buffer_order = 5;  // 4KiB<<5 = 128KiB, 위를 여유 있게 담는다.

static_assert(k_rx_vring_off % k_page_size_c == 0, "rx vring must be page-aligned");
static_assert(k_tx_vring_off % k_page_size_c == 0, "tx vring must be page-aligned");

uint16_t g_rx_queue_size = 0;
uint16_t g_tx_queue_size = 0;
uint16_t g_rx_avail_idx = 0;
uint16_t g_tx_avail_idx = 0;
uint16_t g_rx_used_seen = 0;  // 마지막으로 소비를 확인한 used_idx.
uint8_t* g_dma_virt = nullptr;
uint64_t g_dma_phys = 0;

alignas(k_page_size) uint8_t g_frame_scratch[k_page_size] = {};

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
uint8_t in8(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
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

struct desc_entry {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

// compute_layout()이 반환하는 desc_off/avail_off/used_off는 그 큐
// 자신의 vring 시작(각각 k_rx_vring_off/k_tx_vring_off)을 기준으로
// 한 **상대** 오프셋이다 — g_dma_virt(DMA 버퍼 전체의 시작)에
// 곧바로 더하면 안 되고, 반드시 그 큐의 vring 베이스를 먼저 더해야
// 한다. RX는 k_rx_vring_off==0이라 이 실수를 해도 우연히 맞는
// 값이 나오지만, TX는 k_tx_vring_off!=0이라 즉시 드러난다 — 실제로
// TX만 조용히 멈추는 버그로 나타났다(디바이스는 자신이 정확히
// 안내받은 물리주소(queue_address 레지스터)만 보는데, 드라이버가
// avail 링 갱신은 그 주소가 아니라 DMA 버퍼 맨 앞(RX의 자리)에
// 써 버렸으니 디바이스 쪽에서는 아무 것도 새로 온 것처럼 보이지
// 않았다, 2026-09-10).
vring_layout g_rx_layout;
vring_layout g_tx_layout;

// RX 디스크립터 slot(0..k_rx_buffer_count-1)을 다시 avail 링에
// 올린다 — 최초 채움과, 소비된 버퍼 재활용에 둘 다 쓴다.
void post_rx_buffer(uint32_t slot) {
    auto* desc = reinterpret_cast<desc_entry*>(g_dma_virt + k_rx_vring_off + g_rx_layout.desc_off);
    uint64_t buf_off = k_rx_buffers_off + slot * k_buffer_size;
    desc[slot] = {g_dma_phys + buf_off, static_cast<uint32_t>(k_buffer_size), k_desc_flag_write, 0};

    auto* avail_flags =
        reinterpret_cast<uint16_t*>(g_dma_virt + k_rx_vring_off + g_rx_layout.avail_off);
    auto* avail_idx_ptr = avail_flags + 1;
    auto* avail_ring = avail_flags + 2;
    uint16_t ring_slot = static_cast<uint16_t>(g_rx_avail_idx % g_rx_queue_size);
    avail_ring[ring_slot] = static_cast<uint16_t>(slot);
    ++g_rx_avail_idx;
    *avail_idx_ptr = g_rx_avail_idx;
}

bool init(uint16_t io_base) {
    out8(io_base + k_reg_device_status, 0);
    out8(io_base + k_reg_device_status, k_status_acknowledge);
    out8(io_base + k_reg_device_status, k_status_acknowledge | k_status_driver);

    (void)in32(io_base + k_reg_host_features);
    out32(io_base + k_reg_guest_features, 0);

    out16(io_base + k_reg_queue_select, k_queue_rx);
    g_rx_queue_size = in16(io_base + k_reg_queue_size);
    if (g_rx_queue_size == 0) {
        return false;
    }
    g_rx_layout = compute_layout(g_rx_queue_size);
    if (g_rx_layout.total_size > k_vring_budget) {
        return false;
    }

    out16(io_base + k_reg_queue_select, k_queue_tx);
    g_tx_queue_size = in16(io_base + k_reg_queue_size);
    if (g_tx_queue_size == 0) {
        return false;
    }
    g_tx_layout = compute_layout(g_tx_queue_size);
    if (g_tx_layout.total_size > k_vring_budget) {
        return false;
    }

    for (uint64_t i = 0; i < k_dma_total_size; ++i) {
        g_dma_virt[i] = 0;
    }

    out16(io_base + k_reg_queue_select, k_queue_rx);
    out32(io_base + k_reg_queue_address,
          static_cast<uint32_t>((g_dma_phys + k_rx_vring_off) / k_page_size));
    out16(io_base + k_reg_queue_select, k_queue_tx);
    out32(io_base + k_reg_queue_address,
          static_cast<uint32_t>((g_dma_phys + k_tx_vring_off) / k_page_size));

    out8(io_base + k_reg_device_status,
         k_status_acknowledge | k_status_driver | k_status_driver_ok);

    g_rx_avail_idx = 0;
    g_tx_avail_idx = 0;
    g_rx_used_seen = 0;

    for (uint32_t i = 0; i < k_rx_buffer_count; ++i) {
        post_rx_buffer(i);
    }
    out16(io_base + k_reg_queue_notify, k_queue_rx);

    return true;
}

bool do_send_frame(uint16_t io_base, uint64_t length) {
    if (length == 0 || length > k_max_frame_size) {
        return false;
    }
    // g_dma_virt+k_tx_buffer_off의 앞 k_net_hdr_size바이트는 init()의
    // 0-채움 이후 아무도 건드리지 않는다 — GSO/체크섬 오프로드를
    // 전혀 안 쓰므로(guest_features=0) 항상 0이면 충분하다(VirtIO
    // 1.0 §5.1.6.1).
    auto* desc = reinterpret_cast<desc_entry*>(g_dma_virt + k_tx_vring_off + g_tx_layout.desc_off);
    desc[0] = {g_dma_phys + k_tx_buffer_off, static_cast<uint32_t>(k_net_hdr_size + length), 0, 0};

    auto* avail_flags =
        reinterpret_cast<uint16_t*>(g_dma_virt + k_tx_vring_off + g_tx_layout.avail_off);
    auto* avail_idx_ptr = avail_flags + 1;
    auto* avail_ring = avail_flags + 2;
    uint16_t slot = static_cast<uint16_t>(g_tx_avail_idx % g_tx_queue_size);
    avail_ring[slot] = 0;  // 디스크립터 0 하나만 재사용(blk와 같은 정신).
    ++g_tx_avail_idx;
    *avail_idx_ptr = g_tx_avail_idx;

    auto* used_idx_ptr =
        reinterpret_cast<volatile uint16_t*>(g_dma_virt + k_tx_vring_off + g_tx_layout.used_off + 2);
    uint16_t used_before = *used_idx_ptr;

    out16(io_base + k_reg_queue_notify, k_queue_tx);

    constexpr uint64_t k_poll_iterations = 100'000'000ull;
    for (uint64_t i = 0; i < k_poll_iterations; ++i) {
        if (*used_idx_ptr != used_before) {
            return true;
        }
    }
    return false;
}

// out_data는 최소 k_max_frame_size바이트. 반환: 실제 프레임 길이
// (0=이번 폴링 예산 안에 아무것도 안 옴 — netsrv가 필요하면 다시
// 호출해 누적 대기한다).
uint64_t do_recv_frame(const uint8_t* /*unused*/, uint8_t* out_data) {
    auto* used_idx_ptr =
        reinterpret_cast<volatile uint16_t*>(g_dma_virt + k_rx_vring_off + g_rx_layout.used_off + 2);

    constexpr uint64_t k_poll_iterations = 20'000'000ull;
    bool got = false;
    for (uint64_t i = 0; i < k_poll_iterations; ++i) {
        if (*used_idx_ptr != g_rx_used_seen) {
            got = true;
            break;
        }
    }
    if (!got) {
        return 0;
    }

    // used ring 엔트리: {id(u32), len(u32)} 배열, used_off+4부터
    // (앞 4바이트는 flags(2)+idx(2)).
    uint16_t ring_slot = static_cast<uint16_t>(g_rx_used_seen % g_rx_queue_size);
    auto* used_ring =
        reinterpret_cast<volatile uint32_t*>(g_dma_virt + k_rx_vring_off + g_rx_layout.used_off + 4);
    uint32_t desc_id = used_ring[ring_slot * 2];
    uint32_t total_len = used_ring[ring_slot * 2 + 1];
    ++g_rx_used_seen;

    uint64_t frame_len = 0;
    if (total_len > k_net_hdr_size) {
        frame_len = total_len - k_net_hdr_size;
        if (frame_len > k_max_frame_size) {
            frame_len = k_max_frame_size;
        }
        const uint8_t* src = g_dma_virt + k_rx_buffers_off + desc_id * k_buffer_size + k_net_hdr_size;
        for (uint64_t i = 0; i < frame_len; ++i) {
            out_data[i] = src[i];
        }
    }

    // 다 읽었으니 같은 슬롯을 즉시 재활용한다 — 그러지 않으면
    // k_rx_buffer_count번 받은 뒤로는 더 받을 곳이 없어진다.
    post_rx_buffer(desc_id);
    // (io_base는 do_recv_frame 호출자가 이미 알고 있으므로 여기서
    // 별도로 notify하지 않는다 — 호출자가 바로 이어서 notify한다.)

    return frame_len;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    uapi::message req{};
    req.label = k_op_register_driver;
    req.regs[0] = k_match_mode_vendor_device;
    req.regs[1] = k_virtio_net_vendor_device;
    req.regs[2] = 0;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_devmgr_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));

    if (reply.regs[0] != k_status_ok || (reply.regs[3] & k_is_io_bit) == 0) {
        debug_log("[virtio-net] no I/O-BAR virtio-net device registered by devmgr\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    auto io_base = static_cast<uint16_t>(reply.regs[1]);
    debug_log_hex("[virtio-net] io_base=", io_base);

    do_syscall(uapi::k_syscall_io_activate, io_base, 0x20, 0);

    uapi::dma_buffer_result dma{};
    uint64_t alloc_err = do_syscall(uapi::k_syscall_alloc_dma_buffer,
                                     reinterpret_cast<uint64_t>(&dma), k_dma_buffer_order, 0);
    if (alloc_err != 0) {
        debug_log("[virtio-net] alloc_dma_buffer failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    g_dma_virt = reinterpret_cast<uint8_t*>(dma.virt_addr);
    g_dma_phys = dma.phys_addr;

    if (!init(io_base)) {
        debug_log("[virtio-net] init failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    debug_log("[virtio-net] init ok\n");

    uint8_t mac[6];
    for (uint32_t i = 0; i < 6; ++i) {
        mac[i] = in8(static_cast<uint16_t>(io_base + k_reg_net_config_mac + i));
    }

    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0) {
            out.label = in.label;
            if (in.label == k_op_send_frame) {
                uint64_t length = in.regs[0];
                if (in.page_count == 1 && length > 0 && length <= k_max_frame_size) {
                    auto* src = reinterpret_cast<const uint8_t*>(in.pages[0].vaddr);
                    uint8_t* dst = g_dma_virt + k_tx_buffer_off + k_net_hdr_size;
                    for (uint64_t i = 0; i < length; ++i) {
                        dst[i] = src[i];
                    }
                    out.regs[0] = do_send_frame(io_base, length) ? 0 : 1;
                } else {
                    out.regs[0] = 1;
                }
            } else if (in.label == k_op_recv_frame) {
                uint8_t frame_buf[k_max_frame_size];
                uint64_t len = do_recv_frame(nullptr, frame_buf);
                if (len > 0) {
                    for (uint64_t i = 0; i < len; ++i) {
                        g_frame_scratch[i] = frame_buf[i];
                    }
                    out16(io_base + k_reg_queue_notify, k_queue_rx);
                    out.regs[0] = 0;
                    out.regs[1] = len;
                    out.page_count = 1;
                    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_frame_scratch);
                    out.pages[0].length = k_page_size;
                    out.pages[0].mode = uapi::transfer_mode::copy;
                } else {
                    out.regs[0] = 1;
                }
            } else if (in.label == k_op_get_mac) {
                uint64_t reg = 0;
                __builtin_memcpy(&reg, mac, 6);
                out.regs[0] = reg;
            }
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}
