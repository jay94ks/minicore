// x86_64 커널 진입점(higher-half, docs/spec/virtual-memory-layout.md §2.1
// 3단계에서 boot.S의 _start64가 여기로 점프한다).
//
// docs/plan/kernel-bootstrap.md M1: QEMU 시리얼 콘솔에 "hello from
// kernel" 출력. M2: boot_info(Multiboot2 태그 파싱) 파이프라인. M3:
// mm(물리 페이지 할당자 + 슬랩 힙) 초기화·왕복 확인. M4: 핸들 테이블
// (objects.md) + 페이지테이블 조작 API 왕복 확인.
#include "boot_info.hpp"
#include "boot_info_x86_64.hpp"
#include "klog.hpp"
#include "page_table.hpp"

#include <cstdint>

#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>
#include <mm/slab.hpp>
#include <object/handle_table.hpp>
#include <object/kernel_objects.hpp>

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

}  // namespace

extern "C" [[noreturn]] void kernel_main() {
    klog::init();
    klog::printf("hello from kernel\n");

    demo_mm();
    demo_object_model();
    demo_page_table();

    for (;;) {
        asm volatile("hlt");
    }
}
