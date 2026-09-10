// FPU 활성화 + lazy 전환 구현 (fpu.hpp 상단 주석 참고).
#include "fpu.hpp"

#include "lapic.hpp"

#include <cstdint>

#include <klog.hpp>
#include <libk/panic.hpp>
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

namespace kern::arch::x86_64 {

namespace {

constexpr uint64_t k_cr0_mp = 1ull << 1;
constexpr uint64_t k_cr0_em = 1ull << 2;
constexpr uint64_t k_cr0_ts = 1ull << 3;
constexpr uint64_t k_cr4_osfxsr = 1ull << 9;
constexpr uint64_t k_cr4_osxmmexcpt = 1ull << 10;
constexpr uint64_t k_cr4_osxsave = 1ull << 18;

constexpr uint32_t k_cpuid1_ecx_xsave = 1u << 26;
constexpr uint32_t k_cpuid1_ecx_avx = 1u << 28;

// XCR0 비트(Intel SDM Vol.1 §13.3): x87=0, SSE=1, AVX=2.
constexpr uint32_t k_xcr0_enable_mask = (1u << 0) | (1u << 1) | (1u << 2);

// kern::object::thread::fpu_save_area의 실제 크기(ADR-133) — 이보다 커지면
// 버퍼 오버플로다.
constexpr uint32_t k_max_fpu_area_size = 1024;

bool g_use_xsave = false;
uint32_t g_fpu_area_size = 512;  // FXSAVE 고정 크기가 기본값(폴백).

// 코어별 "지금 FPU 레지스터의 실제 소유자" — apic_id로 직접 색인한다
// (smp.cpp::g_apic_id_to_node와 같은 패턴 — 별도 코어 인덱스 압축이
// 필요 없다). nullptr = 아무도 소유하지 않음(리셋 이후 또는 이전
// 소유자가 이미 종료됨, arch_fpu_thread_exiting 참고).
kern::object::thread* g_fpu_owner_by_apic_id[256] = {};

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx,
           uint32_t& edx) {
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
}

// XSAVE/XRSTOR는 EDX:EAX에 "요청한 기능 비트마스크"(RFBM)를 싣고
// 실행한다 — 항상 k_xcr0_enable_mask(부팅 시 XCR0에 켠 것과 동일)를
// 요청한다. 64바이트 정렬 버퍼가 필요하다(kern::object::thread::fpu_save_area
// 가 이미 alignas(64), ADR-133).
void xsave_area(void* area) {
    asm volatile("xsave (%0)" : : "r"(area), "a"(k_xcr0_enable_mask), "d"(0u) : "memory");
}
void xrstor_area(void* area) {
    asm volatile("xrstor (%0)" : : "r"(area), "a"(k_xcr0_enable_mask), "d"(0u) : "memory");
}
void fxsave_area(void* area) { asm volatile("fxsave (%0)" : : "r"(area) : "memory"); }
void fxrstor_area(void* area) { asm volatile("fxrstor (%0)" : : "r"(area) : "memory"); }

void save_fpu_state(void* area) {
    if (g_use_xsave) {
        xsave_area(area);
    } else {
        fxsave_area(area);
    }
}

void restore_fpu_state(void* area) {
    if (g_use_xsave) {
        xrstor_area(area);
    } else {
        fxrstor_area(area);
    }
}

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

    // M11b(ADR-133) — XSAVE(bit26)·AVX(bit28) 지원 여부 검사. 둘 다
    // 있어야 XSAVE 경로를 쓴다(AVX 없이 XSAVE만 있으면 얻을 이득이
    // 없다 — ADR-127이 이미 "AVX를 쓸 일이 없으면 FXSAVE로 충분"이라고
    // 판단한 것과 같은 논리).
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, 0, eax, ebx, ecx, edx);
    bool has_xsave = (ecx & k_cpuid1_ecx_xsave) != 0;
    bool has_avx = (ecx & k_cpuid1_ecx_avx) != 0;

    if (has_xsave && has_avx) {
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= k_cr4_osxsave;
        asm volatile("mov %0, %%cr4" : : "r"(cr4));

        // XSETBV: ECX = XCR0 인덱스(0), EDX:EAX = 새 값.
        asm volatile("xsetbv" : : "c"(0u), "a"(k_xcr0_enable_mask), "d"(0u));

        // CPUID leaf 0xD sub-leaf 0의 EBX = "현재 XCR0에서 활성화된
        // 기능들"을 저장하는 데 필요한 바이트 수(Intel SDM Vol.2A) —
        // 방금 XSETBV로 x87|SSE|AVX를 켰으니 정확히 그 조합의 크기다.
        cpuid(0xD, 0, eax, ebx, ecx, edx);
        uint32_t required = ebx;
        if (required == 0 || required > k_max_fpu_area_size) {
            // ADR-133이 "AVX-512는 다루지 않으므로 항상 1024바이트
            // 이내"라고 전제했다 — 이 전제가 깨지면 조용히 넘어가지
            // 않고 즉시 멈춘다(ADR-001 정확성 우선).
            LIBK_PANIC("init_fpu: XSAVE area size exceeds 1024-byte budget (ADR-133)");
        }
        g_use_xsave = true;
        g_fpu_area_size = required;
    }
    // 둘 중 하나라도 없으면 g_use_xsave는 false로 남고(기본값), 위에서
    // 이미 설정한 CR4.OSFXSR 경로(FXSAVE/FXRSTOR, 512바이트)로 폴백한다
    // — ADR-127의 원래 경로 그대로.

    // BSP·AP 모두 이 함수를 호출한다(멱등, 코어별 CR0/CR4/XCR0 설정이
    // 필요해서다) — LAPIC이 아직 없는 시점(BSP의 첫 호출)에도 안전하게
    // 찍을 수 있도록 apic_id는 넣지 않는다(계획 §M11b 검증목표(b)의
    // "-cpu 옵션으로 AVX 유/무 두 QEMU 구성 모두 확인"을 이 로그로
    // 구분한다).
    kern::klog::printf("[fpu] xsave_avail=%u avx_avail=%u using_xsave=%u area_size=%u\n", has_xsave,
                 has_avx, g_use_xsave, g_fpu_area_size);
}

}  // namespace kern::arch::x86_64

extern "C" void arch_x86_64_handle_nm_trap() {
    // #NM 핸들러의 첫 일 — TS를 클리어해 이 명령이 재실행될 때
    // 다시 트랩하지 않게 한다(ADR-133 §결정2-1).
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~kern::arch::x86_64::k_cr0_ts;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    uint32_t apic_id = kern::arch::x86_64::lapic_id();
    kern::object::thread* current = kern::sched::current();
    kern::object::thread*& owner = kern::arch::x86_64::g_fpu_owner_by_apic_id[apic_id];

    if (owner == current) {
        // 마지막으로 이 코어에서 FPU를 쓴 게 바로 지금 스레드고, 그
        // 사이 아무도 레지스터를 건드리지 않았다 — 레지스터에 이미
        // 맞는 값이 들어있으므로 저장/복원 없이 그냥 리턴한다
        // (ADR-133 §결정2-2, lazy 전환의 핵심 이점). 계획 §M11b의
        // 검증 목표(b) — "같은 스레드가 연속으로 FPU를 쓸 때 저장/
        // 복원 없이 즉시 리턴함"을 로그로 남긴다.
        kern::klog::printf("[fpu] #NM apic_id=%u owner_changed=0\n", apic_id);
        return;
    }

    if (owner != nullptr) {
        kern::arch::x86_64::save_fpu_state(owner->fpu_save_area);
    }
    if (current != nullptr) {
        kern::arch::x86_64::restore_fpu_state(current->fpu_save_area);
    }
    owner = current;
    kern::klog::printf("[fpu] #NM apic_id=%u owner_changed=1\n", apic_id);
}

extern "C" void arch_fpu_thread_exiting(kern::object::thread* t) {
    for (kern::object::thread*& owner : kern::arch::x86_64::g_fpu_owner_by_apic_id) {
        if (owner == t) {
            owner = nullptr;
        }
    }
}
