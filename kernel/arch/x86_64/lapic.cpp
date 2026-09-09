// LAPIC xAPIC MMIO 드라이버 구현 (lapic.hpp 상단 주석 참고).
#include "lapic.hpp"

#include <cstdint>

#include <klog.hpp>
#include <mm/phys_map.hpp>

namespace arch_x86_64 {

namespace {

// 레지스터 오프셋(Intel SDM Vol.3 §11.4.1 표 11-1).
constexpr uint32_t k_reg_id = 0x20;
constexpr uint32_t k_reg_tpr = 0x80;
constexpr uint32_t k_reg_eoi = 0xB0;
constexpr uint32_t k_reg_spurious = 0xF0;
constexpr uint32_t k_reg_icr_low = 0x300;
constexpr uint32_t k_reg_icr_high = 0x310;

// M21(ADR-176) — LVT Timer/Initial Count/Divide Configuration
// (Intel SDM Vol.3 §11.5.4, 표 11-2/11-11).
constexpr uint32_t k_reg_lvt_timer = 0x320;
constexpr uint32_t k_reg_timer_initial_count = 0x380;
constexpr uint32_t k_reg_timer_divide_config = 0x3E0;

constexpr uint32_t k_lvt_timer_mode_periodic = 1u << 17;  // 0=one-shot, 1=periodic.
// Divide Configuration Register 인코딩(표 11-11) — bit0,1,3이 실제
// 값이고 bit2는 항상 0이다. 0b0011 = divide by 16.
constexpr uint32_t k_timer_divide_by_16 = 0b0011;

constexpr uint32_t k_icr_delivery_init = 0b101u << 8;
constexpr uint32_t k_icr_delivery_startup = 0b110u << 8;
constexpr uint32_t k_icr_delivery_fixed = 0b000u << 8;
constexpr uint32_t k_icr_level_assert = 1u << 14;
constexpr uint32_t k_icr_delivery_status = 1u << 12;

volatile uint32_t* g_lapic_base = nullptr;

volatile uint32_t& reg(uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<volatile uint8_t*>(g_lapic_base) + offset);
}

// ICR 전송 완료(Delivery Status=0)까지 스핀 대기 — QEMU/실기 모두 이
// 비트는 즉시 떨어진다(Intel SDM Vol.3 §11.6.2, 실제로 QEMU에서 항상
// spin=0에서 바로 클리어됨을 확인했다). 그래도 상한 없는 루프는 두지
// 않는다 — 상한을 두고 타임아웃을 klog로 남기는 쪽이, LAPIC MMIO가
// 실제로 응답하지 않는(예: base 주소가 잘못됨) 상황을 조용히 영원히
// 멈추는 대신 진단 가능하게 만든다(ADR-126과 같은 "원인 불명 정지
// 진단" 정신).
void wait_icr_idle() {
    constexpr uint64_t k_timeout_iterations = 10'000'000;
    for (uint64_t spin = 0; spin < k_timeout_iterations; ++spin) {
        if ((reg(k_reg_icr_low) & k_icr_delivery_status) == 0) {
            return;
        }
        asm volatile("pause");
    }
    klog::printf("[lapic] wait_icr_idle timeout icr_low=0x%lx icr_high=0x%lx\n",
                 static_cast<unsigned long>(reg(k_reg_icr_low)),
                 static_cast<unsigned long>(reg(k_reg_icr_high)));
}

// 규격이 요구하는 INIT/SIPI 사이 지연(10ms/200us, Intel MP 초기화
// 절차)을 흉내내는 고정 횟수 바쁜 대기 — M10은 타이머 인프라를 아직
// 갖추지 않는다(계획 §범위 밖)라 실제 밀리초 단위로 보정하지 않았다.
// QEMU에서 실측한 값(아래 lapic_send_init_sipi_sipi)으로 절차의
// 목적(AP가 이전 신호를 처리할 시간을 준다)은 충분히 달성된다.
void busy_delay(uint64_t iterations) {
    // asm volatile("pause")가 매 반복 관찰 가능한 부작용으로 취급되어
    // 루프 자체가 최적화로 사라지지 않는다 — 루프 변수를 volatile로
    // 둘 필요가 없다.
    for (uint64_t i = 0; i < iterations; ++i) {
        asm volatile("pause");
    }
}

void send_icr(uint32_t target_apic_id, uint32_t low_bits) {
    reg(k_reg_icr_high) = target_apic_id << 24;
    reg(k_reg_icr_low) = low_bits;
    wait_icr_idle();
}

}  // namespace

void lapic_enable_this_core() {
    reg(k_reg_tpr) = 0;  // 모든 우선순위의 인터럽트를 받아들인다.

    // 소프트웨어 활성화(bit8) + 스퓨리어스 벡터(idt.hpp::k_vector_spurious).
    // 상수를 여기서도 그대로 못 쓰는 이유는 lapic.hpp가 idt.hpp를 몰라도
    // 되게 하려는 것(HAL 내부에서도 계층을 최소화) — 값은 0xFF로
    // idt.hpp와 동기화되어 있다(두 값이 어긋나면 스퓨리어스 인터럽트이
    // catch-all로 잘못 라우팅되어 즉시 눈에 띈다).
    constexpr uint32_t k_apic_software_enable = 1u << 8;
    constexpr uint32_t k_spurious_vector = 0xFF;
    reg(k_reg_spurious) = k_apic_software_enable | k_spurious_vector;
}

void lapic_init(uint64_t base_phys) {
    g_lapic_base = static_cast<volatile uint32_t*>(mm::phys_to_virt(base_phys));
    lapic_enable_this_core();
}

uint32_t lapic_id() { return reg(k_reg_id) >> 24; }

void eoi() { reg(k_reg_eoi) = 0; }

void lapic_send_init_sipi_sipi(uint32_t target_apic_id, uint64_t trampoline_phys) {
    uint32_t sipi_vector = static_cast<uint32_t>((trampoline_phys >> 12) & 0xFFu);

    // INIT assert. deassert 단계는 두지 않는다(현대 로컬 APIC/QEMU
    // 에뮬레이션 모두 assert만으로 충분 — Linux도 최신 커널에서는
    // deassert를 생략한다).
    send_icr(target_apic_id, k_icr_delivery_init | k_icr_level_assert);
    busy_delay(1'000'000);  // ~INIT 후 지연(정확한 10ms는 아니다, 위 주석).

    // STARTUP(SIPI)에는 level/trigger 비트를 쓰지 않는다 — Intel SDM
    // Vol.3 Table 11-8: STARTUP 딜리버리 모드에서는 그 비트들이 적용
    // 대상이 아니다(edge). 처음에 INIT과 똑같이 level|assert를 얹어
    // 보냈다가 delivery status가 영원히 안 떨어지는 버그를 QEMU에서
    // 실제로 겪었다 — 이 비트를 빼자 정상화됨을 확인했다.
    //
    // 두 번째 SIPI는 보내지 않는다 — 고전적인 "SIPI 두 번" 관례는
    // 486급 MP 호환성 때문이었는데, 실제로 QEMU에서 두 번째를 보내면
    // (이미 첫 SIPI로 AP가 기동을 마치고 곧 인터럽트 비활성 상태로
    // hlt에 들어간 뒤이므로) ICR delivery status가 영원히 안 떨어지는
    // 정지를 재현·확인했다 — 첫 SIPI 하나로 AP 기동이 이미 확인되므로
    // (아래 bring_up_aps의 온라인 확인 루프) 필요하지 않다.
    send_icr(target_apic_id, k_icr_delivery_startup | sipi_vector);
    busy_delay(50'000);
}

void lapic_send_fixed_ipi(uint32_t target_apic_id, uint8_t vector) {
    send_icr(target_apic_id, k_icr_delivery_fixed | vector);
}

void lapic_start_periodic_timer(uint8_t vector, uint32_t initial_count) {
    reg(k_reg_timer_divide_config) = k_timer_divide_by_16;
    reg(k_reg_lvt_timer) = k_lvt_timer_mode_periodic | vector;
    // Initial Count에 쓰는 순간부터 카운트다운이 시작한다(SDM Vol.3
    // §11.5.4) — LVT_Timer를 먼저 걸어 둬야 이 첫 카운트다운이 끝나는
    // 순간부터 곧바로 vector가 걸린다.
    reg(k_reg_timer_initial_count) = initial_count;
}

}  // namespace arch_x86_64

extern "C" void lapic_eoi() { arch_x86_64::eoi(); }
