# 완료 보고: general-purpose-completion M24 — 유저랜드 동적 메모리

**대상 계획**: [general-purpose-completion.md](../plan/general-purpose-completion.md) §M24
**관련 결정**: [kernel-memory.md](../design/kernel-memory.md) ADR-180
(sys_brk 고정 1MiB 힙 슬롯 + libmc 범프 할당자)
**실행일**: 2026-09-10

## 완료한 것

### D1. 커널 — `sys_brk`

새 syscall `sys_brk`(번호 13, 당시 `uapi.hpp::brk_request` — M50/
ADR-200 이후 `libs/mc/include/mc/syscall.h`)를 추가했다. `object::address_space`에 `heap_top`(정확한 brk
포인터)/`heap_mapped_top`(지금까지 실제로 페이지를 매핑해 둔
경계) 두 필드를 추가하고, [process_ops.cpp::brk()](../../kernel/arch/x86_64/process_ops.cpp)
가 첫 호출에서 지연 초기화(`heap_top = heap_mapped_top =
k_heap_user_vaddr`)한 뒤, `increment>0`이면 필요한 페이지만
`mm::alloc_pages`+`map_page`(write|user)로 새로 매핑한다.
`increment==0`은 순수 조회, `increment<0`(축소)은
`invalid_argument`로 거부한다(계획이 요구한 "늘리는 최소 기능"만
충족). 힙 가상주소는 [kernel-memory.md ADR-160](../design/kernel-memory.md)
의 슬롯 표를 따라 슬롯 5(`k_heap_user_vaddr = k_user_stack_top +
5*0x100000` — 슬롯 4는 이미 IPC `pages[]` 매핑이 차지)에 두고,
슬롯 하나(1MiB) 전체를 힙의 예산으로 쓴다.

### D2. libmc — 최소 malloc/free

[mc/heap.h](../../libs/mc/include/mc/heap.h)/
[heap.c](../../libs/mc/src/mem/heap.c)에 `mc_malloc`/
`mc_free`를 추가했다 — `sys_brk` 위의 순수 범프 할당자(4페이지
단위로 미리 확보해 syscall 왕복을 줄인다, `mc_free`는 회수하지
않는 no-op).

### D3. userland/shell — malloc 왕복 검증

`userland/shell/main.c`(이 minicore 네이티브 셸 자체는 M53/ADR-224가
`msh`로 교체하며 저장소에서 완전히 제거했다)의 `cat` 빌트인이
쓰던 스택 배열(`char content[MAX_CAT_LEN]`)을 `mc_malloc(MAX_CAT_LEN)`
으로 받은 버퍼로 바꿨다. 버퍼 확보 자체(`"[shell] malloc buffer
ok=1"`)와 그 버퍼로 `test.txt`를 읽은 내용이 여전히 정확함
(`"[shell] cat ok=1"`, M20이 이미 확인해 온 것과 동일한 기준)을
함께 확인해 유저랜드 동적 메모리 왕복을 증명한다.

QEMU 실측 첫 시도에 바로 통과했다(추가 디버깅 불필요) — M21/M22가
겪은 설계 재검토, M23이 사전에 memfs 구조를 확인해 둔 것처럼, 이번
라운드도 커널 API를 `sbrk()` 고전 관례 그대로 채택해 모호함 자체가
적었다.

### D4. 검증

`tools/smoke-test-x86_64.sh`에 `"[shell] malloc buffer ok=1"`을
추가했다. 4개 QEMU 스위트 전부 재확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0개) |

## 남겨 둔 것 (OPEN)

새 OPEN 항목은 열지 않는다 — 알려진 단순화(힙 축소 미지원, free가
회수 안 함, 힙이 스레드별이 아니라 프로세스별)는 모두 ADR-180
본문에 명시했고, 계획이 스스로 정한 "최소 기능" 범위를 그대로
충족한 것이라 별도 추적이 필요한 미결 사항으로 보지 않는다.

## 다음

[general-purpose-completion.md](../plan/general-purpose-completion.md)
§M25(최소 네트워킹)로 이어간다.
