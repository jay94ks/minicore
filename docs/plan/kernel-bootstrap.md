# 실행 계획: 커널 부팅 + IPC 착수

**대상 스펙**: [boot.md](../spec/boot.md), [ipc.md](../spec/ipc.md), [debug-console.md](../spec/debug-console.md), [objects.md](../spec/objects.md), [memory.md](../spec/memory.md), [scheduler.md](../spec/scheduler.md), [cxx-conventions.md](../spec/cxx-conventions.md)
**관련 결정**: ADR-004, 007, 009~017, 023~025, 026~037, 042

## 목적

x86_64 우선(ADR-009) 커널이 "부팅 → 최소 초기화 → initrun 진입 → IPC
왕복 1회 성공"까지 도달하는 **최초 수직 슬라이스**를 만든다. 실제 커널
소스 작성에 착수하기 위한 마일스톤 단위 계획이다.

**SMP/NUMA에 대하여 (ADR-035)**: M1~M8은 BSP(부트스트랩 코어) 단일
코어 + 단일 NUMA 노드로 **실행**된다 — 보조 코어(AP) 기동은 하지 않는다.
그러나 메모리 할당자(M3)와 스케줄러(M5)의 **자료구조와 API는 처음부터
다중 코어·다중 NUMA 노드를 전제**로 설계한다(ADR-033/034/036). QEMU
기본 구성(단일 소켓, 노드 1개)에서는 이 구조가 자연히 "코어 1개·노드
1개"로 단순화되므로, M1~M8에서 관찰되는 동작 자체는 단일 코어 커널과
다르지 않다 — 다만 나중에 AP를 추가할 때 스케줄러·할당자를 다시 쓰지
않아도 되는 것이 목적이다. 실제 AP 기동·코어 간 IPI·TLB shootdown은
§M9에서 별도로 다룬다.

## 전제 조건 (착수 전 반드시 해결)

- **크로스 툴체인 준비.** 이 개발 머신에는 Clang과 크로스 GCC가 모두
  없음이 이미 확인되었다([scaffold-repo-skeleton.md](../done/scaffold-repo-skeleton.md)).
  ADR-031에 따라 툴체인은 저장소 밖에서 별도로 빌드/설치하고, `PATH`
  또는 CMake 캐시 변수로만 참조한다 — 저장소 안에 툴체인 소스를 두지
  않는다. M1 착수 전에 다음 중 하나가 완료되어야 한다:
  - Clang/LLVM 설치 (ADR-020 1순위) — freestanding 타깃(`--target=x86_64-unknown-none-elf`) 컴파일 확인.
  - 또는 `x86_64-elf-gcc`/`x86_64-elf-g++` 크로스 툴체인 설치(ADR-020 폴백).
  - `cmake --preset x86_64-clang -S . -B build/x86_64-clang`(또는 `-gcc`)이
    "컴파일러를 찾지 못함" 오류 없이 구성을 통과하면 준비 완료로 간주한다.

## 범위 (이번 계획에 포함)

### M1. Arch 부트 스텁 (x86_64) + 디버그 콘솔
- `kernel/arch/x86_64/boot/`: Multiboot2 헤더, 32→64비트 전환, 최소 GDT,
  항등 매핑 페이징 (spec/boot.md §1.1).
- `klog`(디버그 콘솔, [spec/debug-console.md](../spec/debug-console.md))
  구현 — 이 마일스톤과 이후 모든 마일스톤의 관찰 수단이다.
- 목표: QEMU에서 시리얼 포트로 "hello from kernel" 출력.
- 검증: `tools/run-qemu.sh x86_64`로 부팅 후 시리얼 로그 확인.

### M2. boot_info 파이프라인
- Multiboot2 태그 파서 → `boot_info`(spec/boot.md §3) 구성.
- 목표: 메모리맵 항목 수·initrd 위치를 시리얼 콘솔에 덤프.

### M3. 코어 메모리 관리
- `libk`: [cxx-conventions.md](../spec/cxx-conventions.md) §4의
  `result<T,E>`, `optional<T>`, `span<T>`, `intrusive_list`, `atomic<T>`,
  `spinlock` 최소 구현 — 이후 모든 마일스톤의 선행 조건.
- `kernel/core/mm`: [memory.md](../spec/memory.md)의 §2~4(노드별 풀 +
  per-CPU 로컬 프리 리스트, buddy 할당자, `alloc_pages`/`free_pages`)와
  §6(슬랩 힙) 구현. M1~M8 실행 환경에서는 노드가 1개뿐이라 사실상 단일
  풀처럼 동작한다(memory.md §1). §5(쿼터)는 `limit_bytes = UINT64_MAX`로
  두어 골격만 갖춘다(memory.md §7).

### M4. 객체/핸들 테이블
- `kernel/core/object`: [objects.md](../spec/objects.md)의 §1~3(핸들
  테이블, 프록시 트리 자료구조)과 §5~6(`sys_handle_close` 등 시스템콜,
  cascade revoke 절차) 구현. `thread`·`address_space` 커널 객체에
  [scheduler.md](../spec/scheduler.md) §2의 `thread_sched_fields`
  (선호 NUMA 노드 등, ADR-034/036)를 포함한다.
- 페이지테이블 조작 API(매핑/해제/권한 변경). COW 복제 프리미티브의
  자료구조 골격만 준비(ADR-016) — 실제 `fork` 시스템콜은 이 계획의 범위 밖.

### M5. 컨텍스트 스위치 + 스케줄러 골격
- `kernel/core/sched`: [scheduler.md](../spec/scheduler.md) §1~3(노드별
  `run_queue` 배열, 커널/유저 2단 밴드, 기본 라운드로빈)까지 구현.
  §4(승격 비례 타임슬라이스)·§5(승격 syscall)·§6(도네이션)은 M6 이후로
  미룬다. M1~M8 실행 환경은 노드가 1개이므로 배열은 원소 1개짜리로 동작.
- 목표: 커널 스레드 2개가 번갈아 실행됨을 시리얼 로그로 확인.

### M6. IPC: endpoint + Call/Reply
- `kernel/core/ipc`: [ipc.md](../spec/ipc.md) §2~4의
  `sys_call`/`sys_recv`/`sys_reply` 구현. 레지스터 기반 짧은 메시지
  (`label`+`regs`)만 우선, 페이지·핸들 전달은 M7.
- 도네이션 우선순위 상속([scheduler.md](../spec/scheduler.md) §6,
  ADR-028) 포함.
- 목표: 커널 스레드 2개 사이에 Call → Recv → Reply 왕복 성공.

### M7. IPC: 페이지·핸들 전달 + Notification
- `page_descriptor` 중 **copy 모드만** 우선 구현(move/map은 이후 계획,
  ADR-015/029). `sys_notify`/`sys_wait`(ipc.md §7) 구현.
- 핸들 전달([objects.md](../spec/objects.md) §4, ipc.md §4의
  `handles[]`) 구현 — M8의 initrun↔커널 왕복에서 실제로 쓰이진 않지만,
  이후 devmgr 등 서버 착수 시 바로 쓸 수 있도록 이 마일스톤에서 함께 완성.
- 목표: 1페이지 데이터를 두 스레드 사이에 copy 모드로 전달 성공.

### M8. initrun 로딩
- `tools/mkinitrd.*` 구현: MCPACK v1 포맷(spec/boot.md §5)으로 패키징.
- 커널: initrd 파서, ELF 로더, 유저모드 진입, boot_info 전달(ADR-030,
  spec/boot.md §6).
- `init/initrun`: "부팅 성공"만 출력하고 커널에 IPC Call을 보내는
  최소 바이너리.
- 목표: 커널이 유저모드 initrun을 실행하고, initrun이 커널에 보낸
  IPC Call에 대한 응답을 받는다 — **이 계획의 최종 완료 기준**.

### M9 이후 (이 계획의 범위 밖 — 별도 계획으로 예고)

M1~M8이 끝나면 다음은 별도 계획 문서로 다룬다 (ADR-035). 정책 자체는
이미 확정되어 있으므로([kernel-scheduler.md](../design/kernel-scheduler.md),
[kernel-memory.md](../design/kernel-memory.md) 참고) M9는 **구현**이 목표다:

- AP(보조 코어) 기동 — 부팅 직후 전부 즉시 기동(x86_64는
  INIT-SIPI-SIPI, aarch64는 PSCI `CPU_ON`), 코어 간 IPI로 즉시
  브로드캐스트하는 TLB shootdown 구현 (ADR-055).
- QEMU 다중 코어/다중 NUMA 노드 구성으로 M3~M5에서 만든 노드별 자료
  구조(노드별 메모리 풀, 노드별 run_queue, 워크 스틸링, 자동 메모리
  폴백)를 실제 다중 코어 환경에서 검증.
- 락 순서(lock ordering) 정적 규칙 문서화 (ADR-052).

## 범위 밖 (하지 않음)

- aarch64 이식 — M1~M8을 x86_64에서 먼저 완주한 뒤 별도 계획으로 다룬다.
- **SMP 실활성화(AP 기동)와 다중 노드 실환경 검증** — 위 M9 참고,
  이 계획은 자료구조만 SMP/NUMA 인지로 만들 뿐 실제로 여러 코어를
  띄우지는 않는다.
- IPC move/map 전달 모드, 우선순위 승격(ADR-027), 정책 서버 — 이후 계획.
- procsrv/vfs/memfs 등 실제 시스템 서버 — 이후 계획(이 계획은 initrun
  까지만 다룬다).
- 자동화 CI 파이프라인 구성.

## 검증 방법

각 마일스톤은 QEMU 부팅 로그로 확인 가능한 관찰 가능한 산출물(콘솔 출력,
왕복 성공/실패 로그)을 갖는다. M1 완료 시점에 `tools/`에 최소한의 스모크
테스트 스크립트(부팅 후 특정 로그 문자열 확인)를 추가한다.

## 완료 후

각 마일스톤(또는 M1~M8 전체) 완료 시 `docs/done/`에 결과를 기록한다.
분량이 크므로 마일스톤 3~4개 단위로 묶어 보고할 것을 권장하며, 실제
분할은 실행 시점에 판단한다. 이 계획 문서 자체는 실행 후에도 수정하지
않고 "실행 전 계획" 그대로 보존한다.
