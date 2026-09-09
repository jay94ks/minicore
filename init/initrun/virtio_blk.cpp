// 최소 legacy virtio-blk 클라이언트 구현. virtio_blk.hpp 상단 주석
// 참고. 레지스터 오프셋/vring 레이아웃은 VirtIO 1.0 스펙 §4.1.4.8
// ("Legacy Interfaces: A Note on PCI Device Layout")과 §2.6.2("Legacy
// Interfaces: A Note on Virtqueue Layout")를 그대로 따른다.
#include "virtio_blk.hpp"

namespace virtio_blk {

namespace {

// --- legacy PCI I/O 레지스터 오프셋(io_base 기준) ---
constexpr uint16_t k_reg_host_features = 0x00;  // u32, RO
constexpr uint16_t k_reg_guest_features = 0x04;  // u32, RW
constexpr uint16_t k_reg_queue_address = 0x08;   // u32(PFN), RW
constexpr uint16_t k_reg_queue_size = 0x0C;      // u16, RO
constexpr uint16_t k_reg_queue_select = 0x0E;    // u16, RW
constexpr uint16_t k_reg_queue_notify = 0x10;    // u16, RW
constexpr uint16_t k_reg_device_status = 0x12;   // u8, RW
constexpr uint16_t k_reg_capacity = 0x14;         // u64, RO(virtio-blk 전용 설정 공간)

constexpr uint8_t k_status_acknowledge = 1;
constexpr uint8_t k_status_driver = 2;
constexpr uint8_t k_status_driver_ok = 4;

constexpr uint16_t k_desc_flag_next = 1;
constexpr uint16_t k_desc_flag_write = 2;  // 디바이스가 이 디스크립터에 쓴다(우리 관점: 읽기 응답).

constexpr uint32_t k_virtio_blk_t_in = 0;  // 읽기 요청.

// vring을 DMA 버퍼 맨 앞 16KiB(queue_size<=256까지 안전하게 담김,
// §근거는 아래 계산 참고)에, 그 뒤에 요청 헤더+상태 바이트+데이터를
// 둔다. 두 상수 다 dma 버퍼 안의 **오프셋**이다.
constexpr uint64_t k_vring_region_size = 0x4000;
constexpr uint64_t k_req_header_offset = k_vring_region_size;
constexpr uint64_t k_req_header_size = 16;  // {type:u32, reserved:u32, sector:u64}
constexpr uint64_t k_data_offset = k_req_header_offset + k_req_header_size;

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

// 레거시 vring 레이아웃(VirtIO 1.0 §2.6.2 "A Note on Virtqueue
// Layout") — desc[num](16바이트×num) + avail(flags+idx+ring[num]+
// used_event, 2바이트×(3+num)), 여기까지를 4096 경계로 올린 지점부터
// used(flags+idx+ring[num]의 {id,len}쌍(8바이트)+avail_event,
// 2바이트×3 + 8바이트×num)가 시작한다.
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

}  // namespace

bool init(uint16_t io_base, uint8_t* dma_virt, uint64_t dma_phys) {
    // 1) 리셋 + ACKNOWLEDGE + DRIVER.
    out8(io_base + k_reg_device_status, 0);
    out8(io_base + k_reg_device_status, k_status_acknowledge);
    out8(io_base + k_reg_device_status, k_status_acknowledge | k_status_driver);

    // 2) feature 협상 — 옵션 기능을 아무것도 요구하지 않는다(0).
    (void)in32(io_base + k_reg_host_features);
    out32(io_base + k_reg_guest_features, 0);

    // 3) 큐 0 선택 + 크기 확인.
    out16(io_base + k_reg_queue_select, 0);
    uint16_t queue_size = in16(io_base + k_reg_queue_size);
    if (queue_size == 0) {
        return false;
    }
    vring_layout layout = compute_layout(queue_size);
    if (layout.total_size > k_vring_region_size) {
        // queue_size가 예상보다 훨씬 커서(§근거: 16KiB는 queue_size<=340
        // 정도까지 안전하게 담긴다 — QEMU virtio-blk-pci는 128을 쓴다)
        // 이 최소 클라이언트가 가정한 상한을 넘었다.
        return false;
    }
    for (uint64_t i = 0; i < k_vring_region_size; ++i) {
        dma_virt[i] = 0;
    }

    // 4) QueueAddress = PFN(물리주소/4096).
    out32(io_base + k_reg_queue_address, static_cast<uint32_t>(dma_phys / k_page_size));

    // 5) DRIVER_OK.
    out8(io_base + k_reg_device_status,
         k_status_acknowledge | k_status_driver | k_status_driver_ok);

    return true;
}

bool read_all(uint16_t io_base, uint8_t* dma_virt, uint64_t dma_phys, uint64_t max_bytes,
              const uint8_t** out_data, uint64_t* out_len) {
    uint16_t queue_size = in16(io_base + k_reg_queue_size);
    if (queue_size == 0) {
        return false;
    }
    vring_layout layout = compute_layout(queue_size);

    // virtio-blk의 capacity는 legacy PCI I/O 공간 중 device-specific
    // config 영역(오프셋 0x14부터, u64)에 있다 — 별도 매핑 없이 같은
    // I/O 포트로 읽는다.
    uint32_t cap_lo = in32(io_base + k_reg_capacity);
    uint32_t cap_hi = in32(io_base + k_reg_capacity + 4);
    uint64_t capacity_sectors = (static_cast<uint64_t>(cap_hi) << 32) | cap_lo;

    uint64_t want_bytes = capacity_sectors * 512ull;
    if (want_bytes > max_bytes) {
        want_bytes = max_bytes & ~static_cast<uint64_t>(511);  // 섹터 경계로 내림.
    }
    if (want_bytes == 0) {
        return false;
    }

    // 요청 헤더 — VIRTIO_BLK_T_IN(읽기), sector=0(디스크 맨 앞부터).
    auto* req = reinterpret_cast<uint32_t*>(dma_virt + k_req_header_offset);
    req[0] = k_virtio_blk_t_in;
    req[1] = 0;  // reserved
    auto* req_sector = reinterpret_cast<uint64_t*>(dma_virt + k_req_header_offset + 8);
    *req_sector = 0;

    uint64_t data_offset = k_data_offset;
    uint64_t status_offset = data_offset + want_bytes;
    dma_virt[status_offset] = 0xFF;  // 디바이스가 덮어써야 할 자리 — 안 쓰였는지 확인용 sentinel.

    // 디스크립터 3개: 요청 헤더(디바이스가 읽음) → 데이터(디바이스가 씀) → 상태(디바이스가 씀).
    struct desc_entry {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    };
    auto* desc = reinterpret_cast<desc_entry*>(dma_virt + layout.desc_off);
    desc[0] = {dma_phys + k_req_header_offset, static_cast<uint32_t>(k_req_header_size),
               k_desc_flag_next, 1};
    desc[1] = {dma_phys + data_offset, static_cast<uint32_t>(want_bytes),
               k_desc_flag_next | k_desc_flag_write, 2};
    desc[2] = {dma_phys + status_offset, 1, k_desc_flag_write, 0};

    // avail 링에 head(디스크립터 0)를 등록.
    auto* avail_flags = reinterpret_cast<uint16_t*>(dma_virt + layout.avail_off);
    auto* avail_idx = avail_flags + 1;
    auto* avail_ring = avail_flags + 2;
    avail_flags[0] = 0;
    avail_ring[0] = 0;  // head descriptor index.
    // 메모리 배리어 없이도 x86 TSO(store-store 순서 보존)로 충분하다 —
    // avail_ring에 쓴 뒤 avail_idx를 올리는 순서 자체가 디바이스가
    // 기대하는 가시성 순서와 일치한다.
    *avail_idx = 1;

    auto* used_idx_ptr =
        reinterpret_cast<volatile uint16_t*>(dma_virt + layout.used_off + 2);
    uint16_t used_idx_before = *used_idx_ptr;

    out16(io_base + k_reg_queue_notify, 0);

    // 순수 폴링(인터럽트 없음, ADR-131 §근거 — 부트스트랩 1회성 전용
    // 최소 클라이언트) — 유한 횟수만 돈다.
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

    uint8_t status = dma_virt[status_offset];
    if (status != 0) {
        return false;  // VIRTIO_BLK_S_OK가 아니다(IOERR/UNSUPP).
    }

    *out_data = dma_virt + data_offset;
    *out_len = want_bytes;
    return true;
}

}  // namespace virtio_blk
