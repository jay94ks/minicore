// servers/fs/ext4/main.cpp — ext4 FS 서버(읽기전용, v1)
// (docs/plan/system-servers-bringup.md §M16, boot-and-drivers.md
// ADR-129).
//
// devmgr에 등록해(vendor:device=0x1af4:0x1001) 받은 자기 자신의
// virtio-blk-pci 장치를 마운트한다. virtio-blk 레지스터 프로토콜은
// servers/fs/fat32/main.cpp와 완전히 같은 코드를 독립적으로 다시
// 구현한 것이다(공유 라이브러리가 없다 — 다른 드라이버들과 같은
// 이유). ADR-129가 이미 확정한 v1 범위:
//   - extent 트리 기반 블록 매핑만(depth==0, 단일 레벨) — 레거시
//     간접 블록 포인터, 다단계 extent 트리는 범위 밖.
//   - 저널이 있는 이미지는 마운트를 거부한다(재생을 구현하지
//     않았으므로 — clean/dirty를 구분하는 대신 "저널 자체가 있으면
//     거부"로 더 단순하게 판단한다, dirty 오탐이 없으므로 안전).
//   - htree 인덱스는 안 쓴다 — 디렉터리 항목은 inode==0(가짜/인덱스
//     엔트리)을 건너뛰고 나머지를 선형 스캔한다.
//   - feature_incompat/feature_ro_compat 화이트리스트 검사 — 64bit/
//     metadata_csum/encrypt 등을 요구하는 볼륨은 마운트 거부.
//   - 단일 블록 그룹만 지지한다(그룹 디스크립터 0번만 읽는다) —
//     이 라운드의 작은 테스트 이미지가 항상 이 형태다.
//   - 루트 디렉터리 평평한 스캔만(하위 디렉터리 진입 없음, memfs/
//     fat32와 같은 전제), 파일 하나당 최대 1페이지(4096바이트).
#include <uapi.hpp>

namespace {

constexpr uint32_t k_devmgr_handle = 2;  // depends=devmgr.

constexpr uint32_t k_op_register_driver = 1;
constexpr uint64_t k_match_mode_vendor_device = 0;
constexpr uint64_t k_status_ok = 0;
constexpr uint64_t k_is_io_bit = 1ull << 32;
constexpr uint64_t k_virtio_vendor_device = (0x1AF4ull << 16) | 0x1001ull;

// --- legacy virtio-blk(servers/fs/fat32/main.cpp와 동일, 상단 주석 참고). ---
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
constexpr uint32_t k_dma_buffer_order = 5;

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

// byte_off에서 len바이트를 out에 읽어 담는다(섹터 경계를 넘으면 여러
// 번 read_sector). len은 512바이트 이하로 호출하는 쪽이 보장한다(이
// 파일은 한 번에 최대 ext4 block_size(<=4096, 실제로는 1024 전제)
// 만큼만 부르므로 필요하면 나눠 부른다).
bool read_bytes(uint64_t byte_off, uint32_t len, uint8_t* out) {
    uint32_t done = 0;
    while (done < len) {
        uint64_t cur = byte_off + done;
        uint64_t sector = cur / k_sector_size;
        uint32_t offset_in_sector = static_cast<uint32_t>(cur % k_sector_size);
        if (!read_sector(sector)) {
            return false;
        }
        uint32_t chunk = static_cast<uint32_t>(k_sector_size) - offset_in_sector;
        if (chunk > len - done) {
            chunk = len - done;
        }
        // 가변 길이 __builtin_memcpy는 이 freestanding 빌드에서 실제
        // memcpy 심볼 호출로 낮춰져 링크에 실패한다 — 손으로 바이트
        // 루프를 쓴다(servers/fs/fat32/main.cpp와 같은 이유).
        const uint8_t* src = g_dma_virt + k_data_offset + offset_in_sector;
        for (uint32_t i = 0; i < chunk; ++i) {
            out[done + i] = src[i];
        }
        done += chunk;
    }
    return true;
}

// ---------- ext4 ----------
uint32_t g_block_size = 0;
uint32_t g_inode_size = 0;
uint32_t g_first_data_block = 0;
uint32_t g_bg_inode_table = 0;

uint32_t u32_at(const uint8_t* p, uint32_t off) {
    uint32_t v;
    __builtin_memcpy(&v, p + off, 4);
    return v;
}
uint16_t u16_at(const uint8_t* p, uint32_t off) {
    uint16_t v;
    __builtin_memcpy(&v, p + off, 2);
    return v;
}

bool read_block(uint64_t block_num, uint8_t* out /* >= g_block_size바이트 */) {
    return read_bytes(block_num * g_block_size, g_block_size, out);
}

constexpr uint32_t k_ext4_extents_fl = 0x80000;
constexpr uint16_t k_ext_magic = 0xF30A;

// 최대 4096바이트짜리 스크래치(ext4 block_size가 이보다 크면 이
// 드라이버는 다루지 않는다 — v1 전제, 테스트 이미지는 1024).
alignas(4096) uint8_t g_block_scratch[4096];

// i_block(60바이트)의 extent 트리(depth==0만 지지)를 순회해 논리
// 블록 순서대로 물리 블록을 읽어 out[0..max_bytes)에 채운다. 반환값은
// 실제로 채운 바이트 수(뒤쪽은 0으로 남는다 — 호출 전에 out을 이미
// 0으로 초기화해 둬야 한다).
uint32_t read_extent_data(const uint8_t* i_block, uint32_t max_bytes, uint8_t* out) {
    if (u16_at(i_block, 0) != k_ext_magic || u16_at(i_block, 6) != 0) {
        return 0;  // extent 아님, 또는 다단계 트리(v1 범위 밖).
    }
    uint16_t entries = u16_at(i_block, 2);
    uint32_t filled = 0;
    for (uint16_t e = 0; e < entries; ++e) {
        const uint8_t* ent = i_block + 12 + e * 12;
        uint32_t ee_block = u32_at(ent, 0);
        uint16_t ee_len = u16_at(ent, 4);
        uint16_t ee_start_hi = u16_at(ent, 6);
        uint32_t ee_start_lo = u32_at(ent, 8);
        uint64_t phys_start = (static_cast<uint64_t>(ee_start_hi) << 32) | ee_start_lo;
        uint64_t logical_byte = static_cast<uint64_t>(ee_block) * g_block_size;
        for (uint16_t b = 0; b < ee_len; ++b) {
            uint64_t block_byte_off = logical_byte + static_cast<uint64_t>(b) * g_block_size;
            if (block_byte_off >= max_bytes) {
                break;
            }
            if (!read_block(phys_start + b, g_block_scratch)) {
                return filled;
            }
            uint32_t copy_len = g_block_size;
            if (block_byte_off + copy_len > max_bytes) {
                copy_len = static_cast<uint32_t>(max_bytes - block_byte_off);
            }
            for (uint32_t i = 0; i < copy_len; ++i) {
                out[block_byte_off + i] = g_block_scratch[i];
            }
            uint64_t end = block_byte_off + copy_len;
            if (end > filled) {
                filled = static_cast<uint32_t>(end);
            }
        }
    }
    return filled;
}

struct inode_info {
    bool ok = false;
    uint32_t size = 0;
    uint8_t i_block[60] = {};
};

inode_info read_inode(uint32_t inode_num) {
    inode_info info{};
    uint64_t off = static_cast<uint64_t>(g_bg_inode_table) * g_block_size +
                   static_cast<uint64_t>(inode_num - 1) * g_inode_size;
    uint8_t buf[128];
    if (!read_bytes(off, 128, buf)) {
        return info;
    }
    uint32_t flags = u32_at(buf, 32);
    if ((flags & k_ext4_extents_fl) == 0) {
        return info;  // extent 기반이 아님(v1 범위 밖).
    }
    info.size = u32_at(buf, 4);
    for (uint32_t i = 0; i < 60; ++i) {
        info.i_block[i] = buf[40 + i];
    }
    info.ok = true;
    return info;
}

bool mount_ok = false;

bool ext4_mount() {
    uint8_t sb[1024];
    if (!read_bytes(1024, 1024, sb)) {
        return false;
    }
    uint16_t magic = u16_at(sb, 56);
    if (magic != 0xEF53) {
        return false;
    }
    uint32_t feature_compat = u32_at(sb, 92);
    uint32_t feature_incompat = u32_at(sb, 96);
    uint32_t feature_ro_compat = u32_at(sb, 100);

    constexpr uint32_t k_compat_has_journal = 0x0004;
    if (feature_compat & k_compat_has_journal) {
        return false;  // 저널 재생 미구현 — 상단 주석대로 항상 거부.
    }
    constexpr uint32_t k_incompat_supported = 0x2 /*FILETYPE*/ | 0x40 /*EXTENTS*/ | 0x200 /*FLEX_BG*/;
    if (feature_incompat & ~k_incompat_supported) {
        return false;  // 64bit/recover/meta_bg 등 지원 밖 기능.
    }
    constexpr uint32_t k_rocompat_supported =
        0x1 /*SPARSE_SUPER*/ | 0x2 /*LARGE_FILE*/ | 0x8 /*HUGE_FILE*/ | 0x20 /*DIR_NLINK*/ |
        0x40 /*EXTRA_ISIZE*/;
    if (feature_ro_compat & ~k_rocompat_supported) {
        return false;  // metadata_csum/gdt_csum/project 등 지원 밖 기능.
    }

    uint32_t log_block_size = u32_at(sb, 24);
    g_block_size = 1024u << log_block_size;
    if (g_block_size > sizeof(g_block_scratch)) {
        return false;
    }
    g_inode_size = u16_at(sb, 88);
    g_first_data_block = u32_at(sb, 20);

    uint64_t gd_off = static_cast<uint64_t>(g_first_data_block + 1) * g_block_size;
    uint8_t gd[32];
    if (!read_bytes(gd_off, 32, gd)) {
        return false;
    }
    g_bg_inode_table = u32_at(gd, 8);
    // 단일 블록 그룹만 지지한다(상단 주석) — 그룹 0의 디스크립터만
    // 읽는다. 이 이상(여러 그룹)은 v1 범위 밖.
    return true;
}

constexpr uint32_t k_page_cap = 4096;

// 루트(inode 2)의 데이터 블록들을 순회하며 이름이 일치하는 dirent를
// 찾는다. inode==0(htree 인덱스/가짜 엔트리)은 건너뛴다(ADR-129).
bool find_root_entry(const char* name, uint32_t& out_inode_num) {
    inode_info root = read_inode(2);
    if (!root.ok) {
        return false;
    }
    uint8_t dirblock[4096];
    for (uint32_t i = 0; i < sizeof(dirblock); ++i) {
        dirblock[i] = 0;
    }
    uint32_t filled = read_extent_data(root.i_block, root.size, dirblock);
    uint32_t name_len_query = 0;
    while (name[name_len_query] != '\0') {
        ++name_len_query;
    }

    uint32_t off = 0;
    while (off + 8 <= filled) {
        uint32_t inode_num = u32_at(dirblock, off);
        uint16_t rec_len = u16_at(dirblock, off + 4);
        uint8_t name_len = dirblock[off + 6];
        if (rec_len == 0) {
            break;
        }
        if (inode_num != 0 && name_len == name_len_query) {
            bool same = true;
            for (uint8_t i = 0; i < name_len; ++i) {
                if (dirblock[off + 8 + i] != static_cast<uint8_t>(name[i])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                out_inode_num = inode_num;
                return true;
            }
        }
        off += rec_len;
    }
    return false;
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
    uint32_t inode_num = 0;
};
open_instance g_opens[k_max_open_files];

alignas(k_page_size) uint8_t g_read_scratch[k_page_size] = {};

void handle_open(const uapi::message& in, uapi::message& out) {
    char path[33];
    __builtin_memcpy(path, in.regs, 32);
    path[32] = '\0';

    uint32_t inode_num = 0;
    if (!find_root_entry(path, inode_num)) {
        out.regs[0] = 0;
        out.regs[1] = k_fs_status_not_found;
        return;
    }
    for (uint32_t i = 0; i < k_max_open_files; ++i) {
        if (!g_opens[i].used) {
            g_opens[i].used = true;
            g_opens[i].inode_num = inode_num;
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
    for (uint32_t i = 0; i < k_page_size; ++i) {
        g_read_scratch[i] = 0;
    }
    inode_info info = read_inode(g_opens[open_id - 1].inode_num);
    uint32_t max_bytes = (info.size < k_page_cap) ? info.size : k_page_cap;
    uint32_t n = info.ok ? read_extent_data(info.i_block, max_bytes, g_read_scratch) : 0;

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
        debug_log("[ext4] no I/O-BAR virtio-blk device registered by devmgr\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    g_io_base = static_cast<uint16_t>(reply.regs[1]);
    debug_log_hex("[ext4] io_base=", g_io_base);

    do_syscall(uapi::k_syscall_io_activate, g_io_base, 0x20, 0);

    uapi::dma_buffer_result dma{};
    uint64_t alloc_err = do_syscall(uapi::k_syscall_alloc_dma_buffer,
                                     reinterpret_cast<uint64_t>(&dma), k_dma_buffer_order, 0);
    if (alloc_err != 0) {
        debug_log("[ext4] alloc_dma_buffer failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }
    g_dma_virt = reinterpret_cast<uint8_t*>(dma.virt_addr);
    g_dma_phys = dma.phys_addr;

    if (!virtio_init()) {
        debug_log("[ext4] virtio init failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }

    mount_ok = ext4_mount();
    debug_log_hex("[ext4] mount ok=", mount_ok ? 1 : 0);
    if (mount_ok) {
        debug_log_hex("[ext4] block_size=", g_block_size);
    }

    for (;;) {
        uapi::message in{};
        uint64_t recv_err = do_syscall(uapi::k_syscall_ipc_recv, k_own_endpoint_handle,
                                        reinterpret_cast<uint64_t>(&in), 0);
        uapi::message out{};
        if (recv_err == 0 && mount_ok) {
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
