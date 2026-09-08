# 메모리 관리 스펙

**관련 결정**: ADR-010, ADR-012, ADR-024, ADR-033, ADR-036
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`kernel/core/mm`)
**관련 스펙**: [boot.md](boot.md) §3(`memory_region`, `numa_node_count`, `cpu_node_map`)

이 문서는 물리 페이지 할당자, 커널 힙, 프로세스 메모리 쿼터를
구현 가능한 수준으로 정의한다. ADR-012(전통적 슬랩/페이지 할당자),
ADR-033(락-프리+per-CPU 우선), ADR-036(NUMA 인지)을 하나의 자료구조로
합친다.

## 1. 계층 구조

```
per_cpu_cache (코어 로컬, 락 없음, order-0 전용)
      ↑ 고갈 시 보충 / 과잉 시 반납
per_node_pool (NUMA 노드당 1개, 스핀락 1개, buddy 할당자)
      ↑ boot_info.memory_map으로 초기화
물리 메모리 (boot_info.memory_map_addr, boot.md §3)
```

`boot_info.numa_node_count`(§boot.md 3)가 1이면(M1~M8 실행 환경,
ADR-035) 이 계층은 그대로 유지되지만 `per_node_pool` 배열의 크기가
1이 되어 사실상 "전역 buddy 할당자 하나"로 동작한다 — 별도의
단일 노드 전용 코드 경로를 두지 않는다.

## 2. 자료구조

```cpp
static constexpr uint32_t k_max_order       = 10;  // order N = 4KB << N (최대 4MB 블록)
static constexpr uint32_t k_max_numa_nodes  = 8;   // 골격 상한, 필요 시 조정
static constexpr uint32_t k_max_cpus        = 64;  // 골격 상한, 필요 시 조정
static constexpr uint32_t k_per_cpu_cache_limit = 32; // order-0 페이지 개수

struct page_frame {
    uint64_t    physical_address;
    uint32_t    numa_node;
    uint32_t    order;
    page_frame* next;   // free list 내에서만 유효
};

struct per_node_pool {
    spinlock    lock;                          // ADR-033: 노드당 락 1개
    page_frame* free_lists[k_max_order + 1];   // buddy 자유 목록, order별
    uint64_t    total_bytes;
    uint64_t    free_bytes;
};

struct per_cpu_cache {
    page_frame* local_free_list;   // order-0 전용, 이 코어만 접근 (락 없음)
    uint32_t    local_free_count;
};

per_node_pool  g_node_pools[k_max_numa_nodes];   // ADR-034/036
per_cpu_cache  g_cpu_caches[k_max_cpus];          // ADR-033
```

## 3. 초기화

1. `boot_info.memory_map`(boot.md §3)을 순회하며 `type == Usable`인
   영역을 `memory_region.node_id`별로 해당 `per_node_pool`에 등록한다.
2. `boot_info.numa_node_count`/`cpu_node_map`이 없으면(토폴로지 정보
   없음) 모든 영역과 모든 코어를 노드 0으로 취급한다(boot.md §2).
3. 각 `per_node_pool`은 buddy 초기화 알고리즘으로 `free_lists`를 채운다.

## 4. 할당 API

```cpp
enum class alloc_error : uint32_t {
    out_of_memory,
    quota_exceeded,
    invalid_node,
};

// preferred_node: 요청자의 선호 노드 (scheduler.md의 thread.preferred_node에서 옴, ADR-036)
result<uint64_t, alloc_error> alloc_pages(uint32_t order, uint32_t preferred_node, handle owner_process);
void free_pages(uint64_t physical_address, uint32_t order, handle owner_process);
```

`alloc_pages` 순서:

1. `order == 0`이고 현재 실행 중인 코어의 `per_cpu_cache`에 여유가
   있으면 거기서 즉시 반환한다(락 없음).
2. 그 외의 경우 `owner_process`의 쿼터(§5)를 먼저 확인한다 — 초과 시
   `alloc_error::quota_exceeded`.
3. `preferred_node`의 `per_node_pool.lock`을 잡고 buddy 분할로 할당한다.
4. 3단계가 실패하면(해당 노드 고갈) **거리가 가까운 노드부터 순차로
   자동 폴백**한다(ADR-054) — 노드 간 거리 행렬은 §3에서 확보한
   ACPI SLIT/FDT distance-map 정보를 사용하며, 토폴로지 정보가 없는
   환경(노드 1개)에서는 이 단계가 자명하게 스킵된다. 모든 노드가
   실패해야 `alloc_error::out_of_memory`를 반환한다. 폴백으로 할당된
   페이지는 요청 스레드의 `preferred_node`와 다른 노드에 있을 수
   있다는 점을 호출자가 감안해야 한다.
5. `order == 0` 할당이 반복적으로 같은 코어에서 일어나면 `per_cpu_cache`
   보충(refill)이 일어난다 — 캐시가 `k_per_cpu_cache_limit`의 절반
   이하로 떨어지면 노드 풀에서 그만큼 가져온다.

`free_pages`도 대칭적으로: `order == 0`이면 우선 `per_cpu_cache`로
반납하고, 캐시가 `k_per_cpu_cache_limit`을 넘으면 절반을 노드 풀로
돌려보낸다(drain).

## 5. 프로세스 메모리 쿼터 (ADR-012, ADR-024)

```cpp
struct quota_state {
    uint64_t limit_bytes;    // 정책 서버가 설정 (기본값은 initrun 기동 시 부여)
    uint64_t used_bytes;
};
```

- 모든 프로세스는 `quota_state`를 하나씩 가진다(프로세스 생성 시
  초기화, 부모로부터 상속하지 않음 — 정책 서버가 매번 새로 부여).
- `alloc_pages`는 `used_bytes + requested > limit_bytes`면
  `quota_exceeded`를 반환한다(ADR-012 §영향: `result` 실패로 표현).
- 쿼터 조정 syscall은 정책 서버 전용이다:

```cpp
// caller가 ADR-027과 동일한 방식(정책 서버가 발급한 프록시 핸들)으로
// 인증되어야 한다 — 커널은 "이 호출자가 quota_admin 권한을 가진
// 유효한 핸들을 제시했는가"만 검사하고, 그 핸들을 발급하는 정책은
// 유저랜드(정책 서버)의 몫이다.
result<void, alloc_error> sys_quota_set(handle quota_admin, handle target_process, uint64_t new_limit_bytes);
```

- 정책 서버가 다운된 동안에는 각 프로세스의 쿼터가 **마지막 승인값에
  고정(freeze)**된다(ADR-051) — 새로운 `sys_quota_set` 호출은 정책
  서버가 복구될 때까지 불가능하지만, 이미 설정된 `limit_bytes`로는
  계속 할당이 가능하다. 새 프로세스는 구현 시 정한 기본값 쿼터를 받는다.

## 6. 커널 힙 (슬랩, ADR-012)

```cpp
// 고정 크기 클래스. 실제 목록은 구현 시 프로파일링으로 조정 가능.
static constexpr size_t k_slab_size_classes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

void* slab_alloc(size_t size);   // 가장 가까운 상위 크기 클래스에서 할당
void  slab_free(void* ptr, size_t size);
```

- 각 크기 클래스는 `per_node_pool`(§2)에서 order-0 페이지를 받아
  그 크기로 잘라 쓰는 전통적 슬랩 구조다. 슬랩 자체도 요청한
  스레드의 `preferred_node`(ADR-036)에서 페이지를 받아 지역성을
  지킨다.
- 커널 자신의 내부 자료구조(`handle_entry`, `page_frame` 등)는 이
  슬랩 위에서 할당된다 — 즉 §2의 `page_frame` 구조체 자체도
  재귀적으로 이 힙 위에 있을 수 있으므로, 부트스트랩 단계에서는
  정적 배열(§2의 `g_node_pools`, `g_cpu_caches`)로 시작하고 이후
  동적 확장이 필요해지면 슬랩을 사용한다.

## 7. M1~M8과의 관계

`kernel-bootstrap.md` M3는 이 스펙의 §2~4(할당자 골격)와 §6(슬랩)을
구현 대상으로 한다. §5(쿼터)와 정책 서버 연동은 procsrv/정책 서버가
등장하는 이후 계획의 범위다 — M1~M8에서는 쿼터를 사실상 무제한
(`limit_bytes = UINT64_MAX`)으로 두어 골격만 동작을 확인한다.

## 아직 정하지 않은 것

- 노드 간 거리 행렬을 어떤 자료구조로 커널 내부에 보관할지(인접
  행렬 vs 정렬된 이웃 목록)는 구현 시 정한다.
- 새 프로세스의 기본 쿼터 크기, 슬랩 크기 클래스 목록의 최종 확정.
