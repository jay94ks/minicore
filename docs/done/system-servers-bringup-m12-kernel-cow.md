# 완료 보고: system-servers-bringup M12 — 커널 측 fork/exec/spawn 전체 검증 (M12 자체는 아직 미완료)

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M12
**관련 결정**: ADR-140~145([kernel-memory.md](../design/kernel-memory.md)
ADR-140/141/142/145, [kernel-scheduler.md](../design/kernel-scheduler.md)
ADR-144, [boot-and-drivers.md](../design/boot-and-drivers.md) ADR-143)
**실행일**: 2026-09-09~10

**주의**: 이 문서는 M12 전체의 완료 보고가 **아니다**. M12가 실제로
요구하는 QEMU 검증 목표("initrun이 부트 디스크를 마운트해 procsrv를
찾아 기동하고, procsrv가 자기 자신을 fork/exec해 두 번째 완전한 유저
프로세스를 만든다")의 **procsrv/virtio-blk/cpio 부분**은 아직 손대지
않았다. 하지만 그 목표가 요구하는 **커널 프리미티브 전체**(COW,
fork/exec/process_spawn syscall, 협조적 스케줄러 하에서 여러 독립
주소공간의 생성·전환·회수)는 **initrun이 자기 자신을 fork/exec/spawn
하는 자체 검증 경로로 실제 QEMU에서 종단간 확인을 마쳤다** — 남은
범위는 procsrv 자체·virtio-blk·cpio/INI 파서·부트 디스크 빌드
도구뿐이며, OPEN-54~57로 승인 대기 중이다(OPEN-53은 이번에 해결함).

## 완료한 것

### A1. 물리 프레임 참조 카운트 (ADR-140 §1)

[page_allocator.hpp](../../kernel/core/mm/page_allocator.hpp)/
[.cpp](../../kernel/core/mm/page_allocator.cpp)에 order-0 전용 참조
카운트 테이블 추가. `frame_add_ref`/`frame_release`/`frame_ref_count`
세 함수. `frame_release`는 "내가 마지막 소유자였는가"를 `bool`로 직접
반환한다. `free_pages(order=0)`도 이 API로 갈아타 공유 중인 프레임은
실제로 반환하지 않는다.

### A2+A3. PTE COW 소프트웨어 비트 + `clone_address_space_cow` + 쓰기 폴트 핸들러 (ADR-140 §2~4)

[page_table.hpp](../../kernel/arch/x86_64/page_table.hpp)/
[.cpp](../../kernel/arch/x86_64/page_table.cpp)에 `page_perm::cow`
(PTE bit 9) 추가 + `clone_address_space_cow(src_pml4_phys)`(유저 영역만
COW로 복제, 부모/자식 양쪽 write 제거 + cow 세팅 + `frame_add_ref`).
새 파일 [page_fault.hpp](../../kernel/arch/x86_64/page_fault.hpp)/
[.cpp](../../kernel/arch/x86_64/page_fault.cpp) + IDT 벡터 14(`#PF`)
라우팅 — COW 표시된 페이지의 쓰기 폴트에서 `frame_release`가 true
(마지막 소유자)면 그냥 쓰기 권한만 다시 켜고, false(아직 다른
소유자)면 새 프레임에 복사한다.

### A4. syscall 진입 커널 스택 스레드별 분리 (ADR-141)

M8이 전역 스크래치 2개(`g_syscall_kernel_rsp`, `g_syscall_saved_user_rsp`)
로 처리하던 부분을 스레드별로 분리 — `object::thread::syscall_kernel_rsp`
필드 + `create_user_thread`의 사전 계산 + `scheduler.cpp`의
`sync_syscall_kernel_rsp()`(스레드 전환마다 전역을 그 스레드 값으로
맞춤) + `syscall_entry.S`가 유저 RSP를 전역이 아니라 그 스레드 자신의
(이제 분리된) 커널 스택에 직접 push.

### A5. `sys_fork`/`sys_process_spawn`/`sys_exec`/`sys_thread_exit` (ADR-142)

새 파일 [process_ops.hpp](../../kernel/arch/x86_64/process_ops.hpp)/
[.cpp](../../kernel/arch/x86_64/process_ops.cpp) + `uapi.hpp`에 syscall
번호 1~4. `sys_fork`는 `syscall_entry.S`가 매 syscall마다 콜리세이브
레지스터(rbx/rbp/r12~r15)까지 포함해 9워드를 저장해 두고,
`sched::create_forked_thread()`가 자식의 손짜기 초기 스택에 그 값을
그대로 채워 새 arch 훅 `arch_fork_child_resume`으로 진입시킨다 — 그
결과 자식이 **부모가 SYSCALL을 실행한 바로 그 지점**에서 콜리세이브
레지스터까지 동일한 값으로 재개된다(RAX=0만 다르다), 진짜 POSIX
fork() 의미론. `sys_exec`는 새 주소공간+ELF로 스레드의 실행 이미지를
교체하고 성공하면 반환하지 않는다. `sys_thread_exit`은 협조적
스케줄러에서 유저 스레드가 스스로 물러나는 유일한 수단이다.

이 네 syscall을 실제로 왕복 검증하기 위해, procsrv/cpio가 아직 없는
동안만 쓰는 임시 다리를 놓았다 — `kernel_main.cpp::setup_initrun_process`
가 initrun 자신의 원본 ELF 바이트를 initrun의 주소공간에도 매핑해
두고(`uapi::k_m12_self_elf_user_vaddr`), initrun이 그 바이트로 **자기
자신을 fork/exec/spawn**해 보는 식이다(`init/initrun/main.cpp`). 이
다리는 procsrv/cpio가 실제로 생기면 통째로 제거된다.

## 실행 중 발견한 버그 3가지 (전부 처음 발생 — M8~M11은 드러날 조건 자체가 없었다)

### 버그 1: TSS(RSP0) 자체가 없었다 (ADR-143)

`usermode.S`가 M8 시점에 이미 "TSS는 ring3→ring0 방향에만 필요하다"고
정확히 지적해 뒀지만, 그 방향(유저 스레드의 실제 예외/인터럽트)이
M8~M11 어디에도 없어 미뤄 둘 수 있었다. `sys_fork`의 자식이 자기
스택에 처음 쓰려는 순간 COW 쓰기 폴트가 **실제 ring3에서 최초로**
발생하면서, TSS 없이는 CPU가 ring3→ring0 전환 시 갈 곳을 몰라 아무
진단도 남기지 못한 채 멈췄다(`MINICORE_QEMU_GDB=1`로 직접 확인).
[tss.hpp](../../kernel/arch/x86_64/tss.hpp)/[.cpp](../../kernel/arch/x86_64/tss.cpp)
로 최소 TSS+GDT 확장을 추가해 해결.

### 버그 2: `broadcast_tlb_shootdown()`이 로컬 코어 자신의 TLB는 지운 적이 없었다 (ADR-144)

`g_cpu_count<=1`이면 함수 전체를 건너뛰던 로직이 "다른 코어에 알릴
필요 없음"과 "이 코어 자신의 TLB를 지울 필요 없음"을 혼동했다 —
매핑을 바꾼 뒤 **같은 코어가 그 가상주소를 실제 load/store로
재접근**하는 경로가 M4~M11 검증(전부 `query_page()`로만 확인, TLB를
거치지 않음)에서 한 번도 실행되지 않아 드러날 기회가 없었다. COW
쓰기 폴트가 IRETQ로 실패한 명령어를 재시도하면서 이 경로를 처음
실행했고, 부모/자식이 서로의(또는 이미 사라진) 물리 프레임을 계속
보는 것처럼 뒤섞여 동작했다. `invlpg`를 코어 수와 무관하게 항상
먼저 실행하도록 고쳐 해결.

### 버그 3: `create_address_space_root()`가 pml4 index 0을 통째로 공유했다 (ADR-145)

GDT 접근을 위해 [0,8MiB)만 공유하려던 의도(ADR-121)와 달리, 실제
구현(`table[0] = pml4[0]`)은 그 인덱스가 가리키는 512GiB 전체를
공유해 버렸다 — initrun의 링크 주소(`0x10000000`)도 그 안에
속한다. M8~M11은 유저 주소공간이 동시에 하나뿐이라 드러나지
않았다. `sys_process_spawn`으로 두 번째 독립 주소공간을 만들어 같은
주소에 매핑하자 `already_mapped`로 충돌했다. 이 주소공간 전용
`low_pdpt`/`low_pd`를 새로 만들어 [0,8MiB) **내용만** 복사하고
나머지는 비우도록 고치고, `clone_address_space_cow()`의 유저 영역
순회도 index 0부터(단 [0,8MiB) 제외) 돌도록 함께 고쳐 해결.

## 검증 결과 (QEMU 실측, 정직하게 보고)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh       # 49개 전부 PASS (기존 46개 + fork/exec/spawn 3개)
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS (회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS (회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS (회귀 없음)
```

QEMU 실측(발췌, 세 버그를 모두 고친 뒤의 최종 정상 흐름):

```
[process] fork ok child_pml4=0x160000
[pf] cow copy virt=0x6ffffffff000 old_phys=0x16a000 new_phys=0x179000    ← 부모 자신의 스택 COW
[initrun] kernel received boot call ok=1 label=0xb007 ... - 부팅 성공    ← 부모의 기존 M8 boot call
[pf] cow fast-path (sole owner) virt=0x6ffffffff000 phys=0x16a000        ← 자식의 스택 COW(이미 단독 소유)
[process] exec ok entry=0x10000000                                       ← 자식이 자기 자신을 exec
[process] spawn ok entry=0x10000000 trusted=0                            ← 부모가 세 번째 사본을 spawn
```

- **확인함**: `sys_fork`가 진짜 POSIX 의미론대로 동작한다 — 자식이
  부모의 호출 지점으로(콜리세이브 레지스터까지 동일하게) 복귀하고,
  각자의 스택이 독립적인 COW 사본으로 분리된다.
- **확인함**: `sys_exec`가 실행 이미지를 실제로 교체하고 새 진입점에서
  정상 실행을 시작한다.
- **확인함**: `sys_process_spawn`이 완전히 독립적인 세 번째 프로세스를
  만들어 정상 실행·종료시킨다.
- **확인함**: `sys_thread_exit`으로 세 개의 독립된 유저 주소공간(부모/
  자식/스폰된 프로세스)이 협조적 스케줄러 위에서 순서대로 정확히
  넘겨받으며 실행된다.
- **확인하지 못함**(M12 전체 관점): virtio-blk I/O, cpio/INI 파싱,
  procsrv 자체 — 전부 미착수.
- **확인하지 못함**: `sys_exec`의 "이전 주소공간 회수"(현재는 의도적
  누수, process_ops.hpp에 명시) — 실제 회수는 이후 마일스톤.

## 다음 단계

승인이 필요한 항목은 [open-items.md](../design/open-items.md)
OPEN-54~57로 남아 있다 — procsrv IPC 와이어 프로토콜, OPEN-52(서비스
준비 신호)의 M12용 임시 방편 선택, 계정/로그인/crypto(procsrv.md
§5/7/8) 범위를 M12에서 스킵하고 프로토콜 골격만 남기는 결정,
virtio-blk/cpio 구현 순서. OPEN-53(신규 syscall ABI)은 이번에
ADR-142로 해결했다.
