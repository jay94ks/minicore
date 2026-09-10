// HPET MMIO 드라이버 구현 (hpet.hpp 상단 주석 참고). 레지스터 오프셋은
// HPET 사양(Intel/Microsoft "IA-PC HPET Specification" 1.0a) §2.3.
#include "hpet.hpp"

#include <mm/phys_map.hpp>

namespace kern::arch::x86_64 {

namespace {

constexpr uint32_t k_reg_general_caps_low = 0x00;    // COUNTER_CLK_PERIOD은 상위 32비트(0x04)에 있다.
constexpr uint32_t k_reg_general_caps_high = 0x04;
constexpr uint32_t k_reg_general_config_low = 0x10;
constexpr uint32_t k_reg_main_counter_low = 0xF0;
constexpr uint32_t k_reg_main_counter_high = 0xF4;

constexpr uint32_t k_config_enable_cnf = 1u << 0;

volatile uint32_t* g_hpet_base = nullptr;
uint64_t g_period_femtoseconds = 0;
bool g_hpet_available = false;

volatile uint32_t& reg32(uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<volatile uint8_t*>(g_hpet_base) + offset);
}

}  // namespace

void hpet_init(uint64_t base_phys) {
    g_hpet_base = static_cast<volatile uint32_t*>(kern::mm::phys_to_virt(base_phys));

    // 스펙상 COUNTER_CLK_PERIOD은 부팅 후 바뀌지 않는 하드웨어 상수라
    // (롤오버 위험이 없다) 32비트 두 번 읽기를 그냥 이어 붙인다.
    uint64_t caps = (static_cast<uint64_t>(reg32(k_reg_general_caps_high)) << 32) |
                    reg32(k_reg_general_caps_low);
    g_period_femtoseconds = caps >> 32;

    // 메인 카운터를 프리러닝으로 켠다 — QEMU/실기 모두 기본 비활성.
    reg32(k_reg_general_config_low) = k_config_enable_cnf;
    g_hpet_available = true;
}

bool hpet_available() { return g_hpet_available; }

uint64_t hpet_read_counter() {
    // 32비트 두 번 읽기 사이에 상위 32비트가 롤오버되면(64비트 카운터가
    // 실제로 넘어가는 그 순간) 낮은 32비트를 다시 읽는다 — HPET 사양
    // §2.4.7이 32비트 전용 구현을 위해 명시하는 안전한 읽기 순서다.
    uint32_t high1 = reg32(k_reg_main_counter_high);
    uint32_t low = reg32(k_reg_main_counter_low);
    uint32_t high2 = reg32(k_reg_main_counter_high);
    if (high1 != high2) {
        low = reg32(k_reg_main_counter_low);
        high1 = high2;
    }
    return (static_cast<uint64_t>(high1) << 32) | low;
}

uint64_t hpet_period_femtoseconds() { return g_period_femtoseconds; }

void hpet_wait_us(uint64_t us) {
    // 1us = 1e9fs이므로 ticks = us*1e9/period_fs.
    uint64_t ticks_needed = (us * 1'000'000'000ull) / g_period_femtoseconds;
    uint64_t target = hpet_read_counter() + ticks_needed;
    while (hpet_read_counter() < target) {
        asm volatile("pause");
    }
}

}  // namespace kern::arch::x86_64
