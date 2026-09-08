# 완료 보고: smp-fpu-bringup M10 — IDT 기초 + AP 기동 + IPI 기반 TLB shootdown

**대상 계획**: [smp-fpu-bringup.md](../plan/smp-fpu-bringup.md) §M10
**관련 결정**: ADR-055([kernel-scheduler.md](../design/kernel-scheduler.md)),
ADR-135(같은 문서, 이번에 실제 구현·QEMU 검증 과정에서 발견한 세
가지 세부 사항)
**실행일**: 2026-09-09

## 완료 기준 달성 확인

M10의 목표(계획 §M10): "`tools/run-qemu.sh`에 `MINICORE_QEMU_SMP=N`을
추가하고, `-smp N`으로 띄운 QEMU에서 각 AP가 자기 APIC ID를 포함한
로그를 남기고, BSP가 한 페이지를 매핑 해제한 뒤 IPI shootdown을 보내면
그 페이지를 매핑해 뒀던 AP 코어가 무효화 완료를 로그로 확인한다."

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
MINICORE_QEMU_SMP=4 tools/smoke-test-smp-x86_64.sh
# => 11개 항목 전부 PASS
tools/smoke-test-x86_64.sh
# => 기존 M1~M9 39개 항목 전부 PASS (SMP 미설정 시 M1~M9와 관찰 가능한
#    차이 없음을 재확인 — memory_map_count가 4→5로 바뀐 것만 예외,
#    아래 "M1~M9 경로에 생긴 관찰 가능한 변화" 참고)
```

실제 QEMU 출력(`MINICORE_QEMU_SMP=4`, 발췌):

```
[acpi] madt_ok=1 cpu_count=4 lapic_base=0xfee00000
[acpi] cpu[0] apic_id=0
[acpi] cpu[1] apic_id=1
[acpi] cpu[2] apic_id=2
[acpi] cpu[3] apic_id=3
[smp] BSP apic_id=0
[smp] AP apic_id=1 online cpu_index=1
[smp] AP apic_id=2 online cpu_index=2
[smp] AP apic_id=3 online cpu_index=3
[smp] bring_up_aps done online_count=4
[smp] online_cpu_count=4
[object] create_owner ok=1 handle=1
...
[pgtbl] create_address_space_root ok=1
```

이후 `demo_page_table()`/ELF 로더/유저 스택 매핑 등 기존 M1~M9 데모가
호출하는 `map_page`/`unmap_page`/`protect_page`마다 IPI shootdown이
실제로 3개 AP에 브로드캐스트되고 각 AP의 ack를 기다린 뒤 리턴하며,
그 이후 M1~M9 검증 문자열(스레드 A/B, IPC, FPU 등) 전체가 회귀 없이
그대로 통과함을 확인했다 — 별도 데모를 새로 만들지 않고 기존 경로가
자연스럽게 shootdown을 왕복시키는 것으로 계획의 검증 목표를 충족한다.

## 수행한 작업

### 1. IDT 최소 기반 — catch-all 예외 진단 + IPI/스퓨리어스 벡터

[kernel/arch/x86_64/idt.hpp](../../kernel/arch/x86_64/idt.hpp)/[.cpp](../../kernel/arch/x86_64/idt.cpp),
[isr_stubs.S](../../kernel/arch/x86_64/isr_stubs.S) — GAS `.altmacro`로
256개 ISR 스텁 + `isr_stub_table`을 생성한다(하드웨어가 error_code를
미는 8개 벡터는 가짜 0을 밀지 않고, 나머지는 민다). `interrupt_dispatch()`
(idt.cpp)가 벡터로 분기한다: IPI 벡터(0xFC)는 `smp_handle_tlb_shootdown_ipi()`,
스퓨리어스(0xFF)는 로그만, 나머지 전부(0~31 예외 + `#NM` 자리 포함)는
`diagnose_and_halt()` — 레지스터 전체·인터럽트 프레임을 klog로 찍고
ADR-126과 같은 프레임포인터 백트레이스(시작점은 `interrupt_frame::rbp`)
후 정지한다.

**검증(임시 self-test, ADR-126과 같은 방식)**: `kernel_main()`에 고의로
`1 / 0`(정수 0으로 나누기, `#DE`)을 유발하는 코드를 잠깐 추가해 QEMU로
확인했다:

```
[idt] unexpected exception vector=0 error_code=0x0 rip=0xffffffff801118f3 cs=0x18 ...
[idt] rax=0x1 rbx=0xf59e0 rcx=0x0 ...
[idt] backtrace:
```

(백트레이스가 빈 것은 버그가 아니다 — `kernel_main`은 `_start64`가
`call`이 아니라 `jmp`로 진입시켜 진짜 호출자가 없다, 그 위 프레임을
더 걸을 return address가 없는 게 정상.) 확인 후 이 트리거 코드는
제거했다.

### 2. ACPI RSDP 검색 + MADT 파싱

[kernel/arch/x86_64/acpi.hpp](../../kernel/arch/x86_64/acpi.hpp)/[.cpp](../../kernel/arch/x86_64/acpi.cpp) —
`boot_info.arch_data_addr`가 있으면 그대로 쓰고, 없으면(이 개발
머신은 QEMU PVH 직접 부팅이라 항상 없다, ADR-114) ACPI 스펙 자체가
정의하는 표준 폴백(EBDA 첫 1KiB + BIOS ROM `[0xE0000,0x100000)`을
16바이트 경계로 `"RSD PTR "` 시그니처 스캔)으로 RSDP를 직접 찾는다.
찾은 RSDP에서 RSDT(32비트) 또는 XSDT(64비트, revision≥2 우선)를 걸어
signature `"APIC"`(MADT)을 찾고, Processor Local APIC 엔트리(Enabled
비트가 켜진 것만) + Local APIC Address Override 엔트리를 파싱한다.
`docs/spec/boot.md` §7에 이 확장을 반영했다.

**M2/M9까지의 self-test fixture 패턴과 다른 점**: 이번엔 가짜 fixture를
만들지 않고 **QEMU가 실제로 구성해 두는 진짜 ACPI 테이블**을 찾아냈다
— `-smp 1`/`-smp 2`/`-smp 4`로 각각 실행해 `cpu_count`가 정확히
1/2/4로, `apic_id`가 정확히 `0`/`0,1`/`0,1,2,3`으로 나오는 것까지
QEMU로 직접 확인했다(ADR-114/117이 이미 기록한 "이 개발 머신은 GRUB가
없다" 한계와는 별개로, ACPI 테이블 자체는 부트로더 경로와 무관하게
QEMU의 machine 모델이 항상 만들어 둔다는 사실을 이번에 처음 확인했다).

### 3. LAPIC xAPIC 드라이버

[kernel/arch/x86_64/lapic.hpp](../../kernel/arch/x86_64/lapic.hpp)/[.cpp](../../kernel/arch/x86_64/lapic.cpp) —
MMIO 접근은 physmap(이미 512GiB 항등 매핑, M1)을 그대로 재사용해 별도
매핑 없이 가능했다. `lapic_init()`(BSP 1회, base 저장+활성화)과
`lapic_enable_this_core()`(코어마다 반복 — "로컬" APIC이라는 이름
그대로 레지스터 내용은 코어별)을 분리했다. `lapic_send_init_sipi_sipi()`/
`lapic_send_fixed_ipi()`가 ICR(0x300/0x310)을 직접 조작한다.

### 4. AP 트램폴린 (16비트 → 32비트 → 64비트 → higher-half)

[kernel/arch/x86_64/ap_trampoline.S](../../kernel/arch/x86_64/ap_trampoline.S) —
물리주소 `0x8000`(SIPI 8비트 벡터 필드 제약으로 1MiB 미만·4KiB 정렬
필요, 커널 이미지는 전부 1MiB 이상에 링크됨)에 복사해 실행하는 걸
전제로, 내부 절대주소는 전부 `AP_TRAMPOLINE_BASE + (라벨 - 시작라벨)`
링크타임 상수로 계산해 "실제로 어디에 링크되었는지"와 무관하게
동작한다. 로컬(임시) GDT를 자체 내장해 16비트에서 32비트로 전환하고,
BSP가 이미 완성해 둔 `pml4`를 그대로 재사용해 `CR3`/`PAE`/`LME`/`PG`를
켜 64비트로 전환한 뒤, `movabs`로 higher-half의 `ap_entry64_highhalf`
(평범한 커널 코드, `%rip` 상대 참조가 안전한 지점)로 완전히 벗어난다.
그 지점에서 진짜 커널 GDT(`gdt64_pointer`)로 다시 `lgdt`하고, BSP가
mailbox(`g_ap_boot_stack_top`/`g_ap_boot_cpu_index`)에 채워 둔 값으로
스택을 세팅한 뒤 `ap_main()`(C++)을 부른다.

### 5. AP 순차 기동 + IPI TLB shootdown

[kernel/arch/x86_64/smp.hpp](../../kernel/arch/x86_64/smp.hpp)/[.cpp](../../kernel/arch/x86_64/smp.cpp) —
`bring_up_aps()`가 MADT가 찾은 APIC ID를 하나씩(동시가 아니라 "온라인
확인까지 하나씩 순차", ADR-055의 "즉시 전부"는 부팅 시퀀스 안에서
자동으로 전부 기동한다는 뜻이지 병렬 스톰을 요구하지 않는다) 기동하며
`g_online_count`(전역 원자적 카운터) 증가로 온라인 여부를 확인한다.
`broadcast_tlb_shootdown()`은 [page_table.cpp](../../kernel/arch/x86_64/page_table.cpp)의
`map_page`/`unmap_page`/`protect_page` 성공 경로에서 무조건 호출되며,
온라인 AP가 없으면(기본, SMP 미설정) 즉시 반환한다 — 있으면
`g_shootdown_target_vaddr`를 채우고 `g_shootdown_pending`(원자적
카운터)을 세운 뒤 각 AP에 고정 벡터 IPI를 보내고, 그 카운터가 0이
될 때까지 스핀 대기한다. AP 쪽 ISR(`smp_handle_tlb_shootdown_ipi`)은
`invlpg`+카운터 감소+EOI만 한다.

## 실행 중 발견한 버그 3가지 (ADR-135로 확정)

QEMU `-d cpu_reset,guest_errors` 트레이스(ADR-125 인프라, M9 완료
보고가 예고한 대로 실전에 다시 썼다)로 각각 진단했다.

1. **STARTUP(SIPI)에 INIT과 같은 level/assert 비트를 실었다가 ICR
   Delivery Status가 영원히 안 떨어짐** — Intel SDM Vol.3 Table 11-8이
   이미 "STARTUP엔 적용 안 됨"이라고 명시한 것을 어겼다. 비트를 빼자
   즉시 정상화됐다.
2. **전통적인 "SIPI 두 번" 관례를 그대로 따랐다가 같은 증상 재현** —
   첫 SIPI로 이미 기동해 `hlt`에 들어간 AP에게 두 번째를 보내는 경쟁
   조건이었다. 하나만 보내는 것으로 바꿨다(온라인 확인은
   `g_online_count`가 이미 담당).
3. **AP 트램폴린의 로컬 GDT/GDTR 데이터를 `ap_trampoline_end` "뒤"에
   둬서, BSP의 `memcpy([start,end))`가 그 바이트를 복사하지 않음** —
   AP가 `lgdt`로 읽은 GDTR이 전부 0인 채(base=0, limit=0)로 남아 그
   직후의 `ljmp`가 즉시 `#GP`→`#DF`→트리플폴트로 죽었다. 데이터를
   `ap_trampoline_end` 앞으로 옮겨 해결했다.
4. **(3가지라고 했지만 실제로 넷째도 발견) 각 AP가 자기 IDTR을
   재적재하지 않아, 첫 TLB shootdown IPI가 도착하는 순간 트리플폴트**
   — IDTR은 코어별 레지스터라 BSP가 `init_idt()`로 채운 `g_idt`(전역
   메모리)를 실제로 "적재"한 코어는 BSP뿐이었다. `ap_main()`에서
   `init_idt()`를 다시 호출해(멱등이라 안전) 해결했다.

네 가지 모두 ADR-135에 근거·영향과 함께 기록했다.

## M1~M9 경로에 생긴 관찰 가능한 변화

- **`[boot_info:selftest] memory_map_count`가 4→5로 바뀜** — AP
  트램폴린 스크래치 페이지(물리 `0x8000`, 1페이지)를
  `k_region_kernel_image`로 마킹해 물리 할당자에서 영구 배제하는
  엔트리가 추가됐다(§실행 중 발견한 버그와는 별개로, 처음부터 의도한
  설계 — mm이 이 페이지를 다른 용도로 내주면 트램폴린이 덮어써질 수
  있다). `tools/smoke-test-x86_64.sh`의 기대값을 갱신했다.
- 그 외 M1~M9의 모든 검증 문자열은 SMP 미설정(기본) 시 완전히
  동일하다 — `[smp] BSP apic_id=0`/`[smp] online_cpu_count=1`이 새로
  추가됐을 뿐, 기존 로직·순서에는 변화가 없다.

## 검증 결과 (정직하게 보고)

- **확인함**: `-smp 1`(기본)/`-smp 2`/`-smp 4` 모두에서 MADT가 정확한
  코어 수·APIC ID를 실제 QEMU ACPI 테이블에서 얻음, `-smp 4`에서 3개
  AP 전부 온라인 확인, 이후 M1~M9 데모 경로 전체가 실제로 IPI
  shootdown을 여러 차례 왕복시키면서도 회귀 없이 끝까지 통과.
- **확인함**: `MINICORE_QEMU_SMP` 미설정(기본) 시 M1~M9와 완전히 같은
  동작(1개 예외: 위 memory_map_count 변화, 의도된 것).
- **확인하지 못함**: 실제 다중 소켓/NUMA 환경 — 이 개발 머신은 QEMU
  단일 노드 구성만 검증했다(M11의 몫).
- **확인하지 못함**: AP가 워크 스틸링 등으로 실제 스케줄러에 편입되는
  경로 — M10의 AP는 온라인 신호를 보낸 뒤 `arch_idle_halt()`로 영원히
  멈춰(IPI만 받아 처리) 있다. `sched`/`mm::alloc_pages`의 per-CPU 캐시
  (`current_cpu_id()`가 아직 항상 0을 반환)를 AP가 실제로 쓰는 경로는
  이 마일스톤에서 만들지 않았다 — M11이 "다중 코어에서 기존
  스케줄러/IPC 데모가 정확히 동작"함을 검증할 때 처음 그 경로를
  타게 된다.
- **확인하지 못함**: 타이머 인터럽트 기반 선점 — 계획이 명시적으로
  범위 밖으로 뒀다. 스케줄링은 여전히 협조적(yield)이다.
- **확인하지 못함**: aarch64 대응 — ADR-055 자체가 이미 별도 이식
  계획으로 미뤄 둔 부분, 이번에도 손대지 않았다.

## 다음 마일스톤과의 접점

- M11(다중 코어/NUMA 실환경 검증 + 락 순서 문서화)이 이 M10의
  `MINICORE_QEMU_SMP`로 여러 코어를 띄운 채 M5/M6/M7 데모를 재검증하고,
  이번에 처음 실제로 경합하는 락들(엔드포인트, `handle_table`, `klog`
  전역 락 등)의 획득 순서를 `kernel-memory.md`(ADR-052)에 표로
  정리한다.
- M11b(lazy FPU 전환, ADR-133)가 M10이 마련해 둔 `#NM`(벡터 7) 자리를
  실제로 채운다 — 이 M10의 catch-all이 지금 그 벡터를 잡고 있으므로,
  M11b는 `interrupt_dispatch()`에 `#NM` 전용 분기를 추가하는 형태로
  구현될 것이다. ADR-135 §결정3(각 AP의 `lidt`)이 이미 그 전제
  (모든 코어가 유효한 IDTR을 가짐)를 충족해 뒀다.
