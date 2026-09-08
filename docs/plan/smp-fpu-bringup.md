# 실행 계획: FPU 컨텍스트 스위칭 + SMP 기동(AP·IPI·TLB shootdown) + 다중 코어/NUMA 검증

**관련 결정**: ADR-127·**133**([kernel-scheduler.md](../design/kernel-scheduler.md)),
ADR-055·ADR-034~036·ADR-053·ADR-054(같은 문서/[kernel-memory.md](../design/kernel-memory.md)),
ADR-052(kernel-memory.md, 락 순서), ADR-125·ADR-126([build-system.md](../design/build-system.md)/[boot-and-drivers.md](../design/boot-and-drivers.md), 디버깅 인프라 — 본 계획의 부팅 실패 진단에 그대로 활용)

**선행 완료**: [kernel-bootstrap.md](kernel-bootstrap.md) M1~M8 — 이 계획은
그 뒤를 잇는다(CLAUDE.md·kernel-bootstrap.md가 예고한 "M9 이후").

## 배경과 범위

`kernel-bootstrap.md`가 완료 후 예고했던 두 축(AP 기동/IPI/TLB
shootdown, 락 순서 규칙 문서화)에, 설계 과정에서 새로 확인된 세 번째
축(FPU/SIMD 컨텍스트 스위칭 부재)을 더해 하나의 계획으로 묶는다.

FPU 문제는 SMP와 무관하게 **지금(단일 코어)도 이미 존재하는 버그**다
— `arch_context_switch`(M5, `kernel/arch/x86_64/context_switch.S`)는
콜리세이브 정수 레지스터만 저장하고 FPU/XMM 상태는 전혀 건드리지
않는다. 어떤 두 스레드든 하나가 부동소수점 연산 중 블로킹 syscall로
전환되고 다른 하나가 그사이 부동소수점을 쓰면 값이 섞인다. 지금까지
드러나지 않은 건 M1~M8 데모 스레드 중 부동소수점을 쓴 것이 하나도
없었기 때문일 뿐이다(ADR-127 근거 참고). SMP보다 먼저, 그리고 SMP와
독립적으로 고칠 수 있어 M9로 가장 먼저 둔다.

M10(AP 기동+IPI+TLB shootdown)과 M11(다중 코어/NUMA 실환경 검증+락
순서 문서화)은 원래 `kernel-bootstrap.md`가 "M9 이후"로 예고했던
내용 그대로다.

**M11b**는 이후 추가됐다 — M9는 IDT가 없는 시점이라 FPU 전략을
eager FXSAVE(ADR-127)로 시작할 수밖에 없었는데, M10이 IDT를 실제로
만들고 M11이 다중 코어를 검증한 뒤에는 그 전제가 사라진다. 사용자가
lazy 전환(CR0.TS/#NM 트랩)과 AVX 지원을 이 계획 안에서 마저
끝내달라고 요청했고(ADR-133), 두 항목 모두 IDT+다중 코어 인프라가
전제라 M9가 아니라 M11 바로 다음이 순서상 맞는 자리다 — 그래서
"M12"가 아니라 "M11b"로 붙인다: 이 계획 다음 순서인
[system-servers-bringup.md](system-servers-bringup.md)가 이미
M12부터 번호를 쓰고 있어, 새 마일스톤을 그 사이에 끼워 넣으면서
전체를 다시 매기지 않기 위한 표기다.

## M9. FPU/SIMD 컨텍스트 스위칭 (ADR-127)

- **부팅 시퀀스**: `kernel_main` 극초기(페이지테이블 완성 이후,
  IDT보다는 먼저 가능 — FXSAVE/FXRSTOR 자체는 인터럽트 인프라에
  의존하지 않는다)에 CR0.EM 클리어·CR0.MP 설정·CR4.OSFXSR/OSXMMEXCPT
  설정하는 `arch_x86_64::init_fpu()`(가칭) 추가.
- **`object::thread`**: `alignas(16) uint8_t fxsave_area[512];` 필드
  추가(ADR-127 §2). `create_kernel_thread`/`create_user_thread`(모두
  `kernel/core/sched/scheduler.cpp`)가 스레드 생성 시 이 버퍼를 0으로
  채운 뒤 FCW=0x037F, MXCSR=0x1F80을 고정 오프셋에 patch.
- **`context_switch.S`**: 콜리세이브 정수 레지스터 저장/복원 사이에
  `FXSAVE`(이전 스레드 버퍼)/`FXRSTOR`(다음 스레드 버퍼)를 무조건
  추가. 두 스레드의 `fxsave_area` 물리주소를 `arch_context_switch`에
  새 인자로 넘기거나, `object::thread*` 자체를 넘겨 오프셋으로 찾는
  방식 중 구현 시점에 더 단순한 쪽을 택한다.
- **목표(QEMU 검증)**: 데모 스레드 두 개(가칭 thread_fpu_a/b)를 추가해
  각각 서로 다른 상수를 `xmm0`에 로드한 뒤 여러 차례 `sched::yield()`로
  번갈아 실행하고, 매번 자기 값이 유지됐는지 확인해 로그로 남긴다
  (`[fpu] thread A xmm0 preserved=1` 형식). 스위칭 전(고의로 FXSAVE/
  FXRSTOR를 비활성화한 상태)에는 이 검증이 실패함을 먼저 QEMU로
  확인해 버그가 실재했음을 증명한 뒤, 구현을 켜서 통과로 바뀌는 것까지
  확인한다(회귀 방지 근거 확보).

## M10. IDT 기초 + AP 기동 + IPI 기반 TLB shootdown (ADR-055)

- **IDT/예외 처리 최소 기반**: x86_64 IDT를 새로 만든다(지금까지
  전무 — M8까지는 SYSCALL/SYSRET만으로 유저 진입을 다뤘다). 이
  계획에서 IDT에 거는 벡터는 세 종류로 제한한다 — (a) IPI용
  벡터(TLB shootdown 전용), (b) 부팅 중 원인 불명 정지를 진단하기
  위한 catch-all 예외 핸들러(레지스터를 klog로 찍고 ADR-126의
  백트레이스를 재사용해 정지 — 일반 예외 처리 정책 자체는 이 계획의
  범위 밖, M9 이후의 또 다른 계획에서 다룬다), (c) **`#NM`(벡터 7,
  ADR-133)** — M11b의 lazy FPU 전환이 이 벡터를 쓴다, 여기서
  자리만 마련해 둔다. **타이머 인터럽트 기반 선점 스케줄링은 이
  계획에 포함하지 않는다** — 스케줄링은 여전히 협조적(yield 기반)이다.
- **ACPI MADT 파싱**: `boot_info.arch_data_addr`(ACPI RSDP)에서 MADT를
  직접 파싱(ADR-006 — 서드파티 ACPI 라이브러리 금지)해 논리 CPU 개수와
  각 APIC ID 목록을 얻는다. `boot_info.cpu_count`/`cpu_node_map_addr`
  (`boot.md` §3)를 이 값으로 채운다 — M1~M8은 항상 `cpu_count=1`이었던
  자리다.
- **LAPIC 초기화 + AP 트램폴린**: BSP의 LAPIC을 초기화하고, 저지대
  (ADR-121이 이미 모든 새 주소공간에 공유시키는 `[0, 8MiB)` 항등매핑
  범위 안, 실모드 진입 가능한 위치)에 AP가 실행할 트램폴린 코드를
  둔다 — 실모드 → 보호모드 → 롱모드로 올라와 공유 페이지테이블에
  합류한 뒤 `kernel_main`과 별도인 AP 진입점으로 점프.
- **INIT-SIPI-SIPI**: BSP가 MADT로 찾은 각 AP에 INIT-SIPI-SIPI
  시퀀스를 보내 부팅 직후 **즉시 전부** 기동한다(ADR-055 — 온디맨드
  기동 아님).
- **IPI 기반 TLB shootdown**: 페이지테이블 변경 API(`arch_x86_64::
  map_page`/`unmap_page`/`protect_page`, M4)가 매핑을 바꿀 때마다,
  그 매핑을 볼 수 있는 다른 코어들에 예약된 IPI 벡터를 즉시
  브로드캐스트한다(지연/배치 없음, ADR-055). 수신 코어는 핸들러에서
  `INVLPG`(단일 페이지) 또는 필요시 전체 TLB flush 후 EOI.
- **목표(QEMU 검증)**: `tools/run-qemu.sh`에 SMP 코어 수를 지정하는
  opt-in 환경변수(가칭 `MINICORE_QEMU_SMP=N`, ADR-125가 세운
  "기본값 유지 + opt-in" 패턴을 그대로 따른다 — 기본은 여전히 1코어)를
  추가한다. `-smp N`으로 띄운 QEMU에서 각 AP가 자기 APIC ID를 포함한
  로그(`[smp] AP apic_id=N online`)를 남기고, BSP가 한 페이지를
  매핑 해제한 뒤 IPI shootdown을 보내면 그 페이지를 매핑해 뒀던
  AP 코어가 무효화 완료를 로그로 확인한다.

## M11. 다중 코어/다중 NUMA 실환경 검증 + 락 순서 규칙 문서화 (ADR-052~054)

- **다중 코어 검증**: M10의 `MINICORE_QEMU_SMP`로 여러 코어를 띄운 채
  M5(스케줄러)·M6/M7(IPC)의 기존 데모가 여전히 정확히 동작하는지
  확인 — 지금까지는 전부 논리적으로 1코어에서만 실행됐다.
- **다중 NUMA 노드 검증**: QEMU `-numa node,...` 구성으로 노드 2개
  이상을 만들고, M3(노드별 물리 메모리 풀)·M5(노드별 `run_queue`)가
  실제로 노드별로 분리 동작하는지, ADR-053(워크 스틸링)·ADR-054(할당
  자동 폴백)이 설계대로 동작하는지 QEMU 로그로 확인한다. x86_64는
  ACPI SRAT/SLIT을 이 시점에 처음 실제로 파싱한다(`kernel-memory.md`
  ADR-036이 이미 "SMP·전원관리가 필요해지는 시점"으로 미뤄뒀던 작업).
- **락 순서 규칙 문서화(ADR-052)**: 지금까지 락이 실제로 동시에
  여러 코어에서 경합한 적이 없어(M1~M8은 논리적으로 항상 1코어)
  순서 규칙을 적을 실익이 없었다 — M10~M11에서 처음으로 실제 경합이
  발생하는 락들(엔드포인트 락, 노드별 mm/`run_queue` 락, `handle_table`
  락, `klog` 전역 락 ADR-037)을 대상으로 실제 코드에서 관찰되는 획득
  순서를 `kernel-memory.md`(ADR-052 문서)에 표로 정리한다 — 런타임
  검증기는 여전히 두지 않는다(ADR-052가 이미 확정한 v1 방침).
- **완료 기준**: 다중 코어·다중 NUMA QEMU 구성에서 기존 M1~M8 스모크
  테스트 전체 + M9(FPU)/M10(SMP/IPI) 검증이 함께 통과하고,
  `kernel-memory.md`에 락 순서 표가 반영된다.

## M11b. FPU lazy 전환 + XSAVE/AVX 지원 (ADR-133)

M9에서는 아직 IDT가 없어 eager FXSAVE로만 구현했던 것을, M10이 IDT를
만들고 M11이 다중 코어 환경을 검증한 바로 다음 지점에서 lazy로
개정한다 — `#NM` 트랩 기반 lazy 전환은 "레지스터의 실제 소유자가
바뀔 때만 저장/복원"하는 코어별 상태(`g_fpu_owner`)를 다루므로, 이미
다중 코어가 검증된 뒤에 그 위에서 만드는 것이 순서상 자연스럽다
(M9 시점엔 아직 다중 코어 자체가 없어 이 상태의 "코어별"이라는
말 자체가 무의미했다).

- **CPUID 질의 + XSAVE 활성화**: 부팅 시 `CPUID.1:ECX`의 XSAVE/AVX
  비트를 확인해 있으면 `CR4.OSXSAVE`+`XSETBV`(XCR0에 x87/SSE/AVX
  활성화)로 전환, 없으면 M9의 기존 FXSAVE 경로로 폴백.
- **`#NM` 핸들러**(M10이 마련해 둔 벡터 7 자리에 실제 구현): owner
  비교 → 다르면 이전 소유자 저장+현재 스레드 복원, 같으면 아무 것도
  안 하고 리턴 → `CR0.TS` 클리어.
- **`arch_context_switch`**: FXSAVE/FXRSTOR 무조건 호출을 제거하고
  `CR0.TS` 설정 한 줄로 대체.
- **`sched::exit()`**: 종료하는 스레드가 어느 코어의 `g_fpu_owner`면
  그 항목을 지운다.
- **목표(QEMU 검증)**: M9의 `thread_fpu_a/b` 데모를 재사용해 값
  보존은 그대로 통과함을 재확인하고, 추가로 (a) 같은 스레드가
  연속으로 FPU를 쓸 때 `#NM`이 두 번째부터는 발생하지 않음(또는
  발생해도 저장/복원 없이 즉시 리턴함)을 로그로 확인, (b) `-cpu`
  옵션으로 AVX가 있는/없는 두 QEMU 구성 모두에서 정상 동작함을
  확인한다.

## 범위 밖 (하지 않음)

- **aarch64 AP 기동/FPSIMD 컨텍스트 스위칭 실제 구현** — ADR-127·
  ADR-055 모두 aarch64 설계(PSCI `CPU_ON`, STP/LDP 기반 FPSIMD
  저장)를 이미 문서에 남겼지만, aarch64는 M1~M8에 해당하는 부트스텁
  자체가 아직 없다(`tools/run-qemu.sh`, `kernel-bootstrap.md` 범위
  밖 선언과 동일한 이유) — 별도 aarch64 이식 계획으로 미룬다.
- **AVX-512 지원** — ADR-133이 다루는 건 AVX(YMM)까지다. AVX-512는
  이 계획 범위 밖 — 필요해지면 별도 ADR.
- **타이머 인터럽트 기반 선점 스케줄링** — M10이 IDT를 새로 만들지만
  용도는 IPI/진단 예외뿐이다. 스케줄링은 여전히 협조적(yield)이다.
  선점 도입은 이후 별도 계획.
- **일반 예외(#PF, #GP 등) 처리 정책** — M10의 catch-all 핸들러는
  "정지 후 진단"만 한다. 페이지 폴트를 이용한 진짜 지연 매핑/COW
  같은 정책적 예외 처리는 이 계획의 범위 밖.
- **자동화 CI 파이프라인** — kernel-bootstrap.md와 동일하게 이 계획도
  다루지 않는다.

## 검증 방법

각 마일스톤은 QEMU 부팅 로그로 확인 가능한 관찰 가능한 산출물을
갖는다(`kernel-bootstrap.md`와 같은 방식). `tools/smoke-test-x86_64.sh`에
M9/M10/M11/M11b 확인 문자열을 마일스톤 완료 시점마다 추가한다.
SMP/NUMA 검증(M10/M11)은 기본 스모크 테스트 경로와 분리한다 —
`MINICORE_QEMU_SMP`/QEMU `-numa`가 opt-in이므로, 이를 쓰는 검증은
별도 스크립트(가칭 `tools/smoke-test-smp-x86_64.sh`)로 만들어 기존
1코어 기본 경로(`smoke-test-x86_64.sh`)를 그대로 보존한다. M11b의
AVX 유/무 두 QEMU 구성 검증은 `MINICORE_QEMU_CPU`류 opt-in
환경변수로 `-cpu` 값을 바꿔가며 같은 스모크 스크립트를 두 번
돌리는 방식으로 처리한다(세부는 M11b 착수 시점에 정함).

## 완료 후

각 마일스톤(또는 M9~M11b 전체) 완료 시 `docs/done/`에 결과를 기록한다.
이 계획 문서 자체는 실행 후에도 수정하지 않고 "실행 전 계획" 그대로
보존한다(`kernel-bootstrap.md`와 동일한 관례).
