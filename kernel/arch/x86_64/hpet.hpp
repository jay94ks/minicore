// x86_64 HPET(High Precision Event Timer) 최소 드라이버
// (real-libc-syscall-layer.md §M33, ADR-184 §결정2) — LAPIC 타이머
// 보정의 1순위 기준시계. 메인 카운터를 프리러닝(free-running)으로
// 켜 두고 읽기만 한다 — 자체 인터럽트/비교기(comparator)는 쓰지
// 않는다(이 라운드는 "정확한 경과 시간을 안다"만 필요하다).
#pragma once

#include <cstdint>

namespace kern::arch::x86_64 {

// base_phys는 acpi.hpp::find_and_parse_hpet()이 얻은 HPET MMIO 물리
// 주소. 메인 카운터를 활성화한다(General Configuration Register의
// ENABLE_CNF 비트) — QEMU/실기 모두 기본은 비활성 상태로 부팅되므로
// 이 호출 없이는 hpet_read_counter()가 항상 같은 값만 반환한다.
void hpet_init(uint64_t base_phys);

// hpet_init()이 호출된 적이 있으면 true(HPET을 실제로 찾아 켰다는
// 뜻) — calibrate_lapic_timer()(lapic.cpp)가 HPET/PIT 중 어느 쪽을
// 기준시계로 쓸지 판단하는 데 쓴다. BSP가 부팅 극초반 한 번만
// hpet_init()을 부르고(kernel_main.cpp), 이후 이 전역 판단을 각
// AP도 그대로 공유한다(하드웨어 자체가 코어 공용 MMIO라 코어마다
// 다시 초기화할 필요가 없다 — lapic_init()의 g_lapic_base와 같은
// 정신).
bool hpet_available();

// hpet_init() 이후에만 유효 — 프리러닝 메인 카운터의 현재 값(원시
// 틱 수, 단위는 hpet_period_femtoseconds() 참고).
uint64_t hpet_read_counter();

// 이 HPET의 틱 하나가 몇 펨토초(1e-15초)인지 — General Capabilities
// and ID Register의 COUNTER_CLK_PERIOD 필드(하드웨어가 직접 보고,
// 스펙상 항상 100ns=1e8fs 이하). hpet_init() 이후에만 유효.
uint64_t hpet_period_femtoseconds();

// us 마이크로초가 지날 때까지 바쁜 대기한다(hpet_read_counter() 기반).
// hpet_init() 이후에만 호출한다.
void hpet_wait_us(uint64_t us);

}  // namespace kern::arch::x86_64
