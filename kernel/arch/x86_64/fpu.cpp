#include "fpu.hpp"

#include <cstdint>

namespace arch_x86_64 {

namespace {
constexpr uint64_t k_cr0_mp = 1ull << 1;
constexpr uint64_t k_cr0_em = 1ull << 2;
constexpr uint64_t k_cr4_osfxsr = 1ull << 9;
constexpr uint64_t k_cr4_osxmmexcpt = 1ull << 10;
}  // namespace

void init_fpu() {
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~k_cr0_em;  // EM=0: x87/SSE 명령을 에뮬레이션 트랩 없이 직접 실행.
    cr0 |= k_cr0_mp;   // MP=1: WAIT/FWAIT도 TS 트랩 대상에 포함(표준 관례).
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= k_cr4_osfxsr | k_cr4_osxmmexcpt;  // FXSAVE/FXRSTOR 허용 + SIMD FP 예외를
                                              // #UD가 아니라 #XM으로 받는다.
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
}

}  // namespace arch_x86_64
