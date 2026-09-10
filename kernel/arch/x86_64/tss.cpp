// TSS 설정 구현. tss.hpp 상단 주석 참고.
#include "tss.hpp"

#include "gdt_selectors.hpp"

#include <cstdint>

#include <k/panic.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <object/kernel_objects.hpp>

namespace kern::arch::x86_64 {

namespace {

// Intel SDM Vol.3 §8.7 Figure 8-11 — 64비트 TSS 구조체. 이 프로젝트는
// RSP0만 쓴다(IST는 전부 미사용 — #DF/#NM처럼 스택 자체가 깨졌을 때만
// 필요한 별도 스택 전환이라 M12 범위 밖, 아직 필요해진 적 없다).
struct __attribute__((packed)) tss_struct {
    uint32_t reserved0 = 0;
    uint64_t rsp0 = 0;
    uint64_t rsp1 = 0;
    uint64_t rsp2 = 0;
    uint64_t reserved1 = 0;
    uint64_t ist1 = 0;
    uint64_t ist2 = 0;
    uint64_t ist3 = 0;
    uint64_t ist4 = 0;
    uint64_t ist5 = 0;
    uint64_t ist6 = 0;
    uint64_t ist7 = 0;
    uint64_t reserved2 = 0;
    uint16_t reserved3 = 0;
    uint16_t iomap_base = 0;
};
static_assert(sizeof(tss_struct) == 104, "TSS 구조체 크기가 Intel SDM Vol.3 §8.7과 다르다");

// M12(ADR-147) — I/O 허가 비트맵(IOPB, Intel SDM Vol.3 §8.7 "I/O
// Permission Bit Map"). 포트 0~65535 전체를 담으려면 8192바이트(1
// 비트/포트)가 필요하고, CPU가 마지막 포트 확인 시 그 바로 다음
// 바이트까지 읽으므로 스펙이 요구하는 대로 1바이트를 더 붙인다
// (8193바이트). TSS 바로 뒤에 물리적으로 이어 붙여야
// iomap_base(TSS 안의 오프셋)로 가리킬 수 있다 — 그래서 별도
// 구조체가 아니라 이 하나의 struct 안에 둔다. 기본값 전부
// 0xFF(모든 포트 접근 거부) — grant_io_port_range()가 필요한
// 범위만 0으로 지운다.
struct __attribute__((packed)) tss_with_iopb {
    tss_struct tss;
    uint8_t iopb[8193];
};

// M34(real-libc-syscall-layer.md §M34, ADR-185) 실행 중 발견한 진짜
// 버그: TSS는 실제 x86_64 하드웨어 요구사항상 **코어마다 하나씩**
// 있어야 한다(TR 레지스터가 가리키는 TSS를 두 코어가 동시에 같은
// 메모리로 공유할 수 없다 — ring3→ring0 전환마다 CPU가 "지금 이
// 코어의" TR이 가리키는 TSS에서 RSP0를 직접 읽는다). M21~M33까지는
// BSP 하나만 유저모드를 실행해 이 사실이 드러나지 않았다 — AP가
// 자기 몫의 init_tss()를 부른 적이 없어 TR이 리셋 기본값(무효)
// 그대로였고, AP에서 유저 스레드를 처음 예외/인터럽트(특히 M33이
// 이제 실제로 켜는 자기 LAPIC 타이머 틱)로 강제 전환시키는 순간
// TSS를 못 찾아 그 코어가 죽었다(트리플 폴트로 보이는 조용한 정지 —
// "[sched] cpu%u start_ap picked first user thread" 이후 그 코어가
// 도는 유저 스레드 쪽에서 아무 로그도 더 안 나오는 증상으로 실제로
// 드러났다). 고침: g_tss/g_gdt를 코어별 배열로 바꾼다 — 각 코어가
// **자기만의** GDT+TSS를 갖고 자기 자신에게만 LTR한다(0x38 셀렉터
// 값은 모든 코어에서 똑같지만, 서로 다른 사설 테이블을 가리킨다).
constexpr uint32_t k_max_cpus = kern::mm::k_max_cpus;

tss_with_iopb g_tss[k_max_cpus];

// M14(ADR-154) — 지금 IOPB에 실제로 프로그램된 범위(스레드가 아니라
// "IOPB의 현재 상태"를 기억한다 — sync_io_permission이 diff를 계산할
// 유일한 기준). 부팅 시점(아직 아무 스레드도 활성화하지 않음)에는
// 둘 다 0 — init_tss()가 이미 전체를 0xFF(거부)로 채워 두므로 "범위
// 없음" 상태와 정확히 일치한다. M34부터 코어별로 독립 추적한다
// (g_tss와 같은 이유).
uint16_t g_current_io_base[k_max_cpus] = {};
uint16_t g_current_io_count[k_max_cpus] = {};

void set_io_range(uint32_t cpu_index, uint16_t io_base, uint16_t count, bool allow) {
    for (uint32_t port = io_base; port < static_cast<uint32_t>(io_base) + count; ++port) {
        if (allow) {
            g_tss[cpu_index].iopb[port / 8] &= static_cast<uint8_t>(~(1u << (port % 8)));
        } else {
            g_tss[cpu_index].iopb[port / 8] |= static_cast<uint8_t>(1u << (port % 8));
        }
    }
}

// boot.S::gdt64_start(null+code32+data32+code64+data64+user_data64+
// user_code64, 7개×8바이트=56바이트)를 그대로 복사하고, 시스템
// 세그먼트라 16바이트를 쓰는 TSS 디스크립터(셀렉터 0x38)를 이어
// 붙인 확장판이다 — 인덱스 0~6이 boot.S와 완전히 같은 값이라 부팅
// 때 이미 로드된 CS/SS 등 셀렉터가 이 GDT로 바꿔 실어도 계속
// 유효하다(어차피 CPU는 셀렉터 재로드 시점에만 GDT를 다시 읽으므로,
// 지금 당장 셀렉터 레지스터들을 재로드할 필요조차 없다 — TSS만
// 새로 LTR한다).
// M34 — 코어마다 자기만의 사설 GDT 사본(같은 고정 7개 항목 + 자기
// TSS 디스크립터). 셀렉터 값(0x38 등)은 모든 코어에서 동일하다 —
// 서로 다른 물리 테이블을 가리킬 뿐이다.
using gdt_array = uint64_t[9];
gdt_array g_gdt[k_max_cpus];

constexpr gdt_array k_gdt_template = {
    0x0000000000000000ull,  // 0x00: null
    0x00CF9A000000FFFFull,  // 0x08: code32
    0x00CF92000000FFFFull,  // 0x10: data32
    0x00A09A0000000000ull,  // 0x18: code64
    0x0000920000000000ull,  // 0x20: data64
    0x0000F20000000000ull,  // 0x28: user_data64
    0x00A0FA0000000000ull,  // 0x30: user_code64
    0,                       // 0x38: TSS low  — 아래 init_tss()가 채운다.
    0,                       // TSS high
};

struct __attribute__((packed)) gdt_pointer {
    uint16_t limit;
    uint64_t base;
};

// 예외/인터럽트가 ring3에서 발생했을 때 CPU가 전환할 커널 스택
// (RSP0). init_tss()가 부팅 시점에 이 "기본" 스택 하나를 만들어
// g_tss.tss.rsp0의 최초값으로 쓴다 — M21(ADR-177) 전에는 이 스택
// 하나로 충분했다("예외 처리는 항상 순차적, 처리 끝나면 IRETQ로
// 곧바로 돌아간다"). M21부터는 sync_exception_stack()이 컨텍스트
// 스위치마다 이 값을 실제로 실행 중인 유저 스레드의 스택
// (syscall_kernel_rsp)으로 덮어쓴다 — 이 초기값은 첫 스레드가
// 스케줄되기 전 극초기 구간(이론상 인터럽트가 ring3에서 발생할 수
// 없는 구간)에만 의미가 있다.
constexpr uint32_t k_exception_stack_order = 2;  // 16KiB

}  // namespace

// M34(real-libc-syscall-layer.md §M34, ADR-185) — arch_current_cpu_index()
// (smp.cpp)와 완전히 같은 HAL 경계 이유로 여기서도 최소 선언만 가져와
// 쓴다(scheduler.cpp의 같은 extern "C" 선언과 동일한 패턴).
extern "C" uint32_t arch_current_cpu_index();

namespace {

void init_tss_for_this_core(uint32_t cpu_index) {
    auto stack_page = kern::mm::alloc_pages(k_exception_stack_order, 0);
    if (!stack_page.is_ok()) {
        LIBK_PANIC("init_tss: 예외 전용 스택(RSP0) 확보 실패");
    }
    auto* stack_base = static_cast<uint8_t*>(kern::mm::phys_to_virt(stack_page.value()));
    uint64_t stack_top = reinterpret_cast<uint64_t>(stack_base) +
                          (static_cast<uint64_t>(kern::mm::k_page_size) << k_exception_stack_order);
    tss_with_iopb& tss = g_tss[cpu_index];
    tss.tss.rsp0 = stack_top;
    tss.tss.iomap_base = sizeof(tss_struct);  // IOPB가 TSS 바로 뒤에서 시작.
    __builtin_memset(tss.iopb, 0xFF, sizeof(tss.iopb));  // 기본: 모든 포트 접근 거부.

    uint64_t tss_base = reinterpret_cast<uint64_t>(&tss);
    uint64_t limit = sizeof(tss_with_iopb) - 1;

    // Intel SDM Vol.3 §8.2.3 Figure 8-4 — 64비트 TSS 디스크립터(16바이트).
    uint64_t low = (limit & 0xFFFFull) | ((tss_base & 0xFFFFFFull) << 16) |
                   (0x89ull << 40) |  // present=1, DPL=0, S=0, type=0x9(64비트 TSS, available)
                   (((limit >> 16) & 0xFull) << 48) | (((tss_base >> 24) & 0xFFull) << 56);
    uint64_t high = (tss_base >> 32) & 0xFFFFFFFFull;

    gdt_array& gdt = g_gdt[cpu_index];
    __builtin_memcpy(gdt, k_gdt_template, sizeof(gdt_array));
    gdt[7] = low;
    gdt[8] = high;

    gdt_pointer ptr{};
    ptr.limit = static_cast<uint16_t>(sizeof(gdt_array) - 1);
    ptr.base = reinterpret_cast<uint64_t>(gdt);
    asm volatile("lgdt %0" : : "m"(ptr));

    constexpr uint16_t k_sel_tss = 0x38;
    asm volatile("ltr %w0" : : "r"(k_sel_tss));
}

}  // namespace

void init_tss() { init_tss_for_this_core(arch_current_cpu_index()); }

void sync_io_permission(const kern::object::thread& t) {
    uint32_t cpu = arch_current_cpu_index();
    uint16_t new_base = static_cast<uint16_t>(t.io_port_base);
    uint16_t new_count = static_cast<uint16_t>(t.io_port_count);
    if (new_base == g_current_io_base[cpu] && new_count == g_current_io_count[cpu]) {
        return;  // 이미 이 범위로 프로그램돼 있다 — 아무 것도 하지 않는다.
    }
    if (g_current_io_count[cpu] > 0) {
        set_io_range(cpu, g_current_io_base[cpu], g_current_io_count[cpu], /*allow=*/false);
    }
    if (new_count > 0) {
        set_io_range(cpu, new_base, new_count, /*allow=*/true);
    }
    g_current_io_base[cpu] = new_base;
    g_current_io_count[cpu] = new_count;
}

void sync_exception_stack(const kern::object::thread& t) {
    if (t.owner_space != nullptr) {
        g_tss[arch_current_cpu_index()].tss.rsp0 = t.syscall_kernel_rsp;
    }
}

namespace {
constexpr uint32_t k_msr_fs_base = 0xC0000100;

void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = static_cast<uint32_t>(value);
    uint32_t hi = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" ::"c"(msr), "a"(lo), "d"(hi));
}
}  // namespace

void sync_fs_base(const kern::object::thread& t) {
    wrmsr(k_msr_fs_base, t.fs_base);
}

}  // namespace kern::arch::x86_64

// kernel/core/sched/scheduler.cpp가 컨텍스트 스위치마다 부르는 훅
// (ADR-002 HAL 경계 — core는 이 파일을 include하지 않고 이 시그니처만
// extern "C"로 안다).
extern "C" void arch_sync_io_permission(const kern::object::thread& next) {
    kern::arch::x86_64::sync_io_permission(next);
}

// M21(ADR-177) — tss.hpp::sync_exception_stack() 참고. scheduler.cpp가
// arch_sync_io_permission과 같은 자리(컨텍스트 스위치 4곳)에서 함께
// 부른다.
extern "C" void arch_sync_exception_stack(const kern::object::thread& next) {
    kern::arch::x86_64::sync_exception_stack(next);
}

// M28(real-libc-syscall-layer.md §M28) — tss.hpp::sync_fs_base() 참고.
// scheduler.cpp가 위 두 훅과 같은 자리(컨텍스트 스위치 4곳)에서
// 함께 부른다.
extern "C" void arch_sync_fs_base(const kern::object::thread& next) {
    kern::arch::x86_64::sync_fs_base(next);
}
