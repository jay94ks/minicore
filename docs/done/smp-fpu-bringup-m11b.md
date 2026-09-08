# 완료 보고: smp-fpu-bringup M11b — FPU lazy 전환 + XSAVE/AVX 지원

**대상 계획**: [smp-fpu-bringup.md](../plan/smp-fpu-bringup.md) §M11b
**관련 결정**: ADR-133([kernel-scheduler.md](../design/kernel-scheduler.md)),
ADR-138(같은 문서, `fpu_save_area` 별도 페이지 할당),
ADR-139(같은 문서, `sched::yield()` enqueue 순서 버그 수정)
**실행일**: 2026-09-09~10

이 마일스톤으로 [smp-fpu-bringup.md](../plan/smp-fpu-bringup.md) 전체
(M9~M11b)가 완료됐다.

## 완료 기준 달성 확인

M11b의 목표(계획 §M11b): "M9의 thread_fpu_a/b 데모를 재사용해 값
보존은 그대로 통과함을 재확인하고, 추가로 (a) 같은 스레드가 연속으로
FPU를 쓸 때 `#NM`이 두 번째부터는 발생하지 않음(또는 발생해도 저장/
복원 없이 즉시 리턴함)을 로그로 확인, (b) `-cpu` 옵션으로 AVX가
있는/없는 두 QEMU 구성 모두에서 정상 동작함을 확인한다."

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh       # 42개 전부 PASS (M1~M11b 전체, AVX 없는 QEMU 기본 CPU)
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS (AVX 없음/있음 두 QEMU 구성)
```

QEMU 실측(발췌, AVX 없는 기본 구성):

```
[fpu] xsave_avail=0 avx_avail=0 using_xsave=0 area_size=512
...
[fpu] #NM apic_id=0 owner_changed=1   (thread A/B/C가 번갈아 FPU를 쓰는 동안 — 매번 소유자가 바뀜)
...
[fpu] thread A done all_preserved=1
[fpu] thread B done all_preserved=1
[fpu-lazy] thread C iteration 2 xmm0 preserved=1
[fpu-lazy] thread C iteration 3 xmm0 preserved=1   ← 이후 #NM 로그 자체가 더 안 나온다
[fpu-lazy] thread C iteration 4 xmm0 preserved=1
...
[fpu-lazy] thread C done all_preserved=1
```

thread A/B가 끝난 뒤로는 FPU를 쓰는 스레드가 thread C 하나뿐이라
`#NM`이 **전혀 발생하지 않는다** — ADR-139(§아래)가 고친
`sched::yield()`의 "자기 자신만 남으면 컨텍스트 스위치 자체를
생략" 최적화 덕분에, CR0.TS가 다시 세워지는 일도 없어 lazy 전환의
가장 이상적인 형태(값이 바뀌지 않으면 트랩조차 없음)를 그대로
보여준다.

## 수행한 작업

### 1. CPUID 질의 + XSAVE 활성화

[kernel/arch/x86_64/fpu.cpp](../../kernel/arch/x86_64/fpu.cpp)::`init_fpu()` —
CPUID.1:ECX의 XSAVE(bit26)·AVX(bit28)를 확인해 둘 다 있으면
CR4.OSXSAVE+XSETBV로 XCR0(x87|SSE|AVX)를 켜고, CPUID leaf 0xD로
필요한 저장 영역 크기를 얻는다(1024바이트 상한 assert). 없으면
ADR-127의 기존 FXSAVE 512바이트 경로로 폴백한다. BSP뿐 아니라 각
AP도 호출한다(smp.cpp::ap_main, CR0/CR4/XCR0은 코어별 레지스터라
멱등 호출이 필요하다 — M10이 `init_idt()`에 이미 쓴 패턴과 동일).

### 2. lazy 전환 — `arch_context_switch` 단순화 + `#NM` 핸들러

[kernel/arch/x86_64/context_switch.S](../../kernel/arch/x86_64/context_switch.S) —
매 스위치마다 FXSAVE/FXRSTOR를 무조건 하던 M9 코드를 지우고
`CR0.TS`만 무조건 세운다(FPU 레지스터 내용은 전혀 건드리지 않는다).
[kernel/arch/x86_64/fpu.cpp](../../kernel/arch/x86_64/fpu.cpp)::`arch_x86_64_handle_nm_trap()`
(idt.cpp가 벡터 7로 라우팅)이 실제 저장/복원을 담당한다 — 코어별
"현재 FPU 소유자"(`g_fpu_owner_by_apic_id[apic_id]`)를 확인해
같으면 즉시 리턴, 다르면 이전 소유자 저장 후 현재 스레드 복원.
`sched::exit()`가 `arch_fpu_thread_exiting()`(새 HAL 훅)을 불러
영구 종료하는 스레드를 소유자 표에서 지운다(ADR-133 §결정3).

### 3. 검증 데모 확장

thread_fpu_a/b(M9)는 그대로 재사용했다. 새로 추가한
`thread_fpu_c_entry`(8회 반복, a/b의 3회보다 길게)가 a/b가 먼저
끝난 뒤에도 계속 FPU를 써 "소유자가 안 바뀌면 어떻게 되는지"를
직접 보인다. `#NM` 핸들러 자신도 `[fpu] #NM apic_id=%u
owner_changed=%u`를 매번 로그로 남겨 두 경로(저장/복원 vs 즉시 리턴)
를 QEMU 로그에서 직접 구분할 수 있게 했다.

## 실행 중 발견한 버그 2가지

### 버그 1: `fpu_save_area`의 `alignas(64)`가 실제로 지켜지지 않음 (ADR-138)

`object::thread::fpu_save_area`를 512→1024바이트+`alignas(64)`로
키웠을 때, `object::thread` 자체는 여전히 `mm::slab_alloc()`으로
할당됐다. `-cpu max`(XSAVE+AVX)로 처음 부팅했을 때 최초의 `#NM`
핸들러 호출에서 곧바로 `#GP`(vector=13)가 났다 — 폴트 당시 XSAVE의
`area` 인자(RCX)가 64로 나눠 나머지 32(정확히 슬랩 헤더 크기만큼
어긋남)였다. ADR-134(M9)가 슬랩에 보장해 둔 정렬은 16바이트뿐이라
64바이트 요구인 XSAVE와 충돌한 것이다. `fpu_save_area`를
`object::thread` 안의 배열이 아니라 별도 `mm::alloc_pages(0, node)`
(항상 4096바이트 정렬) 결과를 가리키는 포인터로 바꿔 해결했다.

### 버그 2: `sched::yield()`가 "먼저 고르고 나중에 enqueue" 순서라 유일하게 남은 커널 밴드 스레드가 영원히 멈춤 (ADR-139)

thread_fpu_c(8회 반복)가 a/b보다 오래 사는 유일한 시나리오를 만들자,
thread A/B가 끝난 뒤 thread C가 단 하나만 남은 커널 밴드 스레드가
됐다. 이 상태에서 C가 `yield()`할 때마다, "고르기"가 "자기 자신을
다시 enqueue하기"보다 먼저 실행돼 그 순간엔 커널 밴드에 아무도 없는
것처럼 보였다 — 그래서 유저 밴드의 `initrun`(다시 스케줄될 일이
없는 스레드)이 대신 뽑혔고, 이후 스케줄러로 영원히 돌아오지 못해
thread C의 나머지 반복(3~7)이 전혀 실행되지 않았다. `yield()`의
enqueue/고르기 순서를 뒤집어(먼저 enqueue, 그다음 고르기) 해결했다 —
이 버그는 M11b 고유의 문제가 아니라 M5부터 있던 스케줄러 자체의
결함이었지만, M1~M11까지는 항상 "함께 도는" 커널 밴드 스레드가
2개 이상이라 드러날 기회가 없었다.

## 검증 결과 (정직하게 보고)

- **확인함**: AVX 없는 QEMU 기본 CPU와 `-cpu max`(XSAVE+AVX) 둘
  다에서 thread A/B/C 전부 값 보존 + 정상 종료.
- **확인함**: 소유자가 바뀌지 않으면 `#NM` 자체가 발생하지 않음
  (ADR-139의 yield 최적화 덕분에 컨텍스트 스위치조차 생략되어
  CR0.TS가 재설정되지 않는다) — 계획의 "저장/복원 없이 즉시
  리턴"보다 한 단계 더 나은 결과(트랩 자체가 없음)를 확인했다.
- **확인함**: `-cpu max`에서 `area_size=832`(XSAVE 필요 크기, 1024
  이내)로 정확히 계산됨.
- **확인하지 못함**: 실제 다중 코어 환경에서 서로 다른 코어가
  동시에 FPU를 쓰며 소유권을 다투는 상황 — M10/M11 done 보고가
  이미 명시했듯 AP는 협조적 스케줄러에 참여하지 않아 FPU를 쓰는
  스레드를 실행하지 않는다. `g_fpu_owner_by_apic_id`는 코어별로
  올바르게 분리돼 있지만, 이 분리 자체를 실제로 여러 코어에서
  동시에 실행하며 검증하지는 못했다.
- **확인하지 못함**: AVX-512 — ADR-133이 이미 범위 밖으로 뒀다.
- **확인하지 못함**: aarch64 FPSIMD 대응 — ADR-127/133이 이미 설계만
  남기고 구현은 별도 계획으로 미뤄 둔 부분.

## smp-fpu-bringup.md 전체 완료 정리

M9(eager FXSAVE) → M10(IDT+AP 기동+IPI shootdown) → M11(NUMA+락 순서
문서화) → M11b(lazy XSAVE/AVX)까지 전부 QEMU로 실측 검증했다. 다음
계획은 [system-servers-bringup.md](../plan/system-servers-bringup.md)
(M12: 부트 디바이스 마운트+procsrv ~ M20: libc 포팅+로그인 후 셸)다.
