# 완료 보고: smp-fpu-bringup M11 — 다중 코어/다중 NUMA 실환경 검증 + 락 순서 규칙 문서화

**대상 계획**: [smp-fpu-bringup.md](../plan/smp-fpu-bringup.md) §M11
**관련 결정**: ADR-036/052/053/054([kernel-memory.md](../design/kernel-memory.md)/[kernel-scheduler.md](../design/kernel-scheduler.md)),
ADR-136(kernel-memory.md, 실제 락 순서 표), ADR-137(kernel-scheduler.md,
워크 스틸링 범위 정정 — scheduler.md §3.1의 기존 표현이 ADR-014와
모순됨을 이번에 발견해 정정)
**실행일**: 2026-09-09

## 완료 기준 달성 확인

M11의 목표(계획 §M11): "다중 코어·다중 NUMA QEMU 구성에서 기존
M1~M8 스모크 테스트 전체 + M9(FPU)/M10(SMP/IPI) 검증이 함께 통과하고,
kernel-memory.md에 락 순서 표가 반영된다."

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh          # 기존(SMP/NUMA 미설정) — 39개 전부 PASS
tools/smoke-test-smp-x86_64.sh      # M10(-smp 4, 단일 노드) — 11개 전부 PASS
tools/smoke-test-numa-x86_64.sh     # M11(-smp 4 -numa 2, 신규) — 24개 전부 PASS
```

`smoke-test-numa-x86_64.sh`는 QEMU `-numa`로 노드 2개(코어 0,1→노드0,
코어 2,3→노드1)를 구성해, ACPI SRAT/SLIT 파싱부터 M1~M9의 기존 검증
문자열 전체(핸들 테이블, 페이지테이블, IPC, IPC2, 스케줄러, FPU,
initrun)까지 회귀 없이 통과함을 확인한다.

## 수행한 작업

### 1. ACPI SRAT/SLIT 파싱 (M10의 RSDP 경로 재사용)

[kernel/arch/x86_64/acpi.cpp](../../kernel/arch/x86_64/acpi.cpp) —
`find_and_parse_srat_slit()`가 M10이 이미 만든 RSDP 검색(EBDA/BIOS
ROM 스캔 또는 `arch_data_addr`)을 그대로 재사용해 SRAT(Processor
Local APIC Affinity + Memory Affinity 엔트리)과 SLIT(거리 행렬)을
찾는다. proximity domain 값을 그대로 노드 번호로 쓴다(QEMU
`-numa node,nodeid=N`이 그 값을 그대로 SRAT에 싣는 것을 실측
확인) — 재압축하지 않는다.

### 2. SRAT 메모리 어피니티를 실제 boot_info로 채택

[kernel/arch/x86_64/boot_info_x86_64.cpp](../../kernel/arch/x86_64/boot_info_x86_64.cpp)::`build_numa_boot_info()` —
SRAT의 Memory Affinity 엔트리가 있으면(즉 `-numa`로 노드별
`memory-backend-ram`이 실제로 구성됐을 때), M1~M10이 계속 쓰던
self-test fixture(가짜 주소, 단일 노드 0) 대신 **이 실제 범위와 노드
태그를 그대로 `mm::init()`에 넘긴다**. `-numa` 미사용 시(`srat_ok==false`
또는 `mem_affinity_count==0`)는 self-test fixture 경로로 완전히
되돌아가 M1~M10과 관찰 가능한 차이가 없다 — M10이 이미 확립한 "opt-in,
기본값 보존"(ADR-125 패턴)을 그대로 따른다.

QEMU로 실측(발췌, `-smp 4 -numa 2`):

```
[numa] srat_ok=1 node_count=2 mem_affinity_count=3
[numa] cpu[0] apic_id=0 node=0
[numa] cpu[2] apic_id=2 node=1
[numa] mem[0] base=0x0 length=0xa0000 node=0
[numa] mem[2] base=0x8000000 length=0x8000000 node=1
[numa] distance[0][1]=20
[mm:init] node[0] total_bytes=0x7f7b000 free_bytes=0x7f7b000 reserved_bytes=0x0
[mm:init] node[1] total_bytes=0x8000000 free_bytes=0x8000000 reserved_bytes=0x0
```

두 노드의 물리 풀이 실제로 분리 동작함(node[1]이 정확히 128MiB로
독립 집계됨)을 확인했다 — 이것이 이 마일스톤의 "가짜가 아닌" 첫
다중 노드 물리 메모리 풀 검증이다.

### 3. ADR-054 — 거리 기반 노드 폴백 (기존 라운드로빈 대체)

[kernel/core/mm/page_allocator.cpp](../../kernel/core/mm/page_allocator.cpp)::`set_node_distance()`/`build_fallback_order()` —
M1~M10은 "선호 노드부터 노드 번호 순 라운드로빈"으로 폴백했다(ADR-054가
이미 "실제 SLIT을 아직 파싱하지 않았다"고 명시한 단순화). M11이 SLIT을
실제로 파싱하게 되면서, 등록된 거리 행렬이 있을 때는 "가까운 노드부터"
순서로 바꿨다 — 등록되지 않았으면(SLIT 미발견) 이전과 완전히 동일한
라운드로빈으로 남는다(선택 정렬 알고리즘 자체는 동일 거리일 때
안정적으로 원래 순서를 보존하지 않지만, "등록 안 됨" 분기를 별도로
분리해 이 문제를 원천적으로 피했다).

**확인하지 못함**: 3개 이상 노드에서 비대칭 거리(예: 노드0-1은 가깝고
노드0-2는 멂)를 실제로 구성해 폴백 순서가 라운드로빈과 달라지는
것까지는 QEMU로 검증하지 않았다 — `tools/run-qemu.sh`의
`MINICORE_QEMU_NUMA` 자동 구성이 항상 균등 거리(모든 노드 쌍
20)로만 노드를 만들기 때문이다. 알고리즘 자체(선택 정렬로 거리
오름차순)는 코드 검토로 정확성을 확인했다.

### 4. ADR-053 워크 스틸링 — 실제 구현 + 범위 정정(ADR-137)

[kernel/core/sched/scheduler.cpp](../../kernel/core/sched/scheduler.cpp)::`pick_next_with_stealing()` —
M1~M10까지는 `yield()`/`block()`/`exit()`/`start()` 전부가 `g_run_queues[0]`
을 하드코딩해서 봤다(주석조차 "M9 이후 실제 다중 코어가 붙으면 조회
절차가 필요해진다"고 예고해 둔 자리). 이번에 `arch_current_node_id()`
(새 HAL 훅, x86_64는 `smp.cpp`가 LAPIC ID→SRAT 표로 답한다)로 "지금
코어의 노드"를 물어 그 노드부터 보고, 비었으면 다른 노드를 훔쳐온다.

**실제로 QEMU에서 확인하는 과정에서 기존 스펙과의 모순을 발견했다**:
[scheduler.md](../spec/scheduler.md) §3.1(M5 시점 작성)은 "워크
스틸링은 user_band만 대상으로 하고 kernel_band는 노드를 넘지 않는다"고
적어 두었다. 이 제약을 그대로 구현했더니, `preferred_node=1`로 만든
데모 스레드(커널 밴드)가 **한 번도 스케줄되지 않았다** — BSP(노드
0)의 `user_band`에 `initrun`이 스케줄링 시작 시점부터 계속 자리를
차지하고 있어(사실상 다시 스케줄되지 않는 유저 스레드,
kernel-bootstrap-m8.md 참고), "내 노드가 완전히 빈다"는 §3.1의
트리거 조건이 실행 내내 한 번도 성립하지 않았기 때문이다. 조사
결과 이 제약이 ADR-014("커널 밴드는 항상 유저 밴드보다 우선한다",
노드 조건 없음)와 실제로 모순됨을 확인해 — ADR-137로 정정하고
`pick_next_with_stealing()`을 "모든 노드의 kernel_band를 먼저 전부
본 뒤에야 어느 노드든 user_band를 본다"는 순서로 구현했다.
[scheduler.md](../spec/scheduler.md) §3.1도 함께 갱신했다.

QEMU로 실측 확인(발췌):

```
[numa-sched] create_kernel_thread node1=1
...
[fpu] thread B done all_preserved=1
[numa-sched] thread on preferred_node=1 ran (stolen if node_count>1)
[initrun] kernel received boot call ok=1 label=0xb007 ...
```

`-numa` 미사용(노드 1개)에서도 이 스레드는 정상 실행된다(`preferred_node`
가 `g_node_count`로 감싸져 노드 0으로 접힌다) — 다만 이 경우
"훔쳐옴" 자체는 관찰할 수 없다(훔쳐올 다른 노드가 없으므로).

부수적으로 발견한 버그: `create_kernel_thread()`가 `mm::alloc_pages()`
에 `preferred_node`를 감싸지 않고 그대로 넘겨, 존재하지 않는 노드를
요청하면(`enqueue()`는 감싸주는데 정작 스택 할당은 실패) 스레드
생성 자체가 조용히 실패했다 — `enqueue()`와 동일하게
`preferred_node % g_node_count`로 감싸 고쳤다.

### 5. 실제 락 순서 표 (ADR-052 구체화, ADR-136)

일반-목적 서브에이전트로 `kernel/core`·`kernel/arch/x86_64`·`libk`
전체의 락 사용처를 조사해 [kernel-memory.md](../design/kernel-memory.md)
ADR-136에 표로 정리했다. 핵심 결론: 중첩(동시에 두 락을 쥐는) 경로는
전체 코드베이스에 **정확히 하나** — `slab_alloc()`이
`size_class_state::lock`을 쥔 채 `grow_locked()`→`mm::alloc_pages()`를
불러 `per_node_pool::lock`을 추가로 잡는다(순서: 슬랩 락→풀 락,
항상 이 방향뿐 — 역전 경로 없음). 그 외 모든 락(`run_queue`,
`endpoint`, `notification`, `klog`)은 리프 락이다.
`pick_next_with_stealing()`이 여러 노드의 `run_queue::lock`을
순회하지만 매번 잠그고-바로 풀고 다음으로 넘어가 두 개를 동시에
쥐는 순간이 없다. `handle_table`은 자체 락이 전혀 없다는 사실도
확인·기록했다 — 지금까지는 전부 논리적으로 1코어에서만 호출돼
안전하지만, M12 이후 실제 다중 프로세스가 동시에 핸들 테이블에
접근하게 되면 반드시 재검토해야 할 결여로 남겨 둔다.

## 검증 결과 (정직하게 보고)

- **확인함**: `-smp 1/2/4` × `-numa` 미사용/2노드 조합 각각에서
  ACPI SRAT/SLIT이 실제 QEMU 데이터를 정확히 파싱함(코어→노드,
  메모리 어피니티, 거리 행렬).
- **확인함**: 2개 NUMA 노드의 물리 메모리 풀이 실제로 분리 집계됨,
  워크 스틸링이 노드 경계를 넘어 실제로 발생함(커널 밴드 데모
  스레드로 확인), `-numa` 미사용 시 M1~M10과 완전히 동일한 동작
  (self-test fixture, 라운드로빈 폴백) 보존.
- **확인함**: 새 스모크 스크립트(`tools/smoke-test-numa-x86_64.sh`)로
  M1~M9의 핵심 검증 문자열 + M10(SMP/IPI) + M11(NUMA/스틸링) 전체가
  하나의 QEMU 부팅에서 함께 통과.
- **확인하지 못함**: 3개 이상 노드에서 비대칭 SLIT 거리로 실제
  폴백 순서가 라운드로빈과 달라지는 것(§3 참고) — 알고리즘은 코드
  검토로만 확인.
- **확인하지 못함**: 워크 스틸링이 실제로 여러 코어에서 "동시에"
  스케줄러를 실행하는 상황(AP가 자기 노드의 run_queue를 직접
  서비스하는 것) — M10 done 보고가 이미 명시했듯 AP는 여전히
  온라인 신호만 보내고 IPI(TLB shootdown)만 처리할 뿐, 협조적
  스케줄러 자체에는 참여하지 않는다. 이번 검증은 "BSP 한 코어가
  여러 노드의 run_queue를 넘나들며 서비스"하는 경로만 확인했다 —
  진짜 "다른 코어가 다른 노드에서 동시에 스케줄러를 돈다"는 계획
  범위 밖으로 남는다(선점 스케줄링·타이머 도입과 함께 재검토될
  주제).
- **확인하지 못함**: aarch64 대응 — 계획 전체가 이미 범위 밖으로
  명시했다.

## 다음 마일스톤과의 접점

- M11b(lazy FPU 전환, ADR-133)가 이 M11에서 확정한 다중 코어
  인프라(AP별 IDTR, 워크 스틸링) 바로 다음 순서다. `g_fpu_owner[core_id]`
  같은 코어별 상태를 다룰 때 이번에 만든 `arch_current_node_id()`류
  패턴(코어별 조회 HAL 훅)을 그대로 재사용할 수 있다.
- `handle_table`의 무동기화 상태(§5 마지막 문단)는 M12(procsrv) 착수
  시점에 반드시 재검토해야 한다 — 이 ADR-136이 그 결여를 처음으로
  명시적으로 기록해 둔다.
