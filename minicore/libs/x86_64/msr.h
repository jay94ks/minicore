#ifndef MINICORE_LIBS_X86_64_MSR_H
#define MINICORE_LIBS_X86_64_MSR_H

#include "libkenv/types.h"

// minicore/libs/x86_64 - x86-64 아키텍처별 공통 코드 라이브러리
// (SP-8B6B8D25 §3.0, io_port.h와 동일한 배치 근거) - Model-Specific
// Register 접근(rdmsr/wrmsr)은 x86 계열 ISA 명령어라 libkenv(아키텍처
// 무관)가 아니라 여기 있다.
//
// [정리, 2026-09-16, PN-F443FE73 작업 중 발견] 이 두 함수가
// lapic.cpp/syscall_fastpath.cpp에 이름만 조금씩 다르게(kReadMsr/
// kWriteMsr) 각자 중복 구현돼 있었다 - #MC 핸들러(idt.cpp)가 세 번째
// 복사본을 만들려던 차에 여기로 통합했다(동작 변경 없음, 순수 코드
// 정리).

namespace kernel {
namespace arch {

inline uint64_t kReadMsr64(uint32_t msr) {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline void kWriteMsr64(uint32_t msr, uint64_t value) {
    const auto low = static_cast<uint32_t>(value);
    const auto high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

}  // namespace arch
}  // namespace kernel

#endif  // MINICORE_LIBS_X86_64_MSR_H
