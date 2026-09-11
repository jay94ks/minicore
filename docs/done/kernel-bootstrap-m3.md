# 완료 보고: kernel-bootstrap M3 — libk + 코어 메모리 관리

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M3
**관련 스펙**: [cxx-conventions.md](../spec/cxx-conventions.md) §4, [memory.md](../spec/memory.md)
**관련 결정**: ADR-010, 012, 024, 033, 035, 036, 042, 066~077, 104, 113, 115, 116, 117, 118
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M3는 kernel-bootstrap.md에 "목표: ... 출력" 형태의 단일 완료 기준
문장이 없다(M1/M2와 달리) — 대신 "libk 최소 구현"과 "kernel/core/mm
(할당자 골격+슬랩) 구현"이 완료 기준이다. 둘 다 실제로 동작함을
QEMU와 호스트 네이티브 테스트 양쪽으로 확인했다.

```
# 1) 크로스 빌드 + QEMU 왕복 (mm 초기화·할당·반납·슬랩 확인)
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => 7개 항목 모두 PASS (hello, boot_info self-test 2개, mm 4개)

# 2) libk 호스트 네이티브 단위 테스트 (ADR-077)
cmake -S libk/tests -B build/libk-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build/libk-tests
./build/libk-tests/libk_tests.exe
# => 39/39 checks passed
```

## 수행한 작업

### 1. libk (cxx-conventions.md §4, ADR-066~077)

`libk/include/libk/`(M49/ADR-199 이후 `libs/k/include/k/`로 이동+
`lib` 접두사 제거)에 10개 헤더 전부 작성(ADR-066: 헤더 전용,
전역 스코프, 타입 1개당 헤더 1개):

| 헤더 | 내용 |
|---|---|
| [panic.hpp](../../libs/k/include/k/panic.hpp) | `libk_detail::panic_hook` 선언 + `LIBK_PANIC` 매크로(ADR-067) |
| [result.hpp](../../libs/k/include/k/result.hpp) | `result<T,E>` + `result<void,E>` 특수화(ADR-068) |
| [optional.hpp](../../libs/k/include/k/optional.hpp) | `optional<T>`(ADR-069) |
| [span.hpp](../../libs/k/include/k/span.hpp) | `span<T>`(ADR-070) |
| [intrusive_list.hpp](../../libs/k/include/k/intrusive_list.hpp) | `list_hook` + `intrusive_list<T,Hook>`(ADR-071) |
| [atomic.hpp](../../libs/k/include/k/atomic.hpp) | `atomic<T>`(ADR-072, 아래 ADR-118 참고) |
| [spinlock.hpp](../../libs/k/include/k/spinlock.hpp) | TTAS `spinlock` + `libk_detail::cpu_relax()`(ADR-076) |
| [ticket_lock.hpp](../../libs/k/include/k/ticket_lock.hpp) | FIFO `ticket_lock`(ADR-076) |
| [mcs_lock.hpp](../../libs/k/include/k/mcs_lock.hpp) | `mcs_lock` + `qnode`(ADR-076) |
| [irq_safe.hpp](../../libs/k/include/k/irq_safe.hpp) | `irq_safe<Lock>` + `scoped_lock<Lock>` + `arch_irq_save/restore` 선언(ADR-076) |

**알려진 단순화**: `result<T,E>`의 복사 생성자는 ADR-068이 요구한
"T/E가 둘 다 복사 가능할 때만 SFINAE로 활성화"를 하지 않고 무조건
선언했다 — 복사 불가능한 T/E는 실제로 복사를 *시도할 때만* 컴파일
에러가 난다(템플릿 지연 인스턴스화). 아직 `result<T,E>` 자체의
복사 가능 여부를 질의하는 코드가 없어 관찰 가능한 차이는 없다.

### 2. 호스트 네이티브 단위 테스트 (ADR-077)

[libs/k/tests/](../../libs/k/tests/) — 크로스 빌드 트리와 독립된 별도
CMake 프로젝트. `result`/`optional`/`span`/`intrusive_list`/
`spinlock`/`ticket_lock`/`mcs_lock` 39개 체크. `panic_hook`은
테스트 실패(`abort()`)로, `arch_irq_save/restore`는 no-op으로
치환(ADR-077 §영향이 이미 명시한 한계 그대로).

이 저장소에서는 **Windows에서 CMake 기본 제너레이터가 MSVC를
고르면 실패한다** — libk가 GCC/Clang 공통 컴파일러 내장
(`__atomic_*`, `__is_constructible` 등)을 직접 쓰기 때문이다(ADR-020/072/115).
`-DCMAKE_CXX_COMPILER=clang++`를 명시해야 한다(`libk/tests/CMakeLists.txt`
주석에 기록).

### 3. kernel/core/mm — 물리 페이지 할당자 (memory.md §1~4)

- [kernel/core/mm/page_allocator.hpp](../../kernel/core/mm/page_allocator.hpp)/[.cpp](../../kernel/core/mm/page_allocator.cpp) —
  `per_node_pool`(buddy, order 0~10) + `per_cpu_cache`(order-0 전용,
  락 없음) 2단 구조. `page_frame` 헤더는 **자유 블록 자신의 물리
  메모리에** 써 넣는다(memory.md §2 "next는 free list 내에서만
  유효"를 그대로 구현 — 별도 프레임 데이터베이스 없음). buddy
  분할(split)과 병합(coalesce, XOR 트릭 + free list 선형 탐색)을
  모두 구현했다.
- `mm::init()`이 `boot_info.memory_map`(usable 영역)으로
  `per_node_pool`을 채운다 — **버그를 하나 발견해 고쳤다**: usable
  영역이 커널 자신/initrd 영역(`k_region_kernel_image`/
  `k_region_initrd_image`)과 겹칠 수 있는데, memory.md는 이를 별도
  엔트리로 표시할 뿐 usable 엔트리에서 자동으로 빼주지 않는다.
  처음 구현에서는 이걸 놓쳐 실행 중인 커널 위에 페이지를 내줄 수
  있는 상태였다 — `add_free_region_excluding()`(구간 뺄셈)을 추가해
  고쳤다. self-test 값으로 직접 산술 검증(총 usable 바이트가
  기대값과 정확히 일치)까지 했다.
- `mm::phys_to_virt`/`virt_to_phys`([kernel/core/mm/phys_map.hpp](../../kernel/core/mm/phys_map.hpp))가
  virtual-memory-layout.md §5가 정의한 정식 위치에 자리 잡았다 —
  M2가 임시로 arch 쪽에 뒀던 버전을 대체했고, `boot_info_x86_64.cpp`도
  이걸 쓰도록 갱신했다. `<arch_mm_defs.hpp>`(제네릭 이름)를 통해
  arch 계층이 `k_physmap_base`/`k_physmap_size` 상수만 공개하는
  HAL 경계(ADR-002)를 실제 코드로 구현했다 —
  [kernel/arch/x86_64/arch_mm_defs.hpp](../../kernel/arch/x86_64/arch_mm_defs.hpp),
  `kernel/CMakeLists.txt`가 `MINICORE_ARCH`에 맞는 디렉토리를 include
  경로에 얹는다.
- **알려진 단순화** (모두 memory.md §7이 이미 M3 범위 밖으로 명시했거나,
  아직 없는 하위 시스템에 의존하는 부분):
  - `handle owner_process` 매개변수와 §5(쿼터) 실제 집행 — objects.md의
    handle 개념이 아직 없다(M4). `quota_state` 구조체만 존재.
  - §4 5단계의 회수(ADR-105)·블로킹(ADR-106) — `alloc_flags::blocking`을
    받되 현재는 `none`과 동일하게 즉시 `out_of_memory`를 반환한다.
  - 노드 간 폴백은 실제 ACPI SLIT/FDT 거리 행렬 대신 노드 인덱스
    순서를 쓴다 — 파싱이 아직 없다. 노드 1개(M1~M8, ADR-035)에서는
    관찰 가능한 차이가 없다.
  - `free_pages(addr, order)`는 어느 노드 소속인지 알 방법이 없어
    (page_frame이 "free 상태에서만 유효") 노드 0으로 고정한다 —
    다중 노드가 실제로 붙는 시점(M9 이후)에 물리주소→노드 조회
    수단이 필요해진다.

### 4. kernel/core/mm — 슬랩 힙 (memory.md §6)

[kernel/core/mm/slab.hpp](../../kernel/core/mm/slab.hpp)/[.cpp](../../kernel/core/mm/slab.cpp) —
고정 크기 클래스(16~4096바이트) marginal 슬랩. 각 슬랩은 order-0
페이지 하나 — 앞부분에 `slab_header`, 나머지를 청크로 잘라 침습적
free list로 엮는다. **알려진 단순화**: 빈 슬랩을 `alloc_pages`로
반납하지 않고(한 번 늘어난 슬랩 수는 줄지 않음), `slab_alloc`은
크기 클래스의 "머리" 슬랩만 보고 뒤쪽 슬랩의 빈 자리를 훑지 않는다
(정확성 문제 아님, 페이지 낭비 가능성만 있음).

### 5. panic_hook + arch_irq_save/restore (커널 쪽 정의)

- [kernel/core/panic.cpp](../../kernel/core/panic.cpp) — `klog::printf`로 로그
  남긴 뒤 `hlt`(x86_64)/`wfi`(aarch64) 정지 루프.
- [kernel/arch/x86_64/irq.cpp](../../kernel/arch/x86_64/irq.cpp) — `pushfq`/`cli`/`popfq`로
  RFLAGS.IF 저장·복원.
- `kernel/core/klog.cpp`가 이제 `irq_safe<spinlock>` 전역 락으로
  출력을 직렬화한다 — debug-console.md §3이 이미 요구했던 것을
  이번에 실제로 연결했다(M1~M2 시점엔 libk가 없어 미룸).

## 실행 중 발견해 기록한 결정(ADR)

- **ADR-115**(직전 세션에서 방향만 정함, 이번에 실제로 진행) —
  `<type_traits>`/`<utility>`/`<new>`(placement) 3개를 shim으로
  추가했다(`toolchain/freestanding-cxx/`). `<atomic>`은 shim 대신
  `atomic<T>`가 `__atomic_*` 컴파일러 내장을 직접 쓰는 쪽을
  택했다(ADR-115가 미리 예상한 경로). `<bit>`/`<concepts>`/`<limits>`는
  이번 M3 범위에서 필요하지 않아 아직 손대지 않았다.
- **ADR-116** — freestanding 빌드에도 `memset`/`memcpy`/`memmove`/`memcmp`를
  직접 제공해야 한다(구조체 zero-init에서 컴파일러가 이 심벌 호출을
  낼 수 있음). M2에서 이미 발견해 `kernel/core/freestanding_mem.cpp`로
  해결한 것을 M3에서 재확인(추가 변경 없음).
- **ADR-118** — **가장 중요한 발견**: `atomic<T>`의 생성자가
  constexpr이 아니면, 이를 포함하는 전역 변수가 "동적 초기화 필요"로
  판정되어 (crt0가 없어 `.init_array`를 아무도 실행하지 않으므로)
  초기화가 통째로 스킵되고 필드가 `.bss`의 0으로 남는다. 실제로
  슬랩 힙의 크기 클래스 배열에서 `chunk_size`가 0으로 남아 나눗셈
  예외(#DE)로 트리플 폴트가 났다 — QEMU로 재현·확인 후
  `atomic<T>`의 생성자를 `constexpr`로, `mutable` 대신 `const_cast`로
  바꿔 해결했다(`nm`으로 `_GLOBAL__sub_I_*` 심벌이 사라졌음을 확인).
  앞으로 libk에 추가하는 모든 타입은 이 요구사항(constexpr 생성자)을
  지켜야 한다.

## 검증 결과 (정직하게 보고)

- **확인함**: QEMU self-test 경로로 `mm::init` → `alloc_pages`(order
  0/2) → `free_pages` → `slab_alloc`/`slab_free` 전체 왕복이 성공하고,
  free_bytes 증감이 손으로 계산한 기대값(캐시 리필 포함)과 정확히
  일치함을 확인했다.
- **확인함**: `add_free_region_excluding`이 커널/initrd 겹침을
  정확히 잘라내는지 total_bytes를 손으로 계산해 대조했다(§3 참고).
- **확인함**: libk 39개 단위 테스트 전부 통과.
- **확인하지 못함**: buddy 병합(coalesce)이 실제로 형제 블록을
  합치는 경로 — 지금 만든 테스트는 병합이 필요할 만큼 많은
  alloc/free 조합을 만들지 않는다(주문 order0/order2 각각 한 번씩만
  할당·반납). 코드 리뷰 수준으로는 확인했지만 실행으로 병합 자체를
  직접 관찰하지는 않았다.
- **확인하지 못함**: 실제 다중 코어/다중 스레드 환경에서의 락 경합 —
  M1~M8은 단일 코어(ADR-035)라 애초에 경합이 없다. `ticket_lock`의
  FIFO 공정성, `mcs_lock`의 큐 동작은 단일 스레드 호스트 테스트로
  "왕복이 되는가"만 확인했을 뿐, 실제 경합 시나리오는 검증하지
  못했다(ADR-077이 이미 이 한계를 인지·수용).
- **확인하지 못함**: `arch_irq_save`/`arch_irq_restore`가 실제로
  인터럽트를 막는지 — 아직 IDT/인터럽트 컨트롤러가 없다(M9+ 범위).
  코드 자체(RFLAGS.IF 조작)는 정상적인 x86_64 관용구이지만 실행
  검증은 못 했다.

## 다음 마일스톤과의 접점

- M4(객체/핸들 테이블)는 `kernel/core/mm`의 슬랩 힙 위에
  `handle_entry` 등을 할당하게 된다. `mm::alloc_pages`의
  `owner_process`/쿼터 매개변수는 M4가 실제 `handle`/프로세스 개념을
  들여올 때 다시 채워 넣어야 한다(§1 "알려진 단순화" 참고).
- M5(스케줄러)가 등장하면 `mm::current_cpu_id()`(현재 하드코딩 0)를
  실제 코어 식별로 바꾸고, per-cpu 캐시의 리필 정책에
  `thread.preferred_node`를 연결할 수 있다.
- OPEN-48의 나머지(`<bit>`/`<concepts>`/`<limits>`)는 실제로 필요해지는
  마일스톤에서 다시 판단한다 — ADR-115의 정책(헤더별 shim 우선,
  어려우면 libk 대체)은 이미 확정되어 있다.
