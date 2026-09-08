// x86_64 커널 진입점(higher-half, docs/spec/virtual-memory-layout.md §2.1
// 3단계에서 boot.S의 _start64가 여기로 점프한다).
//
// docs/plan/kernel-bootstrap.md M1: QEMU 시리얼 콘솔에 "hello from
// kernel" 출력. M2: boot_info(Multiboot2 태그 파싱) 파이프라인. M3:
// mm(물리 페이지 할당자 + 슬랩 힙) 초기화·왕복 확인. M4: 핸들 테이블
// (objects.md) + 페이지테이블 조작 API 왕복 확인. M5: 커널 스레드
// 2개가 협조적으로 번갈아 실행됨을 확인. M6: 커널 스레드 2개 사이의
// Call → Recv → Reply 왕복 확인.
#include "boot_info.hpp"
#include "boot_info_x86_64.hpp"
#include "klog.hpp"
#include "page_table.hpp"

#include <cstdint>

#include <ipc/endpoint.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <mm/slab.hpp>
#include <object/handle_table.hpp>
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

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

void demo_mm() {
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
// 스레드는 정해진 횟수만큼 돌고 나서 무한 hlt 루프로 들어간다 —
// sched::start() 이후로는 커널 스레드들만 남고 kernel_main으로는
// 돌아오지 않으므로, 이 데모의 마지막 스레드가 사실상 그 전까지
// kernel_main이 하던 "idle" 역할을 이어받는다.
constexpr int k_sched_demo_iterations = 3;

void thread_a_entry() {
    for (int i = 0; i < k_sched_demo_iterations; ++i) {
        klog::printf("[sched] thread A iteration %d\n", i);
        sched::yield();
    }
    klog::printf("[sched] thread A done\n");
    for (;;) {
        asm volatile("hlt");
    }
}

void thread_b_entry() {
    for (int i = 0; i < k_sched_demo_iterations; ++i) {
        klog::printf("[sched] thread B iteration %d\n", i);
        sched::yield();
    }
    klog::printf("[sched] thread B done\n");
    for (;;) {
        asm volatile("hlt");
    }
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

    // sys_reply는 클라이언트를 run_queue에 다시 넣기만 할 뿐, 이
    // 협조적 스케줄러(M5)는 아무도 yield()하지 않으면 그 클라이언트를
    // 영영 스케줄하지 않는다 — 여기서 곧장 hlt 루프로 들어가면
    // 클라이언트가 응답을 확인하는 로그를 볼 기회 자체가 사라진다
    // (실제로 이 문제로 한 번 멈춰서 확인했다). 한 번 양보해 다른
    // 스레드가 최소 한 차례는 더 돌 기회를 준다.
    sched::yield();

    for (;;) {
        asm volatile("hlt");
    }
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

    for (;;) {
        asm volatile("hlt");
    }
}

[[noreturn]] void demo_sched() {
    sched::init();

    object::thread* a =
        sched::create_kernel_thread(&thread_a_entry, object::priority_band::kernel, 0);
    object::thread* b =
        sched::create_kernel_thread(&thread_b_entry, object::priority_band::kernel, 0);
    klog::printf("[sched] create_kernel_thread a=%u b=%u\n", a != nullptr, b != nullptr);

    bool ipc_ready = demo_ipc_setup();
    klog::printf("[ipc] setup ok=%u\n", ipc_ready);

    object::thread* c = nullptr;
    object::thread* d = nullptr;
    if (ipc_ready) {
        c = sched::create_kernel_thread(&thread_c_server_entry, object::priority_band::kernel, 0);
        d = sched::create_kernel_thread(&thread_d_client_entry, object::priority_band::kernel, 0);
    }

    if (a != nullptr) {
        sched::enqueue(*a);
    }
    if (b != nullptr) {
        sched::enqueue(*b);
    }
    if (c != nullptr) {
        sched::enqueue(*c);
    }
    if (d != nullptr) {
        sched::enqueue(*d);
    }

    // 여기서부터는 절대 돌아오지 않는다 — 이후로는 위 스레드들 사이의
    // yield()/sys_call/sys_recv/sys_reply로만 제어가 옮겨간다.
    sched::start();
}

}  // namespace

extern "C" [[noreturn]] void kernel_main() {
    klog::init();
    klog::printf("hello from kernel\n");

    demo_mm();
    demo_object_model();
    demo_page_table();
    demo_sched();
}
