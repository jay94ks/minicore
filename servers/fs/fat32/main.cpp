// servers/fs/fat32/main.cpp — FAT32 FS 서버(읽기전용)
// (docs/plan/system-servers-bringup.md §M16, boot-and-drivers.md
// ADR-057).
//
// devmgr에 등록해(vendor:device=0x1af4:0x1001, servers/drivers/
// virtio-blk/main.cpp와 같은 레거시 virtio-blk 프로토콜 — 코드는
// 독립적으로 다시 구현한다, 같은 이유는 그 파일 상단 주석 참고) 받은
// 자기 자신의 virtio-blk-pci 장치를 마운트한다. v1 범위(ADR-057이
// FAT32를 M16 1순위로 확정할 때 이미 전제한 것과 filesystem.md
// ADR-128의 fs_node_id 방향에 맞춰 이 라운드가 좁힌 것):
//   - 읽기전용 — OP_WRITE 없음.
//   - 루트 디렉터리 평평한 스캔만(하위 디렉터리 진입 없음) — memfs와
//     같은 전제, LFN(long file name) 엔트리는 건너뛴다(8.3 짧은
//     이름만 인식).
//   - 파일 하나당 최대 1페이지(4096바이트)만 읽어 준다(fs-protocol.md
//     v2 §2.3의 pages[] 슬롯 예산).
#include <uapi.hpp>

namespace kernsrv::fs::fat32 {

namespace {

constexpr uint32_t k_devmgr_handle = 2;  // depends=devmgr.

constexpr uint32_t k_op_register_driver = 1;
constexpr uint64_t k_match_mode_vendor_device = 0;
constexpr uint64_t k_status_ok = 0;
constexpr uint64_t k_is_io_bit = 1ull << 32;
constexpr uint64_t k_virtio_vendor_device = (0x1AF4ull << 16) | 0x1001ull;

// --- legacy virtio-blk 레지스터(io_base 기준) — servers/drivers/
// virtio-blk/main.cpp와 동일한 레이아웃, 이 파일은 읽기(VIRTIO_BLK_T_IN)
// 만 쓴다. ---
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
constexpr uint16_t k_desc_flag_write = 2;

constexpr uint32_t k_virtio_blk_t_in = 0;

constexpr uint64_t k_sector_size = 512;

constexpr uint64_t k_vring_region_size = 0x4000;
constexpr uint64_t k_req_header_offset = k_vring_region_size;
constexpr uint64_t k_req_header_size = 16;
constexpr uint64_t k_data_offset = k_req_header_offset + k_req_header_size;
constexpr uint64_t k_status_offset = k_data_offset + k_sector_size;
constexpr uint32_t k_dma_buffer_order = 5;  // 128KiB.

uint16_t g_queue_size = 0;
uint16_t g_avail_idx = 0;
uint16_t g_io_base = 0;
uint8_t* g_dma_virt = nullptr;
uint64_t g_dma_phys = 0;

struct vring_layout {
    uint64_t desc_off;
    uint64_t avail_off;
    uint64_t used_off;
    uint64_t total_size;
};
vring_layout g_layout;

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

bool virtio_init() {
    out8(g_io_base + k_reg_device_status, 0);
    out8(g_io_base + k_reg_device_status, k_status_acknowledge);
    out8(g_io_base + k_reg_device_status, k_status_acknowledge | k_status_driver);
    (void)in32(g_io_base + k_reg_host_features);
    out32(g_io_base + k_reg_guest_features, 0);

    out16(g_io_base + k_reg_queue_select, 0);
    uint16_t queue_size = in16(g_io_base + k_reg_queue_size);
    if (queue_size == 0) {
        return false;
    }
    vring_layout layout = compute_layout(queue_size);
    if (layout.total_size > k_vring_region_size) {
        return false;
    }
    for (uint64_t i = 0; i < k_vring_region_size; ++i) {
        g_dma_virt[i] = 0;
    }
    out32(g_io_base + k_reg_queue_address, static_cast<uint32_t>(g_dma_phys / k_page_size));
    out8(g_io_base + k_reg_device_status,
         k_status_acknowledge | k_status_driver | k_status_driver_ok);

    g_layout = layout;
    g_queue_size = queue_size;
    g_avail_idx = 0;
    return true;
}

// 섹터 하나를 읽어 g_dma_virt+k_data_offset에 채운다(호출자가 그 뒤
// 확인). servers/drivers/virtio-blk/main.cpp::do_request와 같은
// 프로토콜(읽기 전용으로 좁혔을 뿐).
bool read_sector(uint64_t sector) {
    auto* req_type = reinterpret_cast<uint32_t*>(g_dma_virt + k_req_header_offset);
    req_type[0] = k_virtio_blk_t_in;
    req_type[1] = 0;
    auto* req_sector = reinterpret_cast<uint64_t*>(g_dma_virt + k_req_header_offset + 8);
    *req_sector = sector;
    g_dma_virt[k_status_offset] = 0xFF;

    struct desc_entry {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    };
    auto* desc = reinterpret_cast<desc_entry*>(g_dma_virt + g_layout.desc_off);
    desc[0] = {g_dma_phys + k_req_header_offset, static_cast<uint32_t>(k_req_header_size),
               k_desc_flag_next, 1};
    desc[1] = {g_dma_phys + k_data_offset, static_cast<uint32_t>(k_sector_size),
               static_cast<uint16_t>(k_desc_flag_next | k_desc_flag_write), 2};
    desc[2] = {g_dma_phys + k_status_offset, 1, k_desc_flag_write, 0};

    auto* avail_flags = reinterpret_cast<uint16_t*>(g_dma_virt + g_layout.avail_off);
    auto* avail_idx_ptr = avail_flags + 1;
    auto* avail_ring = avail_flags + 2;
    avail_flags[0] = 0;
    uint16_t slot = static_cast<uint16_t>(g_avail_idx % g_queue_size);
    avail_ring[slot] = 0;
    ++g_avail_idx;
    *avail_idx_ptr = g_avail_idx;

    auto* used_idx_ptr = reinterpret_cast<volatile uint16_t*>(g_dma_virt + g_layout.used_off + 2);
    uint16_t used_idx_before = *used_idx_ptr;
    out16(g_io_base + k_reg_queue_notify, 0);

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
    return g_dma_virt[k_status_offset] == 0;
}

// ---------- FAT32 ----------
uint16_t bytes_per_sector = 0;
uint8_t sectors_per_cluster = 0;
uint16_t reserved_sector_count = 0;
uint8_t num_fats = 0;
uint32_t sectors_per_fat32 = 0;
uint32_t root_cluster = 0;
uint64_t first_fat_sector = 0;
uint64_t first_data_sector = 0;

bool fat32_mount() {
    if (!read_sector(0)) {
        return false;
    }
    const uint8_t* bpb = g_dma_virt + k_data_offset;
    bytes_per_sector = static_cast<uint16_t>(bpb[11] | (static_cast<uint16_t>(bpb[12]) << 8));
    sectors_per_cluster = bpb[13];
    reserved_sector_count = static_cast<uint16_t>(bpb[14] | (static_cast<uint16_t>(bpb[15]) << 8));
    num_fats = bpb[16];
    __builtin_memcpy(&sectors_per_fat32, bpb + 36, 4);
    __builtin_memcpy(&root_cluster, bpb + 44, 4);

    if (bytes_per_sector != k_sector_size || sectors_per_cluster == 0 || num_fats == 0 ||
        sectors_per_fat32 == 0 || root_cluster < 2) {
        return false;  // FAT32가 아니거나 이 드라이버가 지지하지 않는 변형(fs-protocol.md §3 MOUNT_ERROR).
    }
    first_fat_sector = reserved_sector_count;
    first_data_sector = reserved_sector_count + static_cast<uint64_t>(num_fats) * sectors_per_fat32;
    return true;
}

uint64_t cluster_to_sector(uint32_t cluster) {
    return first_data_sector + static_cast<uint64_t>(cluster - 2) * sectors_per_cluster;
}

// FAT[cluster](4바이트, 상위 4비트는 예약이라 마스크)를 읽는다.
uint32_t next_cluster(uint32_t cluster) {
    uint64_t fat_byte_off = static_cast<uint64_t>(cluster) * 4;
    uint64_t fat_sector = first_fat_sector + fat_byte_off / k_sector_size;
    uint64_t offset_in_sector = fat_byte_off % k_sector_size;
    if (!read_sector(fat_sector)) {
        return 0x0FFFFFFF;  // 오류 시 체인이 끝난 것처럼 취급(안전한 폴백).
    }
    uint32_t v;
    __builtin_memcpy(&v, g_dma_virt + k_data_offset + offset_in_sector, 4);
    return v & 0x0FFFFFFF;
}

bool is_end_of_chain(uint32_t cluster) { return cluster >= 0x0FFFFFF8; }

// name83(11바이트, 공백 패딩)을 "NAME.EXT" 형태로 복원한다(확장자
// 없으면 점도 없음). 대문자 그대로 돌려준다 — 비교하는 쪽에서
// 대문자로 맞춰 비교한다.
void format_short_name(const uint8_t* name83, char* out, uint64_t out_size) {
    uint64_t pos = 0;
    for (int i = 0; i < 8 && name83[i] != ' '; ++i) {
        if (pos + 1 < out_size) {
            out[pos++] = static_cast<char>(name83[i]);
        }
    }
    if (name83[8] != ' ') {
        if (pos + 1 < out_size) {
            out[pos++] = '.';
        }
        for (int i = 8; i < 11 && name83[i] != ' '; ++i) {
            if (pos + 1 < out_size) {
                out[pos++] = static_cast<char>(name83[i]);
            }
        }
    }
    out[pos] = '\0';
}

char to_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool name_equals_ci(const char* a, const char* b) {
    uint64_t i = 0;
    for (;; ++i) {
        char ca = to_upper(a[i]);
        char cb = to_upper(b[i]);
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
}

constexpr uint8_t k_attr_long_name = 0x0F;
constexpr uint8_t k_attr_volume_id = 0x08;
constexpr uint8_t k_attr_directory = 0x10;

// 루트 디렉터리(root_cluster에서 시작하는 체인)를 평평하게 스캔해
// name과 일치하는 8.3 엔트리를 찾는다 — LFN/볼륨 라벨/서브디렉터리는
// 건너뛴다(이 드라이버의 v1 범위, 상단 주석 참고).
bool find_root_entry(const char* name, uint32_t& out_first_cluster, uint32_t& out_size) {
    uint32_t cluster = root_cluster;
    while (!is_end_of_chain(cluster)) {
        for (uint8_t s = 0; s < sectors_per_cluster; ++s) {
            uint64_t sector = cluster_to_sector(cluster) + s;
            if (!read_sector(sector)) {
                return false;
            }
            const uint8_t* buf = g_dma_virt + k_data_offset;
            for (uint32_t off = 0; off + 32 <= k_sector_size; off += 32) {
                const uint8_t* entry = buf + off;
                if (entry[0] == 0x00) {
                    return false;  // 디렉터리 끝(더 이상 엔트리 없음).
                }
                if (entry[0] == 0xE5) {
                    continue;  // 삭제된 엔트리.
                }
                uint8_t attr = entry[11];
                if (attr == k_attr_long_name || (attr & k_attr_volume_id) != 0 ||
                    (attr & k_attr_directory) != 0) {
                    continue;
                }
                char entry_name[13];
                format_short_name(entry, entry_name, sizeof(entry_name));
                if (name_equals_ci(entry_name, name)) {
                    uint16_t hi = static_cast<uint16_t>(entry[20] | (static_cast<uint16_t>(entry[21]) << 8));
                    uint16_t lo = static_cast<uint16_t>(entry[26] | (static_cast<uint16_t>(entry[27]) << 8));
                    out_first_cluster = (static_cast<uint32_t>(hi) << 16) | lo;
                    __builtin_memcpy(&out_size, entry + 28, 4);
                    return true;
                }
            }
        }
        cluster = next_cluster(cluster);
    }
    return false;
}

// first_cluster에서 시작하는 체인의 앞부분 최대 4096바이트를
// out(4096바이트 버퍼)에 읽어 담는다. 반환값은 실제로 읽은 바이트 수.
uint32_t read_file_first_page(uint32_t first_cluster, uint32_t file_size, uint8_t* out) {
    uint32_t to_read = (file_size < k_page_size) ? file_size : k_page_size;
    for (uint32_t i = 0; i < k_page_size; ++i) {
        out[i] = 0;
    }
    if (to_read == 0 || is_end_of_chain(first_cluster)) {
        return 0;
    }
    uint32_t bytes_done = 0;
    uint32_t cluster = first_cluster;
    while (bytes_done < to_read && !is_end_of_chain(cluster)) {
        for (uint8_t s = 0; s < sectors_per_cluster && bytes_done < to_read; ++s) {
            if (!read_sector(cluster_to_sector(cluster) + s)) {
                return bytes_done;
            }
            uint32_t chunk = to_read - bytes_done;
            if (chunk > k_sector_size) {
                chunk = k_sector_size;
            }
            // 가변 길이 __builtin_memcpy는 이 freestanding 빌드에서
            // 실제 memcpy 심볼 호출로 낮춰져 링크에 실패한다(크기가
            // 컴파일 타임 상수가 아닐 때) — 다른 드라이버들과 같은
            // 이유로 손으로 바이트 루프를 쓴다.
            for (uint32_t i = 0; i < chunk; ++i) {
                out[bytes_done + i] = (g_dma_virt + k_data_offset)[i];
            }
            bytes_done += chunk;
        }
        cluster = next_cluster(cluster);
    }
    return bytes_done;
}

// ---------- fs-protocol.md v2 오퍼레이션 ----------
constexpr uint32_t k_own_endpoint_handle = 1;
constexpr uint32_t k_op_open = 1;
constexpr uint32_t k_op_read = 3;
constexpr uint64_t k_fs_status_ok = 0;
constexpr uint64_t k_fs_status_not_found = 1;

constexpr uint32_t k_max_open_files = 8;
struct open_instance {
    bool used = false;
    uint32_t first_cluster = 0;
    uint32_t size = 0;
};
open_instance g_opens[k_max_open_files];

alignas(k_page_size) uint8_t g_read_scratch[k_page_size] = {};

void handle_open(const uapi::message& in, uapi::message& out) {
    char path[33];
    __builtin_memcpy(path, in.regs, 32);
    path[32] = '\0';

    uint32_t first_cluster = 0;
    uint32_t size = 0;
    if (!find_root_entry(path, first_cluster, size)) {
        out.regs[0] = 0;
        out.regs[1] = k_fs_status_not_found;
        return;
    }
    for (uint32_t i = 0; i < k_max_open_files; ++i) {
        if (!g_opens[i].used) {
            g_opens[i].used = true;
            g_opens[i].first_cluster = first_cluster;
            g_opens[i].size = size;
            out.regs[0] = i + 1;
            out.regs[1] = k_fs_status_ok;
            return;
        }
    }
    out.regs[0] = 0;
    out.regs[1] = k_fs_status_not_found;
}

void handle_read(const uapi::message& in, uapi::message& out) {
    uint32_t open_id = static_cast<uint32_t>(in.regs[0]);
    if (open_id == 0 || open_id > k_max_open_files || !g_opens[open_id - 1].used) {
        out.regs[0] = 0;
        out.regs[1] = k_fs_status_not_found;
        return;
    }
    open_instance& f = g_opens[open_id - 1];
    uint32_t n = read_file_first_page(f.first_cluster, f.size, g_read_scratch);
    out.page_count = 1;
    out.pages[0].vaddr = reinterpret_cast<uint64_t>(g_read_scratch);
    out.pages[0].length = k_page_size;
    out.pages[0].mode = uapi::transfer_mode::copy;
    out.regs[0] = n;
    out.regs[1] = k_fs_status_ok;
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
        debug_log("[fat32] no I/O-BAR virtio-blk device registered by devmgr\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    g_io_base = static_cast<uint16_t>(reply.regs[1]);
    debug_log_hex("[fat32] io_base=", g_io_base);

    do_syscall(uapi::k_syscall_io_activate, g_io_base, 0x20, 0);

    uapi::dma_buffer_result dma{};
    uint64_t alloc_err = do_syscall(uapi::k_syscall_alloc_dma_buffer,
                                     reinterpret_cast<uint64_t>(&dma), k_dma_buffer_order, 0);
    if (alloc_err != 0) {
        debug_log("[fat32] alloc_dma_buffer failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    g_dma_virt = reinterpret_cast<uint8_t*>(dma.virt_addr);
    g_dma_phys = dma.phys_addr;

    if (!virtio_init()) {
        debug_log("[fat32] virtio init failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }

    bool mounted = fat32_mount();
    debug_log_hex("[fat32] mount ok=", mounted ? 1 : 0);
    if (mounted) {
        debug_log_hex("[fat32] root_cluster=", root_cluster);
        debug_log_hex("[fat32] sectors_per_cluster=", sectors_per_cluster);
    }

    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0 && mounted) {
            out.label = in.label;
            switch (in.label) {
                case k_op_open:
                    handle_open(in, out);
                    break;
                case k_op_read:
                    handle_read(in, out);
                    break;
                default:
                    out.regs[1] = k_fs_status_not_found;
                    break;
            }
        } else {
            out.label = in.label;
            out.regs[1] = k_fs_status_not_found;
        }
        do_syscall(uapi::k_syscall_ipc_reply, reinterpret_cast<uint64_t>(&out), 0, 0);
    }
}

}  // namespace kernsrv::fs::fat32
