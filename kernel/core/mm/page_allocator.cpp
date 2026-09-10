// 물리 페이지 할당자 구현 (docs/spec/memory.md §2~4). 알려진 단순화는
// page_allocator.hpp 상단 주석 참고.
#include "mm/page_allocator.hpp"
#include "mm/phys_map.hpp"

#include <k/irq_safe.hpp>  // scoped_lock

namespace kern::mm {

namespace {

per_node_pool g_node_pools[k_max_numa_nodes];
per_cpu_cache g_cpu_caches[k_max_cpus];
uint32_t g_node_count = 0;
bool g_initialized = false;

// M12(ADR-016) — order-0 프레임 참조 카운트 테이블. init()이 usable
// 영역에서 본 최대 물리주소를 기준으로 딱 한 번 할당한다(frame_add_ref
// 등 참고, page_allocator.hpp).
uint32_t* g_frame_refcount = nullptr;
uint64_t g_frame_refcount_entries = 0;

uint32_t frame_index(uint64_t physical_address) {
    uint64_t idx = physical_address / k_page_size;
    if (g_frame_refcount == nullptr || idx >= g_frame_refcount_entries) {
        LIBK_PANIC("kern::mm::frame_*: physical_address out of refcount table range");
    }
    return static_cast<uint32_t>(idx);
}

// M11(ADR-054) — set_node_distance()가 실제로 호출되기 전까지는
// false로 남아, alloc_pages()의 노드 폴백이 M1~M10과 완전히 동일한
// 순서(라운드로빈)를 그대로 쓴다(has_distance_table 참고).
uint8_t g_node_distance[k_max_numa_nodes][k_max_numa_nodes] = {};
bool g_has_distance_table = false;

// M1~M8은 BSP 단일 코어(ADR-035) — 실제 코어 식별(APIC ID 등)은
// 스케줄러가 등장하는 M5 이후 과제다.
uint32_t current_cpu_id() { return 0; }

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }

// addr 위치의 물리 메모리에 page_frame 헤더를 써 넣고 free_lists[order]의
// 새 머리로 만든다. 호출자가 pool.lock을 들고 있어야 한다.
void push_free_block(per_node_pool& pool, uint32_t node_id, uint64_t addr, uint32_t order) {
    auto* frame = static_cast<page_frame*>(phys_to_virt(addr));
    frame->physical_address = addr;
    frame->numa_node = node_id;
    frame->order = order;
    frame->next = pool.free_lists[order];
    pool.free_lists[order] = frame;
}

// free_lists[order]에서 물리주소가 addr인 블록을 찾아 제거한다(있었으면
// true). 버디 병합(free_to_node_pool)에 쓰인다. 호출자가 pool.lock을
// 들고 있어야 한다.
bool remove_from_free_list(per_node_pool& pool, uint32_t order, uint64_t addr) {
    page_frame** link = &pool.free_lists[order];
    while (*link != nullptr) {
        if ((*link)->physical_address == addr) {
            *link = (*link)->next;
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}

// free_lists[order] 이상에서 가장 작은 비어있지 않은 order를 찾아
// 하나 꺼내고, 요청한 order까지 분할(split)한다. 호출자가 pool.lock을
// 들고 있어야 한다.
bool try_alloc_from_pool_locked(per_node_pool& pool, uint32_t node_id, uint32_t order,
                                 alloc_flags flags, uint64_t& out_addr) {
    uint32_t found_order = order;
    while (found_order <= k_max_order && pool.free_lists[found_order] == nullptr) {
        ++found_order;
    }
    if (found_order > k_max_order) {
        return false;
    }

    uint64_t found_block_size = static_cast<uint64_t>(k_page_size) << found_order;
    if (!has_flag(flags, alloc_flags::allow_reserve) &&
        pool.free_bytes - found_block_size < pool.reserved_bytes) {
        // ADR-104: 예약분을 침범하는 일반 할당은 이 단계에서 실패로 취급한다.
        return false;
    }

    page_frame* block = pool.free_lists[found_order];
    pool.free_lists[found_order] = block->next;
    uint64_t addr = block->physical_address;

    while (found_order > order) {
        --found_order;
        uint64_t buddy_addr = addr + (static_cast<uint64_t>(k_page_size) << found_order);
        push_free_block(pool, node_id, buddy_addr, found_order);
    }

    pool.free_bytes -= (static_cast<uint64_t>(k_page_size) << order);
    out_addr = addr;
    return true;
}

// [base, base+length) 범위를 정렬·크기 조건을 만족하는 가장 큰
// order 블록들로 잘라 free_lists에 채운다(memory.md §3 초기화).
void add_free_region(per_node_pool& pool, uint32_t node_id, uint64_t base, uint64_t length) {
    uint64_t start = align_up(base, k_page_size);
    uint64_t end = align_down(base + length, k_page_size);
    if (end <= start) {
        return;
    }

    uint64_t addr = start;
    while (addr < end) {
        uint64_t remaining = end - addr;
        uint32_t order = 0;
        while (order < k_max_order) {
            uint64_t next_block_size = static_cast<uint64_t>(k_page_size) << (order + 1);
            if (next_block_size > remaining || (addr % next_block_size) != 0) {
                break;
            }
            ++order;
        }
        uint64_t block_size = static_cast<uint64_t>(k_page_size) << order;
        push_free_block(pool, node_id, addr, order);
        pool.total_bytes += block_size;
        pool.free_bytes += block_size;
        addr += block_size;
    }
}

struct exclusion_range {
    uint64_t base;
    uint64_t length;
};

// add_free_region과 같지만, exclusions에 나열된 범위와 겹치는 부분은
// 잘라내고 나머지만 채운다 — boot_info의 usable 영역이 커널 자신/initrd
// 물리 범위와 겹칠 수 있어(memory.md는 이 둘을 별도 타입 엔트리로만
// 표시할 뿐 usable 엔트리에서 자동으로 빼주지 않는다) init()이 직접
// 빼야 한다. 그러지 않으면 실행 중인 커널 위에 페이지를 내줄 수 있다.
void add_free_region_excluding(per_node_pool& pool, uint32_t node_id, uint64_t base, uint64_t length,
                                const exclusion_range* exclusions, size_t exclusion_count) {
    if (exclusion_count == 0) {
        add_free_region(pool, node_id, base, length);
        return;
    }

    const exclusion_range& excl = exclusions[0];
    uint64_t end = base + length;
    uint64_t excl_end = excl.base + excl.length;

    if (excl.length == 0 || excl_end <= base || excl.base >= end) {
        // 이 exclusion은 이 범위와 안 겹친다 — 다음 exclusion으로.
        add_free_region_excluding(pool, node_id, base, length, exclusions + 1, exclusion_count - 1);
        return;
    }
    if (excl.base > base) {
        add_free_region_excluding(pool, node_id, base, excl.base - base, exclusions + 1,
                                   exclusion_count - 1);
    }
    if (excl_end < end) {
        add_free_region_excluding(pool, node_id, excl_end, end - excl_end, exclusions + 1,
                                   exclusion_count - 1);
    }
}

// order>0 블록은 어느 노드에서 왔는지 호출자가 넘겨주지 않는다
// (memory.md §4의 free_pages 시그니처 그대로) — page_frame이 "free
// 상태에서만 유효"(§2)라 반납 전에는 소속 노드를 조회할 데이터베이스가
// 없다. M1~M8은 노드가 1개뿐이라(ADR-035) 0으로 고정해도 관찰 가능한
// 차이가 없다. 실제로 노드가 여러 개 붙는 시점(M9 이후)에는 물리주소→
// 노드 조회 수단이 필요해진다 — 이 함수가 그 지점이다.
void free_to_node_pool(uint32_t node_id, uint64_t physical_address, uint32_t order) {
    per_node_pool& pool = g_node_pools[node_id];
    scoped_lock<spinlock> guard(pool.lock);

    uint64_t addr = physical_address;
    uint32_t cur_order = order;
    while (cur_order < k_max_order) {
        uint64_t block_size = static_cast<uint64_t>(k_page_size) << cur_order;
        uint64_t buddy_addr = addr ^ block_size;
        if (!remove_from_free_list(pool, cur_order, buddy_addr)) {
            break;
        }
        addr = (addr < buddy_addr) ? addr : buddy_addr;
        ++cur_order;
    }
    push_free_block(pool, node_id, addr, cur_order);
    pool.free_bytes += (static_cast<uint64_t>(k_page_size) << order);
}

void drain_cpu_cache(per_cpu_cache& cache) {
    uint32_t drain_count = cache.local_free_count / 2;
    for (uint32_t i = 0; i < drain_count; ++i) {
        page_frame* block = cache.local_free_list;
        cache.local_free_list = block->next;
        --cache.local_free_count;
        free_to_node_pool(0, block->physical_address, 0);
    }
}

// per-cpu 캐시는 특정 스레드 요청이 아니라 "이 코어가 자주 쓰는
// order-0 페이지" 용도라 preferred_node 개념이 아직 없다 — 노드 0부터
// 채운다(스케줄러가 등장하는 M5 이후 thread.preferred_node와 연결될
// 수 있다).
void refill_cpu_cache(per_cpu_cache& cache) {
    constexpr uint32_t k_refill_count = k_per_cpu_cache_limit / 2;
    for (uint32_t i = 0; i < k_refill_count; ++i) {
        uint64_t addr = 0;
        bool ok = false;
        for (uint32_t node_id = 0; node_id < g_node_count; ++node_id) {
            per_node_pool& pool = g_node_pools[node_id];
            scoped_lock<spinlock> guard(pool.lock);
            if (try_alloc_from_pool_locked(pool, node_id, 0, alloc_flags::none, addr)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            break;  // 풀도 고갈 — 지금까지 채운 만큼만 쓴다.
        }
        auto* frame = static_cast<page_frame*>(phys_to_virt(addr));
        frame->physical_address = addr;
        frame->next = cache.local_free_list;
        cache.local_free_list = frame;
        ++cache.local_free_count;
    }
}

// alloc_pages()가 시도할 노드 순서를 order_out[0..g_node_count-1]에
// 채운다. order_out[0]은 항상 preferred_node다. g_has_distance_table이
// false면(set_node_distance 미호출, M1~M10과 동일) 나머지는 노드
// 번호 순 라운드로빈 — 기존 동작을 그대로 보존한다. true면(ADR-054)
// preferred_node에서 가까운 노드부터 순서대로 채운다(제자리 선택
// 정렬 — 노드 개수가 k_max_numa_nodes=8 이하로 작아 O(n^2)도 충분히
// 빠르다).
void build_fallback_order(uint32_t preferred_node, uint32_t* order_out) {
    order_out[0] = preferred_node;
    if (!g_has_distance_table) {
        for (uint32_t attempt = 1; attempt < g_node_count; ++attempt) {
            order_out[attempt] = (preferred_node + attempt) % g_node_count;
        }
        return;
    }

    uint32_t count = 0;
    for (uint32_t n = 0; n < g_node_count; ++n) {
        if (n != preferred_node) {
            order_out[1 + count] = n;
            ++count;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < count; ++j) {
            uint32_t node_j = order_out[1 + j];
            uint32_t node_best = order_out[1 + best];
            if (g_node_distance[preferred_node][node_j] < g_node_distance[preferred_node][node_best]) {
                best = j;
            }
        }
        if (best != i) {
            uint32_t tmp = order_out[1 + i];
            order_out[1 + i] = order_out[1 + best];
            order_out[1 + best] = tmp;
        }
    }
}

bool try_alloc_from_cpu_cache(uint64_t& out_addr) {
    per_cpu_cache& cache = g_cpu_caches[current_cpu_id()];
    if (cache.local_free_list == nullptr) {
        refill_cpu_cache(cache);
        if (cache.local_free_list == nullptr) {
            return false;
        }
    }
    page_frame* block = cache.local_free_list;
    cache.local_free_list = block->next;
    --cache.local_free_count;
    out_addr = block->physical_address;
    return true;
}

}  // namespace

void init(const boot::boot_info& info, const boot::memory_region* regions) {
    if (g_initialized) {
        LIBK_PANIC("kern::mm::init called twice");
    }

    g_node_count = info.numa_node_count;
    if (g_node_count == 0) {
        g_node_count = 1;
    }
    if (g_node_count > k_max_numa_nodes) {
        g_node_count = k_max_numa_nodes;
    }

    // 커널 자신/initrd/AP 트램폴린 스크래치 페이지(M10, ADR-055)가
    // 차지한 물리 범위(boot_info_x86_64.cpp의 append_owned_regions가
    // 표시)는 usable 엔트리와 겹칠 수 있다 — add_free_region_excluding이
    // 그 겹침을 잘라낸다.
    exclusion_range exclusions[3];
    size_t exclusion_count = 0;
    for (uint32_t i = 0; i < info.memory_map_count && exclusion_count < 3; ++i) {
        const boot::memory_region& r = regions[i];
        if (r.type == boot::k_region_kernel_image || r.type == boot::k_region_initrd_image) {
            exclusions[exclusion_count++] = exclusion_range{r.base, r.length};
        }
    }

    for (uint32_t i = 0; i < info.memory_map_count; ++i) {
        const boot::memory_region& r = regions[i];
        if (r.type != boot::k_region_usable) {
            continue;
        }
        uint32_t node_id = r.node_id;
        if (node_id >= g_node_count) {
            node_id = 0;
        }
        add_free_region_excluding(g_node_pools[node_id], node_id, r.base, r.length, exclusions,
                                   exclusion_count);
    }

    g_initialized = true;

    // M12(ADR-016) — usable 영역 중 가장 높은 물리주소를 기준으로
    // 프레임 참조 카운트 테이블을 order-10(최대 4MiB, 4GiB 물리메모리
    // 상당) 이내로 확보한다. alloc_pages를 여기서 쓰므로 위에서
    // g_initialized를 이미 true로 만들어 둬야 한다.
    uint64_t max_phys = 0;
    for (uint32_t i = 0; i < info.memory_map_count; ++i) {
        const boot::memory_region& r = regions[i];
        if (r.type != boot::k_region_usable) {
            continue;
        }
        uint64_t end = r.base + r.length;
        if (end > max_phys) {
            max_phys = end;
        }
    }

    uint64_t entry_count = align_up(max_phys, k_page_size) / k_page_size;
    uint64_t bytes_needed = entry_count * sizeof(uint32_t);
    uint64_t pages_needed = align_up(bytes_needed, k_page_size) / k_page_size;
    uint32_t order = 0;
    while ((1ull << order) < pages_needed) {
        ++order;
        if (order > k_max_order) {
            LIBK_PANIC("kern::mm::init: physical memory too large for frame refcount table");
        }
    }

    auto table_page = alloc_pages(order, 0);
    if (!table_page.is_ok()) {
        LIBK_PANIC("kern::mm::init: no memory for frame refcount table");
    }
    g_frame_refcount = static_cast<uint32_t*>(phys_to_virt(table_page.value()));
    g_frame_refcount_entries = entry_count;
    for (uint64_t i = 0; i < entry_count; ++i) {
        g_frame_refcount[i] = 0;
    }
}

result<uint64_t, alloc_error> alloc_pages(uint32_t order, uint32_t preferred_node,
                                           alloc_flags flags) {
    if (!g_initialized) {
        LIBK_PANIC("kern::mm::alloc_pages called before kern::mm::init");
    }
    if (order > k_max_order) {
        LIBK_PANIC("kern::mm::alloc_pages: order exceeds k_max_order");
    }
    if (preferred_node >= g_node_count) {
        return result<uint64_t, alloc_error>::err(alloc_error::invalid_node);
    }

    if (order == 0) {
        uint64_t addr;
        if (try_alloc_from_cpu_cache(addr)) {
            return result<uint64_t, alloc_error>::ok(addr);
        }
    }

    // preferred_node부터 폴백한다(§4 4단계) — set_node_distance()가
    // 호출된 적 있으면(M11, ADR-054) 가까운 노드 순서, 아니면 M1~M10과
    // 동일한 노드 번호 순 라운드로빈이다. 노드 1개(M1~M8)에서는 이
    // 루프가 1회로 끝난다.
    uint32_t fallback_order[k_max_numa_nodes];
    build_fallback_order(preferred_node, fallback_order);
    for (uint32_t attempt = 0; attempt < g_node_count; ++attempt) {
        uint32_t node_id = fallback_order[attempt];
        per_node_pool& pool = g_node_pools[node_id];
        uint64_t addr;
        bool ok;
        {
            scoped_lock<spinlock> guard(pool.lock);
            ok = try_alloc_from_pool_locked(pool, node_id, order, flags, addr);
        }
        if (ok) {
            return result<uint64_t, alloc_error>::ok(addr);
        }
    }

    // ADR-105/106(회수·블로킹)은 아직 구현하지 않았다 — blocking
    // 플래그를 받아들이지만 현재는 none과 동일하게 즉시 실패한다.
    (void)flags;
    return result<uint64_t, alloc_error>::err(alloc_error::out_of_memory);
}

void free_pages(uint64_t physical_address, uint32_t order) {
    if (!g_initialized) {
        LIBK_PANIC("kern::mm::free_pages called before kern::mm::init");
    }
    if (order > k_max_order) {
        LIBK_PANIC("kern::mm::free_pages: order exceeds k_max_order");
    }

    if (order == 0) {
        // M12(ADR-016) — COW로 공유 중인 프레임이면(frame_release가
        // false) 아직 다른 소유자가 있으므로 실제로 반환하지 않는다
        // (공유 카운트는 frame_release가 이미 내부적으로 줄여 뒀다).
        // 호출자 쪽(주소공간 매핑 해제)이 이미 매핑 자체는 지운
        // 뒤이므로 여기서는 그냥 리턴하면 된다.
        if (g_frame_refcount != nullptr && !frame_release(physical_address)) {
            return;
        }

        per_cpu_cache& cache = g_cpu_caches[current_cpu_id()];
        auto* frame = static_cast<page_frame*>(phys_to_virt(physical_address));
        frame->physical_address = physical_address;
        frame->next = cache.local_free_list;
        cache.local_free_list = frame;
        ++cache.local_free_count;

        if (cache.local_free_count > k_per_cpu_cache_limit) {
            drain_cpu_cache(cache);
        }
        return;
    }

    free_to_node_pool(0, physical_address, order);
}

uint32_t node_count() { return g_node_count; }

pool_stats stats(uint32_t node) {
    if (node >= g_node_count) {
        LIBK_PANIC("kern::mm::stats: invalid node");
    }
    const per_node_pool& pool = g_node_pools[node];
    return pool_stats{pool.total_bytes, pool.free_bytes, pool.reserved_bytes};
}

void set_node_distance(uint32_t node_count_arg, const uint8_t* distance) {
    if (node_count_arg != g_node_count || node_count_arg > k_max_numa_nodes) {
        return;  // 호출자가 잘못된 크기를 줬다 — 기존 순서를 그대로 유지.
    }
    for (uint32_t i = 0; i < node_count_arg; ++i) {
        for (uint32_t j = 0; j < node_count_arg; ++j) {
            g_node_distance[i][j] = distance[i * node_count_arg + j];
        }
    }
    g_has_distance_table = true;
}

void frame_add_ref(uint64_t physical_address) {
    uint32_t idx = frame_index(physical_address);
    ++g_frame_refcount[idx];
}

bool frame_release(uint64_t physical_address) {
    uint32_t idx = frame_index(physical_address);
    if (g_frame_refcount[idx] == 0) {
        return true;
    }
    --g_frame_refcount[idx];
    return false;
}

uint32_t frame_ref_count(uint64_t physical_address) {
    return g_frame_refcount[frame_index(physical_address)];
}

}  // namespace kern::mm
