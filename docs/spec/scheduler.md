# 스케줄러 스펙

**관련 결정**: ADR-014, ADR-025, ADR-027, ADR-028, ADR-033, ADR-034, ADR-036
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`kernel/core/sched`)
**관련 스펙**: [ipc.md](ipc.md) §5(도네이션), [objects.md](objects.md), [memory.md](memory.md)

## 1. 개요

우선순위는 두 밴드로 나뉜다(ADR-014):

- **kernel 밴드**: 커널 자체 작업·코어 드라이버(ADR-007). 항상 user
  밴드보다 우선한다.
- **user 밴드**: 모든 유저 프로세스. 기본 동일 우선순위, 승격 가능하나
  kernel 밴드를 절대 넘지 못한다.

실행 큐는 코어 단위가 아니라 **NUMA 노드 단위**다(ADR-034).

## 2. 자료구조

```cpp
enum class priority_band : uint32_t { kernel = 0, user = 1 };

struct run_queue {
    spinlock       lock;             // 노드당 1개 (ADR-033)
    intrusive_list kernel_band;      // priority_band::kernel 스레드
    intrusive_list user_band;        // priority_band::user 스레드
};

run_queue g_run_queues[k_max_numa_nodes];   // memory.md §2와 동일 상수, M1~M8에서는 크기 1
```

`thread` 객체(objects.md §2)에 추가되는 스케줄링 필드:

```cpp
struct thread_sched_fields {
    priority_band band;
    uint32_t      preferred_node;     // ADR-034/036. 기본값: 부모 스레드의 노드 상속
    uint32_t      boost_level;        // 0 = 기본. 승격 정도 (ADR-025/027)
    uint64_t      base_time_slice_us;
    // 유효 타임슬라이스 = base_time_slice_us * f(boost_level), f는 §4
};
```

## 3. 스케줄링 알고리즘

각 코어는 기본적으로 자신이 속한 NUMA 노드의 `run_queue`만 본다:

1. `kernel_band`가 비어있지 않으면 그 안에서 라운드로빈으로 다음
   스레드를 고른다. kernel 밴드는 승격 개념이 없으므로 타임슬라이스는
   균일하다.
2. `kernel_band`가 비었으면 `user_band`에서 §4의 가중 라운드로빈으로
   다음 스레드를 고른다.
3. 두 밴드 모두 비면 §3.1의 워크 스틸링을 시도한 뒤에도 가져올
   작업이 없을 때만 코어는 유휴(idle) 상태로 대기한다(인터럽트 또는
   다른 코어의 notification으로 깨어남).

### 3.1 워크 스틸링 (ADR-053)

자기 노드의 두 밴드가 모두 빈 채로 **임계치 시간**(기본값은 구현 시
결정) 이상 유휴 상태가 지속되면, 코어는 다른 NUMA 노드의 `run_queue`
(`user_band`만 — kernel 밴드 스레드는 노드를 넘나들지 않는다)에서
작업을 훔쳐올 수 있다. 임계치 미만의 짧은 유휴는 훔쳐오지 않는다 —
그사이 자기 노드에 새 작업이 들어올 수 있기 때문이다.

- 훔쳐온 스레드의 `preferred_node`는 원래 값을 그대로 유지한다 —
  스틸링은 일회성 구제일 뿐 영구 이주가 아니다. 다음 스케줄링
  기회에 자기 노드로 돌아갈 수 있다.
- 어떤 노드에서 훔쳐올지 고르는 방법(순차 탐색 vs 가장 부하가 큰
  노드 우선)과 정확한 임계치 값은 구현 시 정한다.
- M1~M8 실행 환경(노드 1개, ADR-035)에서는 훔쳐올 다른 노드가
  없으므로 이 절차가 자명하게 스킵된다.

## 4. user 밴드 가중 라운드로빈 (ADR-025)

```cpp
// boost_level → 타임슬라이스 배율. 선형 매핑으로 시작(구현 시 조정 가능).
// 예: multiplier(0) = 1.0, multiplier(N) = 1.0 + N * 0.5, 상한은 아직 미정.
uint64_t effective_time_slice(const thread_sched_fields& t) {
    return t.base_time_slice_us * multiplier(t.boost_level);
}
```

- 순서 자체는 순수 라운드로빈(모든 스레드가 큐를 한 바퀴씩 돈다)을
  유지하되, 각 스레드가 자기 차례에 실행되는 **길이**가 `boost_level`에
  비례한다 — 승격되지 않은 스레드도 반드시 최소 배율(1.0)을 보장받아
  굶주림이 없다.
- `multiplier` 함수의 정확한 형태(선형/계단식)와 상한은 구현 시 정한다.

## 5. 우선순위 승격 (ADR-027)

```cpp
// boost_admin: 정책 서버가 발급한 프록시 핸들 (objects.md §3의 프록시 메커니즘 재사용).
// 이 핸들을 정책 서버가 sys_handle_close하면 승격 권한이 즉시 철회된다
// (objects.md §6의 cascade revoke가 boost_level 재설정까지 트리거해야 함 —
//  구현 시 "핸들 철회 훅"으로 boost_level을 0으로 되돌리는 콜백 필요).
result<void, sched_error> sys_thread_boost(handle boost_admin, handle target_thread, uint32_t new_boost_level);
```

- `new_boost_level`이 커널 밴드에 해당하는 값으로 user 밴드 스레드를
  올리는 것은 허용되지 않는다 — user 밴드는 그 자체로 별도 밴드이며,
  boost_level은 어디까지나 user 밴드 **내부**의 상대적 가중치일 뿐
  kernel 밴드로의 전환을 의미하지 않는다(ADR-014의 하드 상한).

## 6. 도네이션 (ADR-028, ipc.md §5)

`sys_call` 처리 시:

1. 대상 엔드포인트에 `sys_recv` 대기 중인 서버 스레드가 있으면, 그
   스레드의 `boost_level`을 호출자의 `boost_level`로 **일시 교체**하고
   (원래 값은 저장해둔다), 필요하면 자신의 노드 `run_queue`에서
   즉시 실행 가능하도록 위치를 재조정한다.
2. `sys_reply` 시 서버 스레드의 `boost_level`을 저장해둔 원래 값으로
   복원한다.
3. 호출자와 서버가 서로 다른 NUMA 노드에 선호 노드를 가진 경우에도
   서버는 **자신의 노드 run_queue에 그대로 머문다** — 도네이션은
   우선순위만 옮기고 스레드를 다른 노드로 이주시키지 않는다(이주는
   이 스펙의 범위 밖).

## 7. M1~M8과의 관계

`kernel-bootstrap.md` M5는 §1~3(밴드·노드별 큐·기본 라운드로빈)까지만
구현한다. §4(승격 비례 타임슬라이스), §5(승격 syscall), §6(도네이션)은
M6(IPC) 이후 순서대로 채워진다. M1~M8 실행 환경(ADR-035)에서는
`k_max_numa_nodes` 배열이 원소 1개로 동작하므로 관찰되는 동작은 코어
1개짜리 전통적 스케줄러와 같다.

## 아직 정하지 않은 것

- §3.1 워크 스틸링의 정확한 임계치 값과 노드 선택 방법.
- `multiplier` 함수의 최종 형태와 boost_level 상한.
- 서버가 여러 클라이언트를 동시에 처리할 때(다중 `sys_recv` 스레드)의
  대기열 정렬 정책(ipc.md §5에서도 동일 항목 언급).
