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

// M11(smp-fpu-bringup.md §M11, ADR-054) — ACPI SLIT(x86_64) 등에서 얻은
// 노드 간 거리 행렬을 등록한다. distance[i*node_count+j] = 노드 i에서
// 노드 j까지의 거리(값이 작을수록 가깝다, ACPI 관례상 로컬=10). 등록
// 전까지 alloc_pages()의 노드 폴백은 M1~M10과 동일한 순서(노드 번호
// 순 라운드로빈)를 그대로 쓴다 — 이 함수를 부르는 순간부터만 "가까운
// 노드부터"(ADR-054) 순서로 바뀐다. node_count는 mm::node_count()와
// 같아야 한다(다르면 무시하고 기존 순서를 유지).
void set_node_distance(uint32_t node_count, const uint8_t* distance);

// M12(system-servers-bringup.md §M12, ADR-016) — fork()의 COW(Copy-on-
// Write) 지원. order-0(4KiB) 프레임 단위로만 추적한다 — COW는 항상
// 페이지 단위 매핑을 다루므로 더 큰 order를 추적할 필요가 없다(order>0
// 블록은 절대 이 API를 거치지 않는다, arch_x86_64::clone_address_space_cow
// 참고).
//
// 값의 의미: 0 = "공유되지 않음"(정상적으로 alloc_pages가 막 내준 새
// 프레임의 기본 상태 — 실소유자가 정확히 하나뿐). 1 이상 = "나 말고
// 이만큼 더 있다"(실소유자 수는 이 값+1) — COW 클론이 프레임을 다른
// 주소공간과 공유하게 만들 때마다 1씩 늘어난다.
//
// mm::init() 시점에 보이는 최대 물리주소를 기준으로 크기를 정해
// order-10(4MiB) 블록 하나에 담는다 — 이 상한(4GiB 물리 메모리까지
// 추적 가능, uint32_t 엔트리 기준)을 넘는 시스템은 이 프로젝트의
// QEMU 개발 규모(수백 MiB~수 GiB)를 크게 벗어나므로 LIBK_PANIC한다.
void frame_add_ref(uint64_t physical_address);

// 이 프레임을 하나의 소유자가 놓는다. 반환값이 true면 "나 말고는
// 아무도 없었다" — 호출자가 실제로 free_pages(phys, 0)까지 마쳐야
// 한다는 뜻이다. false면 이미 다른 소유자가 있어(공유 카운트를 이
// 함수가 이미 1 줄여 뒀다) 호출자는 free_pages를 부르면 **안 된다**
// (그 소유자를 대신 반환해 버리는 이중 반환 버그가 된다) — 매핑만
// 제거하는 걸로 끝난다. COW 쓰기 폴트 핸들러도 이 반환값으로 "내가
// 마지막 소유자면 복사 없이 그냥 쓰기 권한만 다시 켠다"를 판단한다.
bool frame_release(uint64_t physical_address);

// 현재 "나 말고 이만큼 더 있다" 값을 그냥 조회한다(변경 없음).
uint32_t frame_ref_count(uint64_t physical_address);

}  // namespace mm
