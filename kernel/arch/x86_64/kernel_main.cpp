// x86_64 커널 진입점(higher-half, docs/spec/virtual-memory-layout.md §2.1
// 3단계에서 boot.S의 _start64가 여기로 점프한다).
//
// docs/plan/kernel-bootstrap.md M1: QEMU 시리얼 콘솔에 "hello from
// kernel" 출력. M2: boot_info(Multiboot2 태그 파싱) 파이프라인. M3:
// mm(물리 페이지 할당자 + 슬랩 힙) 초기화·왕복 확인. M4: 핸들 테이블
// (objects.md) + 페이지테이블 조작 API 왕복 확인. M5: 커널 스레드
// 2개가 협조적으로 번갈아 실행됨을 확인. M6: 커널 스레드 2개 사이의
// Call → Recv → Reply 왕복 확인. M7: 페이지 1개 copy 전달 + 핸들 위임 +
// notification 왕복 확인. M8: initrd에서 initrun ELF를 로드해 유저모드로
// 진입시키고, initrun이 SYSCALL로 보낸 IPC Call에 커널이 응답 — 이
// 계획의 최종 완료 기준(kernel-bootstrap.md M8).
//
// docs/plan/smp-fpu-bringup.md M9(ADR-127): FPU/SIMD 컨텍스트 스위칭 —
// 서로 다른 두 스레드가 각자 xmm 레지스터에 넣어 둔 값이 yield()를
// 여러 번 거쳐도 섞이지 않음을 확인. M10(ADR-055): IDT 기초 + ACPI
// MADT 파싱 + LAPIC 구동 + AP 기동(INIT-SIPI-SIPI) + IPI 기반 TLB
// shootdown — MINICORE_QEMU_SMP=N으로 띄우면 BSP를 제외한 나머지
// N-1개 AP가 전부 온라인되고, 이후 map_page/unmap_page/protect_page
// 호출마다(demo_page_table 등 기존 M1~M9 데모 전체 포함) IPI shootdown이
// 실제로 왕복함을 확인.
#include "acpi.hpp"
#include "boot_info.hpp"
#include "boot_info_x86_64.hpp"
#include "elf_loader.hpp"
#include "fpu.hpp"
#include "idt.hpp"
#include "klog.hpp"
#include "lapic.hpp"
#include "page_table.hpp"
#include "smp.hpp"
#include "syscall.hpp"

#include <cstdint>

#include <initrd/mcpack.hpp>
#include <ipc/endpoint.hpp>
#include <ipc/notification.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <mm/slab.hpp>
#include <object/handle_table.hpp>
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

// init/initrun/CMakeLists.txt가 만들고 kernel/arch/x86_64/initrd_blob.S.in이
// .incbin으로 커널 이미지에 심은 MCPACK 이미지(M8, initrd_blob.S.in
// 상단 주석 참고) — 일반 .rodata라 higher-half 커널 가상주소로 이미
// 바로 역참조 가능하다(phys_to_virt 불필요).
extern "C" {
extern const uint8_t g_embedded_initrd_start[];
extern const uint8_t g_embedded_initrd_end[];
}

// boot.S(_start32)가 부트로더 진입 시점의 EAX/EBX(Multiboot2 매직/info
// 물리주소)를 저장해 둔 전역 변수. .boot.bss(저지대, 항등 매핑 유지)에
// 있어 higher-half에서도 그대로 읽을 수 있다.
extern "C" {
extern uint32_t mb2_magic;
extern uint32_t mb2_info_addr;
}

namespace {

void dump_pool_stats(const char* tag) {
    for (uint32_t node = 0; node < mm::node_count(); ++node) {
        mm::pool_stats s = mm::stats(node);
        klog::printf("[mm:%s] node[%u] total_bytes=0x%lx free_bytes=0x%lx reserved_bytes=0x%lx\n",
                     tag, node, static_cast<unsigned long>(s.total_bytes),
                     static_cast<unsigned long>(s.free_bytes),
                     static_cast<unsigned long>(s.reserved_bytes));
    }
}

// M10(ADR-055) — acpi.cpp의 MADT 파싱이 "real" boot_info.arch_data_addr가
// 있으면 우선 그걸 신뢰하도록(GRUB/UEFI 실배포 경로 대비) 반환값으로
// 넘겨준다. 이 개발 머신(QEMU PVH, ADR-114)에서는 항상 0이라 acpi.cpp
// 자신의 EBDA/BIOS ROM 스캔 폴백이 실제로 타는 경로다.
uint64_t demo_mm() {
    const boot::memory_region* regions = nullptr;
    boot::boot_info info = arch_x86_64::build_boot_info(mb2_magic, mb2_info_addr, &regions);
    boot::dump("real", info, regions);

    // 이 개발 머신에는 GRUB가 없어 QEMU 검증 경로(ADR-114, PVH)로는
    // 위 "real" 결과가 항상 비어 있다 — build_boot_info()의 파싱 로직
    // 자체가 올바른지는 아래 자체 테스트로 확인한다(docs/done/의 M2
    // 완료 보고 참고).
    const boot::memory_region* selftest_regions = nullptr;
    boot::boot_info selftest_info = arch_x86_64::run_boot_info_self_test(&selftest_regions);
    boot::dump("selftest", selftest_info, selftest_regions);

    // mm::init은 실제 boot_info(현재 이 QEMU 경로에서는 항상 빈
    // memory_map)가 아니라 위 self-test boot_info로 물리 페이지
    // 할당자를 채운다 — "real"로 채우면 usable 영역이 0개라 이후
    // 검증 자체가 불가능하다. 이것도 ADR-114/117이 이미 기록한 GRUB
    // 부재 한계의 연장이다(docs/done/kernel-bootstrap-m3.md 참고).
    mm::init(selftest_info, selftest_regions);
    dump_pool_stats("init");

    auto page0 = mm::alloc_pages(0, 0);
    auto page2 = mm::alloc_pages(2, 0);
    klog::printf("[mm:alloc] order0 ok=%u addr=0x%lx order2 ok=%u addr=0x%lx\n", page0.is_ok(),
                 static_cast<unsigned long>(page0.is_ok() ? page0.value() : 0), page2.is_ok(),
                 static_cast<unsigned long>(page2.is_ok() ? page2.value() : 0));
    dump_pool_stats("after_alloc");

    if (page0.is_ok()) {
        mm::free_pages(page0.value(), 0);
    }
    if (page2.is_ok()) {
        mm::free_pages(page2.value(), 2);
    }
    dump_pool_stats("after_free");

    void* slab_a = mm::slab_alloc(32);
    void* slab_b = mm::slab_alloc(32);
    klog::printf("[mm:slab] alloc(32) a=%p b=%p\n", slab_a, slab_b);
    if (slab_a != nullptr) {
        mm::slab_free(slab_a, 32);
    }
    if (slab_b != nullptr) {
        mm::slab_free(slab_b, 32);
    }
    klog::printf("[mm:slab] freed both chunks\n");
    return info.arch_data_addr;
}

// M10(smp-fpu-bringup.md §M10, ADR-055) — ACPI MADT를 파싱해 LAPIC을
// 켜고, 발견한 AP를 전부 순차 기동한다. mm::init()이 이미 끝난 뒤라야
// 한다(bring_up_aps가 AP 커널 스택을 mm::alloc_pages로 확보한다).
void demo_smp(uint64_t real_arch_data_addr) {
    arch_x86_64::madt_result madt{};
    bool madt_ok = arch_x86_64::find_and_parse_madt(real_arch_data_addr, madt);
    klog::printf("[acpi] madt_ok=%u cpu_count=%u lapic_base=0x%lx\n", madt_ok, madt.cpu_count,
                 static_cast<unsigned long>(madt.lapic_base_phys));
    for (uint32_t i = 0; i < madt.cpu_count; ++i) {
        klog::printf("[acpi] cpu[%u] apic_id=%u\n", i, madt.apic_ids[i]);
    }

    constexpr uint64_t k_default_lapic_base = 0xFEE00000ull;
    arch_x86_64::lapic_init(madt_ok ? madt.lapic_base_phys : k_default_lapic_base);
    klog::printf("[smp] BSP apic_id=%u\n", arch_x86_64::lapic_id());

    // MADT를 못 찾았어도(madt_ok==false) BSP 자신은 항상 "온라인 코어
    // 1개"다 — bring_up_aps()는 BSP 등록도 함께 겸하므로(smp.cpp) 이
    // 경우엔 BSP 하나짜리 최소 madt_result를 직접 구성해 그대로
    // 넘긴다(AP 기동 루프는 cpu_count=1이라 아무 것도 더 하지 않는다).
    arch_x86_64::madt_result effective = madt;
    if (!madt_ok) {
        effective.cpu_count = 1;
        effective.apic_ids[0] = arch_x86_64::lapic_id();
    }
    arch_x86_64::bring_up_aps(effective);
    klog::printf("[smp] online_cpu_count=%u\n", arch_x86_64::online_cpu_count());
}

// handle_table(64 엔트리, 엔트리마다 intrusive_list 센티널 포함)은
// 전역으로 두지 않는다 — ADR-118에서 확인했듯 이런 중첩 타입은
// 컴파일러가 "동적 초기화 필요"로 판단하기 쉬운데(이 자유freestanding
// 빌드엔 그걸 실행할 crt0가 없다), mm이 이미 초기화된 뒤 명시적으로
// 호출되는 이 함수 안에서 mm::alloc_pages + placement new로 만들면
// 그 문제 자체가 발생하지 않는다(ADR-010의 "명시적 init 함수로 지연
// 초기화" 원칙과 일치 — 그리고 스택에 두기엔 너무 크다, 8KiB 부트
// 스택 예산 대비).
object::handle_table* create_handle_table() {
    constexpr uint32_t k_order = 2;  // 16KiB — sizeof(handle_table) 여유 있게 담김
    auto page = mm::alloc_pages(k_order, 0);
    if (!page.is_ok()) {
        return nullptr;
    }
    void* mem = mm::phys_to_virt(page.value());
    return new (mem) object::handle_table();
}

void demo_object_model() {
    constexpr uint32_t k_all_rights = 0b111;
    constexpr uint32_t k_read_only = 0b001;

    // "프로세스 A"/"프로세스 B" 역할을 흉내낸다 — 아직 procsrv(M8
    // 이후)가 없어 실제 프로세스는 없다. objects.md §3~6의 핸들
    // 테이블/프록시/cascade revoke 메커니즘 자체가 스펙대로 동작하는지
    // 커널 내부에서 직접 호출해 확인한다(M1~M3와 같은 self-test 패턴).
    object::handle_table* table_a = create_handle_table();
    object::handle_table* table_b = create_handle_table();
    if (table_a == nullptr || table_b == nullptr) {
        klog::printf("[object] handle_table 할당 실패\n");
        return;
    }

    object::thread demo_thread;  // 작아서(list_hook 하나뿐) 스택도 안전하다.

    auto owner = table_a->create_owner(object::object_kind::thread, k_all_rights, &demo_thread);
    klog::printf("[object] create_owner ok=%u handle=%u\n", owner.is_ok(),
                 owner.is_ok() ? owner.value() : 0);
    if (!owner.is_ok()) {
        return;
    }
    object::handle owner_h = owner.value();

    auto owner_info = table_a->handle_info(owner_h);
    klog::printf("[object] owner handle_info ok=%u kind=%u rights=%u\n", owner_info.is_ok(),
                 owner_info.is_ok() ? static_cast<uint32_t>(owner_info.value().kind) : 0,
                 owner_info.is_ok() ? owner_info.value().rights : 0);

    // A -> B로 프록시 위임, 권한을 read-only로 축소(ADR-029: 부모의
    // 부분집합만 허용됨을 확인).
    auto proxy = table_a->create_proxy(owner_h, k_read_only, *table_b, /*badge_override=*/0,
                                        /*has_badge_override=*/false);
    klog::printf("[object] create_proxy ok=%u handle=%u\n", proxy.is_ok(),
                 proxy.is_ok() ? proxy.value() : 0);
    if (!proxy.is_ok()) {
        return;
    }
    object::handle proxy_h = proxy.value();

    auto proxy_info = table_b->handle_info(proxy_h);
    klog::printf("[object] proxy handle_info ok=%u kind=%u rights=%u (expect rights=%u)\n",
                 proxy_info.is_ok(),
                 proxy_info.is_ok() ? static_cast<uint32_t>(proxy_info.value().kind) : 0,
                 proxy_info.is_ok() ? proxy_info.value().rights : 0, k_read_only);

    // 소유 핸들을 닫으면 cascade revoke로 프록시도 함께 무효화되어야
    // 한다(objects.md §6) — 프록시는 다른 테이블(table_b)에 있다는
    // 점에 주의.
    auto close_result = table_a->close(owner_h);
    klog::printf("[object] close(owner) ok=%u\n", close_result.is_ok());

    auto proxy_info_after = table_b->handle_info(proxy_h);
    klog::printf("[object] proxy handle_info after owner close: ok=%u (expect 0 — cascade revoke)\n",
                 proxy_info_after.is_ok());

    auto close_again = table_a->close(owner_h);
    klog::printf(
        "[object] close(owner) again: is_err=%u error=%u (expect already_closed=%u)\n",
        close_again.is_err(), static_cast<uint32_t>(close_again.error()),
        static_cast<uint32_t>(object::handle_error::already_closed));
}

void demo_page_table() {
    auto root = arch_x86_64::create_address_space_root();
    klog::printf("[pgtbl] create_address_space_root ok=%u\n", root.is_ok());
    if (!root.is_ok()) {
        return;
    }
    uint64_t as_root = root.value();

    auto backing_page = mm::alloc_pages(0, 0);
    if (!backing_page.is_ok()) {
        klog::printf("[pgtbl] no page available for demo mapping\n");
        return;
    }

    constexpr uint64_t k_demo_virt = 0x0000700000000000ull;  // 유저 영역(하위 절반) 임의 주소

    auto map_result = arch_x86_64::map_page(
        as_root, k_demo_virt, backing_page.value(),
        arch_x86_64::page_perm::write | arch_x86_64::page_perm::user);
    klog::printf("[pgtbl] map_page ok=%u\n", map_result.is_ok());

    arch_x86_64::page_query_result q1 = arch_x86_64::query_page(as_root, k_demo_virt);
    klog::printf("[pgtbl] query after map: present=%u phys=0x%lx write=%u user=%u exec=%u\n",
                 q1.present, static_cast<unsigned long>(q1.phys),
                 arch_x86_64::has_perm(q1.perm, arch_x86_64::page_perm::write),
                 arch_x86_64::has_perm(q1.perm, arch_x86_64::page_perm::user),
                 arch_x86_64::has_perm(q1.perm, arch_x86_64::page_perm::exec));

    // 다시 매핑하면 already_mapped여야 한다.
    auto remap_result = arch_x86_64::map_page(as_root, k_demo_virt, backing_page.value(),
                                               arch_x86_64::page_perm::user);
    klog::printf("[pgtbl] remap same addr: is_err=%u (expect 1, already_mapped)\n",
                 remap_result.is_err());

    // read-only로 낮춘다(COW clone이 실제로 구현될 때 쓸 프리미티브,
    // page_table.hpp 상단 주석 참고).
    arch_x86_64::protect_page(as_root, k_demo_virt, arch_x86_64::page_perm::user);
    arch_x86_64::page_query_result q2 = arch_x86_64::query_page(as_root, k_demo_virt);
    klog::printf("[pgtbl] query after protect(read-only): write=%u (expect 0)\n",
                 arch_x86_64::has_perm(q2.perm, arch_x86_64::page_perm::write));

    arch_x86_64::unmap_page(as_root, k_demo_virt);
    arch_x86_64::page_query_result q3 = arch_x86_64::query_page(as_root, k_demo_virt);
    klog::printf("[pgtbl] query after unmap: present=%u (expect 0)\n", q3.present);
}

// M5(scheduler.md §1~3) — 커널 스레드 2개가 yield()로 번갈아 실행됨을
// 시리얼 로그로 확인한다(kernel-bootstrap.md M5의 완료 기준). 각
// 스레드는 정해진 횟수만큼 돌고 나서 sched::exit()로 영구 종료한다 —
// sched::start() 이후로는 커널 스레드들만 남고 kernel_main으로는
// 돌아오지 않는다.
//
// **M8에서 바뀐 부분**: M6/M7까지는 각자 "전체 스레드 수보다 넉넉히"
// yield()한 뒤 스스로 무한 hlt 루프로 들어가는 flush_yield() 관례를
// 썼다. M8에서 이 방식이 근본적으로 취약함이 드러났다 — 서로 다른
// 스레드가 완료까지 필요로 하는 총 yield 횟수가 다르면(예: 이 데모의
// "I"처럼 사전 작업이 없는 스레드는 16번만에 끝나지만, "A"/"B"는 반복
// 3회의 yield가 추가로 필요하다), **가장 적게 필요한 스레드가 가장
// 먼저 끝나 hlt로 들어가 버리는 바로 그 순간에 자신이 "현재 실행
// 중"이었다면**, 아직 안 끝난 다른 스레드가 run_queue에 아무리 남아
// 있어도 아무도 그들을 깨워 줄 수 없어(타이머 인터럽트가 없어 hlt는
// 영원히 안 돌아온다) 기계 전체가 멈춰 버린다. M8에서 유저 스레드
// (initrun)가 처음 생기면서 정확히 이 경합이 재현 가능하게 실제로
// 발생했다 — 대신 sched::exit()(kernel/core/sched/scheduler.hpp 상단
// 주석)로 스레드 수·필요 yield 횟수에 무관하게 항상 정확한, 스케줄러
// 자신이 보장하는 종료로 바꿨다.
constexpr int k_sched_demo_iterations = 3;

void thread_a_entry() {
    for (int i = 0; i < k_sched_demo_iterations; ++i) {
        klog::printf("[sched] thread A iteration %d\n", i);
        sched::yield();
    }
    klog::printf("[sched] thread A done\n");
    sched::exit();
}

void thread_b_entry() {
    for (int i = 0; i < k_sched_demo_iterations; ++i) {
        klog::printf("[sched] thread B iteration %d\n", i);
        sched::yield();
    }
    klog::printf("[sched] thread B done\n");
    sched::exit();
}

// M9(smp-fpu-bringup.md, ADR-127) — 서로 다른 두 스레드가 xmm0에 넣어 둔
// 값이 yield()를 여러 차례 거쳐도 서로 오염되지 않음을 확인한다(다른
// 스레드가 그사이 자기 xmm0 값을 쓰기 때문에, FXSAVE/FXRSTOR 없이는
// 반드시 깨진다). 커널 전체는 -mno-sse로 컴파일되므로(CMakeLists.txt
// 상단 주석 — CR4.OSFXSR을 아직 설정하지 않았던 시절의 안전장치, M9
// 이후에도 커널 전역에 SSE 코드생성을 허용할지는 별개 결정이라 그대로
// 둔다) 이 두 함수만 target 속성으로 개별적으로 SSE 코드생성을
// 허용한다.
__attribute__((target("sse2"))) void thread_fpu_a_entry() {
    constexpr uint64_t k_pattern = 0xAAAAAAAAAAAAAAAAull;
    asm volatile("movq %0, %%xmm0" : : "r"(k_pattern) : "xmm0");

    bool all_preserved = true;
    for (int i = 0; i < k_sched_demo_iterations; ++i) {
        sched::yield();
        uint64_t readback;
        asm volatile("movq %%xmm0, %0" : "=r"(readback));
        bool ok = (readback == k_pattern);
        all_preserved = all_preserved && ok;
        klog::printf("[fpu] thread A iteration %d xmm0 preserved=%u\n", i, ok);
    }
    klog::printf("[fpu] thread A done all_preserved=%u\n", all_preserved);
    sched::exit();
}

__attribute__((target("sse2"))) void thread_fpu_b_entry() {
    constexpr uint64_t k_pattern = 0x5555555555555555ull;
    asm volatile("movq %0, %%xmm0" : : "r"(k_pattern) : "xmm0");

    bool all_preserved = true;
    for (int i = 0; i < k_sched_demo_iterations; ++i) {
        sched::yield();
        uint64_t readback;
        asm volatile("movq %%xmm0, %0" : "=r"(readback));
        bool ok = (readback == k_pattern);
        all_preserved = all_preserved && ok;
        klog::printf("[fpu] thread B iteration %d xmm0 preserved=%u\n", i, ok);
    }
    klog::printf("[fpu] thread B done all_preserved=%u\n", all_preserved);
    sched::exit();
}

// M6(ipc.md §3~5) — 커널 스레드 2개(서버/클라이언트) 사이에
// Call → Recv → Reply 왕복이 성공함을 시리얼 로그로 확인한다
// (kernel-bootstrap.md M6의 완료 기준). 핸들 테이블 하나를 공유한다 —
// M4 데모와 같은 이유(아직 프로세스/procsrv가 없어 스레드마다 별도
// 테이블을 가질 이유가 없다)로 "커널 컨텍스트 하나"로 취급한다.
object::handle_table* g_ipc_table = nullptr;
object::handle g_ipc_recv_handle = object::k_invalid_handle;  // 서버용: CAN_RECV만
object::handle g_ipc_send_handle = object::k_invalid_handle;  // 클라이언트용: CAN_SEND만 + 커스텀 badge

constexpr uint64_t k_ipc_demo_badge = 0xCAFEull;

bool demo_ipc_setup() {
    g_ipc_table = create_handle_table();
    if (g_ipc_table == nullptr) {
        return false;
    }

    void* ep_mem = mm::slab_alloc(sizeof(object::endpoint));
    if (ep_mem == nullptr) {
        return false;
    }
    auto* ep = new (ep_mem) object::endpoint();

    auto owner = g_ipc_table->create_owner(
        object::object_kind::endpoint, object::k_right_can_send | object::k_right_can_recv, ep);
    if (!owner.is_ok()) {
        return false;
    }

    // 서버용: CAN_RECV만 남긴 프록시(ADR-029 — 원본 권한의 부분집합만).
    auto recv_proxy =
        g_ipc_table->create_proxy(owner.value(), object::k_right_can_recv, *g_ipc_table,
                                   /*badge_override=*/0, /*has_badge_override=*/false);
    // 클라이언트용: CAN_SEND만 남기고 커스텀 badge를 붙인 프록시 —
    // objects.md §3의 "재위임 가능한 마스터가 새 badge를 붙이는 시점"을
    // 흉내낸다. 서버는 이 badge를 sys_recv의 반환값으로 그대로 받아야
    // 한다(아래 thread_c_server_entry에서 확인).
    auto send_proxy =
        g_ipc_table->create_proxy(owner.value(), object::k_right_can_send, *g_ipc_table,
                                   k_ipc_demo_badge, /*has_badge_override=*/true);
    if (!recv_proxy.is_ok() || !send_proxy.is_ok()) {
        return false;
    }

    g_ipc_recv_handle = recv_proxy.value();
    g_ipc_send_handle = send_proxy.value();
    return true;
}

void thread_c_server_entry() {
    ipc::message in{};
    auto recv_result = ipc::sys_recv(*g_ipc_table, g_ipc_recv_handle, in);
    klog::printf(
        "[ipc] server sys_recv ok=%u badge=0x%lx (expect 0x%lx) label=0x%x regs0=%lu\n",
        recv_result.is_ok(), static_cast<unsigned long>(recv_result.is_ok() ? recv_result.value() : 0),
        static_cast<unsigned long>(k_ipc_demo_badge), in.label,
        static_cast<unsigned long>(in.regs[0]));

    ipc::message out{};
    out.label = 0x5EED;
    out.regs[0] = in.regs[0] + 1;
    ipc::sys_reply(out);
    klog::printf("[ipc] server sys_reply sent\n");

    sched::exit();
}

void thread_d_client_entry() {
    ipc::message out{};
    out.label = 0x1234;
    out.regs[0] = 41;

    ipc::message in{};
    auto call_result = ipc::sys_call(*g_ipc_table, g_ipc_send_handle, out, in);
    klog::printf(
        "[ipc] client sys_call ok=%u reply_label=0x%x (expect 0x5eed) reply_regs0=%lu (expect 42)\n",
        call_result.is_ok(), in.label, static_cast<unsigned long>(in.regs[0]));

    sched::exit();
}

// M7(ipc.md §4/§7) — 페이지 1개를 copy 모드로, 핸들 1개를 두 스레드
// 사이에 전달하고, notification으로 한 스레드가 다른 스레드를 깨우는
// 것까지 확인한다(kernel-bootstrap.md M7 완료 기준: "1페이지 데이터를
// 두 스레드 사이에 copy 모드로 전달 성공"). M6 데모(C/D, endpoint 1개)와
// 섞이지 않도록 endpoint를 하나 더 둔다 — handle_table은 계속 공유
// (M4/M6과 같은 "커널 컨텍스트 하나" 단순화).
object::handle g_ipc2_recv_handle = object::k_invalid_handle;
object::handle g_ipc2_send_handle = object::k_invalid_handle;
object::handle g_notify_owner_handle = object::k_invalid_handle;  // G가 H에게 넘길 핸들
object::handle g_notify_send_handle = object::k_invalid_handle;   // I가 직접 notify할 핸들(같은 객체)

uint64_t g_page_source_phys = 0;
uint64_t g_page_dest_phys = 0;

constexpr uint32_t k_page_pattern_seed = 0x11223344u;
constexpr uint64_t k_notify_bits = 0x2ull;

bool demo_ipc2_setup() {
    void* ep_mem = mm::slab_alloc(sizeof(object::endpoint));
    if (ep_mem == nullptr) {
        return false;
    }
    auto* ep = new (ep_mem) object::endpoint();
    auto owner = g_ipc_table->create_owner(
        object::object_kind::endpoint, object::k_right_can_send | object::k_right_can_recv, ep);
    if (!owner.is_ok()) {
        return false;
    }
    auto recv_proxy = g_ipc_table->create_proxy(owner.value(), object::k_right_can_recv,
                                                 *g_ipc_table, 0, false);
    auto send_proxy = g_ipc_table->create_proxy(owner.value(), object::k_right_can_send,
                                                 *g_ipc_table, 0, false);
    if (!recv_proxy.is_ok() || !send_proxy.is_ok()) {
        return false;
    }
    g_ipc2_recv_handle = recv_proxy.value();
    g_ipc2_send_handle = send_proxy.value();

    void* n_mem = mm::slab_alloc(sizeof(object::notification));
    if (n_mem == nullptr) {
        return false;
    }
    auto* n = new (n_mem) object::notification();
    // objects.md는 notification 전용 rights 비트를 정의하지 않는다 —
    // 0으로 둔다(어차피 sys_notify/sys_wait는 rights를 검사하지 않는다).
    auto notify_owner = g_ipc_table->create_owner(object::object_kind::notification, 0, n);
    auto notify_proxy = g_ipc_table->create_proxy(notify_owner.value(), 0, *g_ipc_table, 0, false);
    if (!notify_owner.is_ok() || !notify_proxy.is_ok()) {
        return false;
    }
    g_notify_owner_handle = notify_owner.value();  // 이걸 G가 H에게 IPC로 위임한다.
    g_notify_send_handle = notify_proxy.value();   // I는 이 프록시로 직접 notify한다.

    auto src_page = mm::alloc_pages(0, 0);
    auto dst_page = mm::alloc_pages(0, 0);
    if (!src_page.is_ok() || !dst_page.is_ok()) {
        return false;
    }
    g_page_source_phys = src_page.value();
    g_page_dest_phys = dst_page.value();

    // 소스 페이지에 알려진 패턴을 써 둔다 — 목적지 페이지는 일부러
    // 건드리지 않는다(진짜 복사됐는지 나중에 값으로 구분하기 위해).
    auto* src_words = static_cast<uint32_t*>(mm::phys_to_virt(g_page_source_phys));
    for (size_t i = 0; i < mm::k_page_size / sizeof(uint32_t); ++i) {
        src_words[i] = k_page_pattern_seed + static_cast<uint32_t>(i);
    }

    return true;
}

void thread_g_sender_entry() {
    ipc::message out{};
    out.page_count = 1;
    out.pages[0] = {reinterpret_cast<uint64_t>(mm::phys_to_virt(g_page_source_phys)),
                    mm::k_page_size, ipc::transfer_mode::copy};
    out.handle_count = 1;
    out.handles[0] = {g_notify_owner_handle, 0xFFFFFFFFu};

    ipc::message in{};
    auto call_result = ipc::sys_call(*g_ipc_table, g_ipc2_send_handle, out, in);
    klog::printf("[ipc2] sender sys_call ok=%u ack_label=0x%x\n", call_result.is_ok(), in.label);

    sched::exit();
}

void thread_h_receiver_entry() {
    ipc::message in{};
    in.page_count = 1;
    in.pages[0] = {reinterpret_cast<uint64_t>(mm::phys_to_virt(g_page_dest_phys)), mm::k_page_size,
                   ipc::transfer_mode::copy};

    auto recv_result = ipc::sys_recv(*g_ipc_table, g_ipc2_recv_handle, in);

    auto* dst_words = static_cast<uint32_t*>(mm::phys_to_virt(g_page_dest_phys));
    bool content_ok = true;
    for (size_t i = 0; i < mm::k_page_size / sizeof(uint32_t); ++i) {
        if (dst_words[i] != k_page_pattern_seed + static_cast<uint32_t>(i)) {
            content_ok = false;
            break;
        }
    }

    auto handle_info = g_ipc_table->handle_info(in.handles[0].src_handle);
    klog::printf(
        "[ipc2] receiver sys_recv ok=%u page_count=%u content_ok=%u handle_count=%u "
        "received_handle_kind=%u (expect notification=%u)\n",
        recv_result.is_ok(), in.page_count, content_ok, in.handle_count,
        handle_info.is_ok() ? static_cast<uint32_t>(handle_info.value().kind) : 0xFF,
        static_cast<uint32_t>(object::object_kind::notification));

    ipc::message ack{};
    ack.label = 0xACC0;
    ipc::sys_reply(ack);

    // 방금 IPC로 받은 새 핸들(원본과 다른 핸들 번호지만 같은 객체를
    // 가리킴)로 직접 기다린다 — 핸들 위임이 "진짜로 쓸 수 있는"
    // 핸들을 만들어 냈음을 보여준다.
    auto wait_result = ipc::sys_wait(*g_ipc_table, in.handles[0].src_handle);
    klog::printf("[ipc2] receiver sys_wait ok=%u bits=0x%lx (expect 0x%lx)\n",
                 wait_result.is_ok(),
                 static_cast<unsigned long>(wait_result.is_ok() ? wait_result.value() : 0),
                 static_cast<unsigned long>(k_notify_bits));

    sched::exit();
}

void thread_i_notifier_entry() {
    auto notify_result = ipc::sys_notify(*g_ipc_table, g_notify_send_handle, k_notify_bits);
    klog::printf("[ipc2] notifier sys_notify ok=%u\n", notify_result.is_ok());

    sched::exit();
}

// M8(boot.md §4/§6, kernel-bootstrap.md) — initrd에서 initrun ELF를
// 찾아 로드하고, 새 주소공간·핸들 테이블을 가진 유저 스레드로 진입시킨다.
// init/initrun/main.cpp 상단 주석과 짝을 이루는 값 — 두 실행파일이
// 서로 다른 컴파일 단위라 공유 헤더로 두지 않고 각자 정의했다(레이블
// 값 자체는 uapi.hpp의 syscall ABI와 달리 이 데모 하나만의 관례라
// kernel/include로 옮길 만큼의 재사용 가치가 없다고 판단했다).
constexpr uint32_t k_initrun_boot_label = 0xB007;

object::handle g_initrun_boot_recv_handle = object::k_invalid_handle;  // 커널(수신) 쪽 핸들

// initrun이 syscall로 보낸 부팅 성공 알림을 받는다 — 이 스레드가
// initrun main.cpp 상단 주석이 말하는 "유일한 출력 관찰 수단"이다.
void thread_initrun_boot_server_entry() {
    ipc::message in{};
    auto recv_result = ipc::sys_recv(*g_ipc_table, g_initrun_boot_recv_handle, in);
    klog::printf(
        "[initrun] kernel received boot call ok=%u label=0x%x (expect 0x%x) - 부팅 성공\n",
        recv_result.is_ok(), in.label, k_initrun_boot_label);

    ipc::message ack{};
    ack.label = 0xB0A0;
    ipc::sys_reply(ack);

    sched::exit();
}

// initrd 파싱 + ELF 로드 + 새 주소공간/핸들 테이블/유저 스레드 생성까지
// 전부 이 함수 하나가 맡는다(§4의 1~4단계). 실패하면 nullptr — 호출자가
// klog로 이미 각 단계를 보고했으므로 추가 보고 없이 그냥 포기한다.
object::thread* setup_initrun_process() {
    // 1) endpoint 하나 — 커널(수신) 쪽은 g_ipc_table에 소유 핸들로 둔다
    //    (M6/M7과 같은 "커널 컨텍스트" 관례 — 이 엔드포인트의 서버는
    //    실제로 커널 스레드다).
    void* ep_mem = mm::slab_alloc(sizeof(object::endpoint));
    if (ep_mem == nullptr) {
        return nullptr;
    }
    auto* ep = new (ep_mem) object::endpoint();
    auto owner = g_ipc_table->create_owner(
        object::object_kind::endpoint, object::k_right_can_send | object::k_right_can_recv, ep);
    if (!owner.is_ok()) {
        return nullptr;
    }
    auto recv_proxy = g_ipc_table->create_proxy(owner.value(), object::k_right_can_recv,
                                                 *g_ipc_table, 0, false);
    if (!recv_proxy.is_ok()) {
        return nullptr;
    }
    g_initrun_boot_recv_handle = recv_proxy.value();

    // 2) initrun 전용 handle_table — 이 테이블에 넣는 첫 핸들이 반드시
    //    1이 되도록(handle_table.cpp::allocate_slot이 1부터 채운다) 이
    //    CAN_SEND 프록시를 가장 먼저, 유일하게 만든다 — init/initrun/
    //    main.cpp의 k_boot_endpoint_handle=1 고정 관례가 여기서 성립한다.
    object::handle_table* initrun_handles = create_handle_table();
    if (initrun_handles == nullptr) {
        return nullptr;
    }
    auto send_proxy = g_ipc_table->create_proxy(owner.value(), object::k_right_can_send,
                                                 *initrun_handles, 0, false);
    if (!send_proxy.is_ok() || send_proxy.value() != 1) {
        klog::printf("[initrun] boot send handle != 1 (got %u) - main.cpp 고정 관례 위반\n",
                     send_proxy.is_ok() ? send_proxy.value() : 0);
        return nullptr;
    }

    // 3) initrd(§5, MCPACK v1)에서 "initrun" 엔트리를 찾는다.
    uint64_t initrd_size =
        static_cast<uint64_t>(g_embedded_initrd_end - g_embedded_initrd_start);
    auto entry = initrd::find_entry(g_embedded_initrd_start, initrd_size, "initrun");
    klog::printf("[initrun] mcpack find_entry ok=%u size=0x%lx\n", entry.is_ok(),
                 static_cast<unsigned long>(entry.is_ok() ? entry.value().size : 0));
    if (!entry.is_ok()) {
        return nullptr;
    }

    // 4) 새 주소공간 — ADR-074: initrun은 무조건 trusted=true(시스템의
    //    유일한 최초 신뢰 루트, boot.md §4 3단계).
    auto root = arch_x86_64::create_address_space_root();
    if (!root.is_ok()) {
        return nullptr;
    }
    uint64_t pml4_phys = root.value();

    void* space_mem = mm::slab_alloc(sizeof(object::address_space));
    if (space_mem == nullptr) {
        return nullptr;
    }
    auto* space = new (space_mem) object::address_space();
    space->trusted = true;
    space->page_table_root = pml4_phys;

    auto load_result = arch_x86_64::load_elf(pml4_phys, entry.value().data, entry.value().size);
    klog::printf("[initrun] load_elf ok=%u entry=0x%lx\n", load_result.is_ok(),
                 static_cast<unsigned long>(load_result.is_ok() ? load_result.value() : 0));
    if (!load_result.is_ok()) {
        return nullptr;
    }

    // 5) 유저 스택(16KiB) — demo_page_table()과 같은 유저 영역 임의
    //    주소대(하위 절반)를 쓴다.
    constexpr uint64_t k_user_stack_top = 0x0000700000000000ull;
    constexpr uint32_t k_user_stack_pages = 4;
    for (uint32_t i = 0; i < k_user_stack_pages; ++i) {
        auto page = mm::alloc_pages(0, 0);
        if (!page.is_ok()) {
            return nullptr;
        }
        uint64_t vaddr = k_user_stack_top - (k_user_stack_pages - i) * mm::k_page_size;
        auto mapped = arch_x86_64::map_page(
            pml4_phys, vaddr, page.value(),
            arch_x86_64::page_perm::write | arch_x86_64::page_perm::user);
        if (!mapped.is_ok()) {
            return nullptr;
        }
    }

    // 6) boot_info 한 페이지를 읽기전용으로 매핑하고 그 유저 가상주소를
    //    RDI로 넘긴다(§6). 이 마일스톤은 아직 진짜 Multiboot2 boot_info가
    //    없어(GRUB 부재, ADR-114) self-test 값을 그대로 싣는다 —
    //    memory_map_addr 등 물리주소 필드는 유저 쪽에서 아직 무의미하다
    //    (init/initrun/main.cpp는 이 내용을 읽지 않는다, 위 주석 참고).
    constexpr uint64_t k_boot_info_user_vaddr = 0x0000700000001000ull;
    auto bi_page = mm::alloc_pages(0, 0);
    if (!bi_page.is_ok()) {
        return nullptr;
    }
    const boot::memory_region* bi_regions = nullptr;
    boot::boot_info bi = arch_x86_64::run_boot_info_self_test(&bi_regions);
    void* bi_virt = mm::phys_to_virt(bi_page.value());
    __builtin_memset(bi_virt, 0, mm::k_page_size);
    __builtin_memcpy(bi_virt, &bi, sizeof(bi));
    auto bi_mapped =
        arch_x86_64::map_page(pml4_phys, k_boot_info_user_vaddr, bi_page.value(),
                               arch_x86_64::page_perm::user);  // write 비트 없음 = 읽기전용.
    if (!bi_mapped.is_ok()) {
        return nullptr;
    }

    return sched::create_user_thread(load_result.value(), k_user_stack_top,
                                      k_boot_info_user_vaddr, space, initrun_handles);
}

[[noreturn]] void demo_sched() {
    sched::init();
    arch_x86_64::install_syscall_entry();  // M8 — 첫 유저 스레드가 뜨기 전에 STAR/LSTAR/FMASK를 설정해 둔다.

    object::thread* a =
        sched::create_kernel_thread(&thread_a_entry, object::priority_band::kernel, 0);
    object::thread* b =
        sched::create_kernel_thread(&thread_b_entry, object::priority_band::kernel, 0);
    klog::printf("[sched] create_kernel_thread a=%u b=%u\n", a != nullptr, b != nullptr);

    object::thread* fpu_a =
        sched::create_kernel_thread(&thread_fpu_a_entry, object::priority_band::kernel, 0);
    object::thread* fpu_b =
        sched::create_kernel_thread(&thread_fpu_b_entry, object::priority_band::kernel, 0);
    klog::printf("[fpu] create_kernel_thread a=%u b=%u\n", fpu_a != nullptr, fpu_b != nullptr);

    bool ipc_ready = demo_ipc_setup();
    klog::printf("[ipc] setup ok=%u\n", ipc_ready);

    object::thread* c = nullptr;
    object::thread* d = nullptr;
    if (ipc_ready) {
        c = sched::create_kernel_thread(&thread_c_server_entry, object::priority_band::kernel, 0);
        d = sched::create_kernel_thread(&thread_d_client_entry, object::priority_band::kernel, 0);
    }

    bool ipc2_ready = demo_ipc2_setup();
    klog::printf("[ipc2] setup ok=%u\n", ipc2_ready);

    object::thread* g = nullptr;
    object::thread* hh = nullptr;
    object::thread* ii = nullptr;
    if (ipc2_ready) {
        g = sched::create_kernel_thread(&thread_g_sender_entry, object::priority_band::kernel, 0);
        hh = sched::create_kernel_thread(&thread_h_receiver_entry, object::priority_band::kernel,
                                          0);
        ii = sched::create_kernel_thread(&thread_i_notifier_entry, object::priority_band::kernel,
                                          0);
    }

    // M8 — g_ipc_table이 demo_ipc_setup()에서 이미 만들어진 뒤라야
    // setup_initrun_process()가 그 위에 boot endpoint를 만들 수 있다.
    object::thread* k = nullptr;
    object::thread* initrun = nullptr;
    if (ipc_ready) {
        k = sched::create_kernel_thread(&thread_initrun_boot_server_entry,
                                         object::priority_band::kernel, 0);
        initrun = setup_initrun_process();
    }
    klog::printf("[initrun] setup_initrun_process ok=%u\n", initrun != nullptr);

    if (a != nullptr) {
        sched::enqueue(*a);
    }
    if (b != nullptr) {
        sched::enqueue(*b);
    }
    if (fpu_a != nullptr) {
        sched::enqueue(*fpu_a);
    }
    if (fpu_b != nullptr) {
        sched::enqueue(*fpu_b);
    }
    if (c != nullptr) {
        sched::enqueue(*c);
    }
    if (d != nullptr) {
        sched::enqueue(*d);
    }
    if (g != nullptr) {
        sched::enqueue(*g);
    }
    if (hh != nullptr) {
        sched::enqueue(*hh);
    }
    if (ii != nullptr) {
        sched::enqueue(*ii);
    }
    if (k != nullptr) {
        sched::enqueue(*k);
    }
    if (initrun != nullptr) {
        sched::enqueue(*initrun);
    }

    // 여기서부터는 절대 돌아오지 않는다 — 이후로는 위 스레드들 사이의
    // yield()/sys_call/sys_recv/sys_reply/sys_notify/sys_wait/SYSCALL로만
    // 제어가 옮겨간다(M8부터 initrun은 유저모드에서 SYSCALL로 들어온다).
    sched::start();
}

}  // namespace

extern "C" [[noreturn]] void kernel_main() {
    klog::init();
    klog::printf("hello from kernel\n");

    // M10(smp-fpu-bringup.md, ADR-055) — IDT를 가장 먼저 건다. 이후의
    // 모든 예외(원인 불명 정지 포함)가 catch-all로 진단 가능해진다.
    arch_x86_64::init_idt();

    // M9(ADR-127) — 첫 arch_context_switch(=첫 FXSAVE/FXRSTOR, demo_sched()
    // 안에서 발생)보다 반드시 먼저 호출해야 한다. CR4.OSFXSR 없이
    // FXSAVE/FXRSTOR를 실행하면 #UD.
    arch_x86_64::init_fpu();

    uint64_t real_arch_data_addr = demo_mm();
    demo_smp(real_arch_data_addr);
    demo_object_model();
    demo_page_table();
    demo_sched();
}
