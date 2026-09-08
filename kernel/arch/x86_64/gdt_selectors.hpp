// GDT 셀렉터 상수 (boot/boot.S의 gdt64_start가 실제 정의). M8부터
// syscall.cpp/usermode.S가 STAR MSR·IRETQ 프레임 구성에 이 값들을
// 쓴다 — boot.S의 .set 값과 반드시 일치해야 한다(두 파일이 같은
// 상수를 각자의 언어로 중복 정의하는 것은 부트 스텁의 기존 관례,
// SEL_CODE64 등도 이미 그렇다).
#pragma once

#include <cstdint>

namespace arch_x86_64 {

inline constexpr uint16_t k_sel_code64 = 0x18;
inline constexpr uint16_t k_sel_data64 = 0x20;
inline constexpr uint16_t k_sel_user_data64 = 0x28;
inline constexpr uint16_t k_sel_user_code64 = 0x30;

}  // namespace arch_x86_64
