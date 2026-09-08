// 물리 페이지 할당자 (docs/spec/memory.md §1~4, ADR-012/024/033/036).
// per_node_pool(buddy) + per_cpu_cache(order-0 전용, 락 없음) 2단
// 구조. M1~M8은 노드 1개(ADR-035)라 사실상 전역 buddy 할당자 하나로
// 동작하지만, 자료구조 자체는 처음부터 다중 노드를 전제한다.
//
// docs/plan/kernel-bootstrap.md M3 범위의 알려진 단순화:
//   - `handle owner_process` 매개변수와 §5(쿼터) 실제 집행은 넣지
//     않았다 — 아직 objects.md의 handle/process 개념이 없다(M4).
//     quota_state는 구조체만 존재하고 alloc_pages가 참조하지 않는다.
//   - §4 5단계의 회수(ADR-105)·블로킹(ADR-106)은 구현하지 않는다 —
//     그 전 단계(캐시/스왑)가 아직 없다. alloc_flags::blocking은
//     받아들이지만 현재는 none과 동일하게 동작한다(즉시
//     out_of_memory 반환).
//   - 노드 간 폴백(§4 4단계)은 실제 ACPI SLIT/FDT 거리 행렬이 아직
//     없어(파싱 미구현) 노드 인덱스 순서로 대체한다 — 노드가 1개뿐인
//     M1~M8 실행 환경에서는 이 폴백 루프가 어차피 실행되지 않는다.
#pragma once

#include <cstddef>
#include <cstdint>

#include <libk/result.hpp>
#include <libk/spinlock.hpp>

#include "boot_info.hpp"

namespace mm {

constexpr uint32_t k_page_size = 4096;
constexpr uint32_t k_max_order = 10;            // order N = 4KiB << N (최대 4MiB)
constexpr uint32_t k_max_numa_nodes = 8;        // 골격 상한(memory.md §2)
constexpr uint32_t k_max_cpus = 64;             // 골격 상한
constexpr uint32_t k_per_cpu_cache_limit = 32;  // order-0 페이지 개수

enum class alloc_error : uint32_t {
    out_of_memory,
    invalid_node,
};

enum class alloc_flags : uint32_t {
    none = 0,
    blocking = 1u << 0,       // ADR-106 — 현재는 none과 동일(회수 미구현)
    allow_reserve = 1u << 1,  // ADR-104 — reserved_bytes 접근 허용
};

inline alloc_flags operator|(alloc_flags a, alloc_flags b) {
    return static_cast<alloc_flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has_flag(alloc_flags flags, alloc_flags bit) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0;
}

// free 상태인 블록 안에만 유효하다(memory.md §2) — 별도의 전체 물리
// 프레임 데이터베이스 없이, 자유 블록 자신의 메모리(phys_to_virt 경유)에
// 이 헤더를 써 넣는 방식으로 free list를 구성한다. next는 가상주소다
// (역참조해야 하므로).
struct page_frame {
    uint64_t physical_address;
    uint32_t numa_node;
    uint32_t order;
    page_frame* next;
};

struct per_node_pool {
    spinlock lock;
    page_frame* free_lists[k_max_order + 1] = {};
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t reserved_bytes = 0;  // ADR-104 — 기본 0(M3는 예약 정책을 다루지 않는다)
};

// M3는 M1~M8 실행 환경(BSP 단일 코어, ADR-035)에 맞춰 "현재 코어"를
// 항상 0으로 취급한다 — 실제 코어 식별(APIC ID 등)은 스케줄러가
// 등장하는 M5 이후 과제다.
struct per_cpu_cache {
    page_frame* local_free_list = nullptr;
    uint32_t local_free_count = 0;
};

// memory.md §5 — procsrv/정책 서버가 아직 없어(M4 이후) alloc_pages가
// 이 구조체를 참조하지 않는다. 골격만 갖춘다(§7).
struct quota_state {
    uint64_t limit_bytes = UINT64_MAX;
    uint64_t used_bytes = 0;
};

// boot_info의 usable 메모리 영역(k_region_usable)으로 per_node_pool들을
// 초기화한다(memory.md §3). 커널 자신/initrd로 마킹된 영역
// (k_region_kernel_image/k_region_initrd_image)과 그 외 비usable
// 영역은 건너뛴다. 두 번 호출하면 LIBK_PANIC(재초기화는 지원하지 않음).
void init(const boot::boot_info& info, const boot::memory_region* regions);

result<uint64_t, alloc_error> alloc_pages(uint32_t order, uint32_t preferred_node,
                                           alloc_flags flags = alloc_flags::none);
void free_pages(uint64_t physical_address, uint32_t order);

struct pool_stats {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t reserved_bytes;
};

uint32_t node_count();
pool_stats stats(uint32_t node);

}  // namespace mm
