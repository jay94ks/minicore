// x86_64 레거시 PIT(8254) 채널2 최소 드라이버 (real-libc-syscall-layer.md
// §M33, ADR-184 §결정2) — HPET이 없을 때(예: QEMU `-no-hpet`) LAPIC
// 타이머 보정의 기준시계 폴백. 인터럽트를 쓰지 않는다 — 채널2의
// OUT2 상태(포트 0x61 bit5)를 폴링만 한다(모드0 "interrupt on
// terminal count"가 실제로는 OUT 핀을 low→high로 바꾸는 것뿐이라,
// 인터럽트 배선 없이도 이 핀 상태만으로 카운트다운 종료를 안다).
#pragma once

#include <cstdint>

namespace kern::arch::x86_64 {

// PIT 입력 클록 고정 주파수(Intel 8254, 모든 PC 호환 플랫폼 공통).
constexpr uint32_t k_pit_frequency_hz = 1193182;

// 채널2를 모드0으로 재프로그램해 ticks(k_pit_frequency_hz 기준)만큼
// 카운트다운한 뒤 반환한다(바쁜 대기). ticks==0이면 즉시 반환하지
// 않는다 — 16비트 카운터의 "0"은 최대값(65536)으로 취급되는 8254의
// 실제 동작을 그대로 따른다(호출자가 0을 넘기지 않도록 주의).
void pit_wait_ticks(uint16_t ticks);

}  // namespace kern::arch::x86_64
