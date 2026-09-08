// AP 부팅 + IPI TLB shootdown 구현 (smp.hpp 상단 주석 참고).
#include "smp.hpp"

#include "idt.hpp"
#include "lapic.hpp"

#include <cstdint>

#include <klog.hpp>
#include <libk/atomic.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>

// ap_trampoline.S가 .rodata.ap_trampoline에 채운 바이트 범위 — bring_up_aps가
// 이 전체를 물리주소 k_ap_trampoline_phys로 복사한다.
extern "C" {
extern const uint8_t ap_trampoline_start[];
extern const uint8_t ap_trampoline_end[];
}

// ap_trampoline.S(ap_entry64_highhalf)가 이 두 값을 읽어 각각 %rsp/
// (ap_main의 첫 인자)로 쓴다 — 순차 기동(한 번에 AP 하나)이라 mailbox
// 슬롯 하나로 충분하다.
extern "C" uint64_t g_ap_boot_stack_top = 0;
extern "C" uint32_t g_ap_boot_cpu_index = 0;

// idle.S(M8, ADR-124) — AP도 온라인 신호를 보낸 뒤 이 코어를 멈춘다.
// M10은 AP에게 스케줄러/타이머를 주지 않으므로(계획 §범위 밖) BSP의
// arch_idle_halt()와 똑같이 hlt 루프로 충분하다.
extern "C" [[noreturn]] void arch_idle_halt();

namespace arch_x86_64 {

namespace {

struct cpu_record {
    uint32_t apic_id = 0;
    bool online = false;
};

cpu_record g_cpus[k_max_madt_cpus];
uint32_t g_cpu_count = 0;  // BSP 포함, 실제로 온라인 확인된 코어 수.

atomic<uint32_t> g_online_count{1};  // BSP는 항상 이미 온라인.

// broadcast_tlb_shootdown()이 채우고 smp_handle_tlb_shootdown_ipi()가
// 읽는다 — x86 TSO(store-store 순서 보존)에 의해, 이 값을 채운 뒤 보낸
// IPI(그 자체도 store, LAPIC ICR MMIO 쓰기)를 다른 코어가 받는 시점에는
// 이 값도 이미 그 코어에 보인다(같은 코어가 발신한 두 store 사이 순서는
// x86에서 항상 보존된다) — 그래서 이 값 자체는 atomic이 아니어도
// 안전하다. g_shootdown_pending만 컴파일러 재정렬을 막기 위해 atomic이다.
uint64_t g_shootdown_target_vaddr = 0;
atomic<uint32_t> g_shootdown_pending{0};

constexpr uint32_t k_ap_stack_order = 2;  // 16KiB — create_kernel_thread와 같은 크기.
constexpr uint64_t k_sipi_timeout_iterations = 50'000'000ull;

// M11(ADR-036) — apic_id로 색인한 노드 번호. 256개(APIC ID 전체 공간)
// 크기라 apic_id를 바로 인덱스로 쓸 수 있다 — set_cpu_node_map()이
// 아직 안 불렸으면 전부 0(토폴로지 정보 없음 폴백).
uint8_t g_apic_id_to_node[256] = {};

}  // namespace

void bring_up_aps(const madt_result& madt) {
    uint32_t bsp_apic_id = lapic_id();
    g_cpus[0].apic_id = bsp_apic_id;
    g_cpus[0].online = true;
    g_cpu_count = 1;

    // 트램폴린 바이트는 매 부팅마다 항상 같은 내용이라 한 번만 복사한다
    // — 모든 AP가 같은 코드를 실행한다.
    uint64_t blob_size = static_cast<uint64_t>(ap_trampoline_end - ap_trampoline_start);
    void* dest = mm::phys_to_virt(k_ap_trampoline_phys);
    __builtin_memcpy(dest, ap_trampoline_start, blob_size);

    for (uint32_t i = 0; i < madt.cpu_count && g_cpu_count < k_max_madt_cpus; ++i) {
        uint32_t apic_id = madt.apic_ids[i];
        if (apic_id == bsp_apic_id) {
            continue;
        }

        auto stack_page = mm::alloc_pages(k_ap_stack_order, 0);
        if (!stack_page.is_ok()) {
            klog::printf("[smp] AP apic_id=%u stack alloc failed — skip\n", apic_id);
            continue;
        }
        auto* stack_base = static_cast<uint8_t*>(mm::phys_to_virt(stack_page.value()));
        uint64_t stack_top =
            reinterpret_cast<uint64_t>(stack_base) + (mm::k_page_size << k_ap_stack_order);

        uint32_t cpu_index = g_cpu_count;
        g_ap_boot_stack_top = stack_top;
        g_ap_boot_cpu_index = cpu_index;

        uint32_t before = g_online_count.load_acquire();
        lapic_send_init_sipi_sipi(apic_id, k_ap_trampoline_phys);

        bool came_up = false;
        for (uint64_t spin = 0; spin < k_sipi_timeout_iterations; ++spin) {
            if (g_online_count.load_acquire() != before) {
                came_up = true;
                break;
            }
            asm volatile("pause");
        }

        if (!came_up) {
            klog::printf("[smp] AP apic_id=%u did not come online (timeout)\n", apic_id);
            continue;
        }
        g_cpus[cpu_index].apic_id = apic_id;
        g_cpus[cpu_index].online = true;
        ++g_cpu_count;
    }

    klog::printf("[smp] bring_up_aps done online_count=%u\n", g_cpu_count);
}

uint32_t online_cpu_count() { return g_cpu_count; }

void set_cpu_node_map(const madt_result& madt, const srat_slit_result& srat) {
    for (uint32_t i = 0; i < madt.cpu_count && i < k_max_madt_cpus; ++i) {
        g_apic_id_to_node[madt.apic_ids[i]] = static_cast<uint8_t>(srat.cpu_node[i]);
    }
}

void broadcast_tlb_shootdown(uint64_t vaddr) {
    if (g_cpu_count <= 1) {
        return;  // AP가 없다 — M1~M9와 동일하게 관찰 가능한 차이 없음.
    }

    g_shootdown_target_vaddr = vaddr;
    g_shootdown_pending.store_release(g_cpu_count - 1);

    for (uint32_t i = 1; i < g_cpu_count; ++i) {
        if (!g_cpus[i].online) {
            continue;
        }
        lapic_send_fixed_ipi(g_cpus[i].apic_id, k_vector_ipi_tlb_shootdown);
    }

    while (g_shootdown_pending.load_acquire() != 0) {
        asm volatile("pause");
    }
}

}  // namespace arch_x86_64

extern "C" void smp_handle_tlb_shootdown_ipi() {
    uint64_t vaddr = arch_x86_64::g_shootdown_target_vaddr;
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    arch_x86_64::g_shootdown_pending.fetch_add_relaxed(static_cast<uint32_t>(-1));
    lapic_eoi();
}

extern "C" uint32_t arch_current_node_id() {
    uint32_t apic_id = arch_x86_64::lapic_id();
    return arch_x86_64::g_apic_id_to_node[apic_id];
}

extern "C" void ap_main(uint32_t cpu_index) {
    // IDTR은 코어별 상태다 — BSP의 init_idt()가 채운 g_idt는 이미
    // 전역(공유 메모리)이지만, lidt 자체는 이 코어에서 다시 실행해야
    // 한다(안 하면 이 AP의 IDTR은 리셋 기본값(base=0,limit=0xFFFF)
    // 그대로 남아, 나중에 TLB shootdown IPI를 받는 순간 유효한 게이트를
    // 못 찾아 #GP→트리플폴트로 죽는다 — 실제로 QEMU에서 이 순서
    // 실수를 재현·확인했다). init_idt()는 같은 결과를 다시 계산할
    // 뿐이라 여러 코어가 반복 호출해도 안전하다(멱등).
    arch_x86_64::init_idt();

    arch_x86_64::lapic_enable_this_core();
    uint32_t apic_id = arch_x86_64::lapic_id();
    klog::printf("[smp] AP apic_id=%u online cpu_index=%u\n", apic_id, cpu_index);
    arch_x86_64::g_online_count.fetch_add_relaxed(1);

    // ap_trampoline.S가 진입 내내 인터럽트를 켜지 않았다(cli 상태 그대로
    // 여기까지 왔다) — IF=0인 채로 hlt하면 나중에 TLB shootdown IPI가
    // 와도 이 코어가 절대 깨어나지 못한다(고정 벡터 인터럽트는 IF=1일
    // 때만 전달된다, NMI/SMI/INIT과 다름). arch_idle_halt() 자체는
    // BSP(M8, sched::exit())도 공유하는 arch 훅이라 여기서 건드리지
    // 않고, AP 전용으로 호출 직전에 켠다.
    asm volatile("sti");
    arch_idle_halt();
}
