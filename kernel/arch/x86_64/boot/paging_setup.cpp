// _start64(boot.S)가 higher-half로 넘어가기 전에 호출하는 페이지테이블
// 확장. 아직 higher-half 매핑이 없는 시점에 실행되므로, 이 파일의 함수와
// 정적 데이터는 전부 .boot 섹션(항등 매핑된 저지대, VMA==LMA)에 강제
// 배치한다 — 그래야 자기 자신의 주소로 실행/접근할 수 있다.
// (docs/spec/virtual-memory-layout.md §2, §2.1)
#include <cstdint>

#include "../memory_layout.hpp"

extern "C" {
extern uint64_t pml4[512];  // boot.S가 .boot.bss에 정의.

// link.ld가 내보내는 커널 이미지 섹션 경계 (higher-half 가상주소).
extern const char _text_start[];
extern const char _text_end[];
extern const char _rodata_start[];
extern const char _rodata_end[];
extern const char _data_start[];
extern const char _bss_end[];
}

// _start64가 이 함수들을 호출하는 시점에는 higher-half 매핑이 아직
// 없다 — 함수 자신도 .boot.text(저지대, VMA==LMA)에 배치해야 자기
// 주소로 호출/실행 가능하다. 데이터만 옮기고 함수를 빠뜨리면 함수가
// 기본 .text(higher-half)에 링크되어, 그 매핑을 만드는 도중인 함수를
// 아직 없는 매핑을 통해 호출하려는 모순에 빠진다.
#define BOOT_TEXT __attribute__((section(".boot.text")))

namespace {

constexpr uint64_t k_page_size = 0x1000;
constexpr uint64_t k_present = 1ull << 0;
constexpr uint64_t k_writable = 1ull << 1;
constexpr uint64_t k_huge_page = 1ull << 7;  // PDPT 엔트리의 PS 비트(1GiB 페이지)
constexpr uint64_t k_no_execute = 1ull << 63;

// docs/spec/virtual-memory-layout.md §2 표. PML4/PDPT 인덱스는
// kern::arch::x86_64::k_physmap_base/k_kernel_virt_offset(memory_layout.hpp,
// 이 파일과 kernel/arch/x86_64/boot_info_x86_64.cpp가 공유하는 단일
// 소스)에서 컴파일 타임에 역산한다 — 매직 넘버로 따로 적어두면
// 상수가 바뀔 때 조용히 어긋날 수 있다.
constexpr uint32_t k_physmap_pml4_index =
    static_cast<uint32_t>((kern::arch::x86_64::k_physmap_base >> 39) & 0x1FFull);
constexpr uint32_t k_kernel_image_pml4_index =
    static_cast<uint32_t>((kern::arch::x86_64::k_kernel_virt_offset >> 39) & 0x1FFull);
constexpr uint32_t k_kernel_image_pdpt_index =
    static_cast<uint32_t>((kern::arch::x86_64::k_kernel_virt_offset >> 30) & 0x1FFull);

// 커널 스택 영역(0xFFFFFFFF00000000, PDPT 인덱스 508~509)은 슬롯 할당
// 알고리즘이 정해지는 M5 이전까지 페이지테이블 엔트리를 만들지 않는다 —
// virtual-memory-layout.md §6 참고. 여기서는 주소 상수만 존재를 인지한다.

__attribute__((section(".boot.bss"), aligned(4096))) uint64_t physmap_pdpt[512];
__attribute__((section(".boot.bss"), aligned(4096))) uint64_t top_pdpt[512];
__attribute__((section(".boot.bss"), aligned(4096))) uint64_t image_pd[512];
__attribute__((section(".boot.bss"), aligned(4096))) uint64_t image_pt[512];

// physmap: 물리 512GiB 전체를 1GiB 페이지 512개로 항등 매핑(ADR-078).
// M1~M8은 실제로 이 영역을 아직 쓰지 않지만(phys_to_virt는 M3), 부팅
// 시점에 함께 구성해두면 이후 마일스톤에서 부트 코드를 다시 건드릴
// 필요가 없다.
BOOT_TEXT void build_physmap() {
    for (uint32_t i = 0; i < 512; ++i) {
        uint64_t phys = static_cast<uint64_t>(i) << 30;
        physmap_pdpt[i] = phys | k_present | k_writable | k_huge_page | k_no_execute;
    }
    pml4[k_physmap_pml4_index] =
        reinterpret_cast<uint64_t>(physmap_pdpt) | k_present | k_writable;
}

// [virt_start, virt_end) 범위를 4KiB 페이지 단위로 image_pt에 채운다.
// virt는 커널 이미지 영역의 첫 2MiB(image_pd[0]) 안에 있어야 한다 —
// M1의 작은 이미지에서는 항상 성립한다.
BOOT_TEXT void map_image_range(const char* virt_start, const char* virt_end, uint64_t flags) {
    uint64_t start = reinterpret_cast<uint64_t>(virt_start) & ~(k_page_size - 1);
    uint64_t end = (reinterpret_cast<uint64_t>(virt_end) + k_page_size - 1) &
                    ~(k_page_size - 1);

    for (uint64_t virt = start; virt < end; virt += k_page_size) {
        uint64_t phys = virt - kern::arch::x86_64::k_kernel_virt_offset;
        uint64_t pt_index = (phys / k_page_size) % 512;
        image_pt[pt_index] = phys | flags;
    }
}

// 커널 이미지: docs/spec/virtual-memory-layout.md §3의 섹션별 권한대로
// .text(R+X)/.rodata(R)/.data+.bss(R+W)를 매핑한다.
BOOT_TEXT void build_kernel_image_mapping() {
    image_pd[0] = reinterpret_cast<uint64_t>(image_pt) | k_present | k_writable;
    top_pdpt[k_kernel_image_pdpt_index] =
        reinterpret_cast<uint64_t>(image_pd) | k_present | k_writable;
    pml4[k_kernel_image_pml4_index] =
        reinterpret_cast<uint64_t>(top_pdpt) | k_present | k_writable;

    map_image_range(_text_start, _text_end, k_present);
    map_image_range(_rodata_start, _rodata_end, k_present | k_no_execute);
    map_image_range(_data_start, _bss_end, k_present | k_writable | k_no_execute);
}

}  // namespace

extern "C" BOOT_TEXT void boot_setup_paging() {
    build_physmap();
    build_kernel_image_mapping();
}
