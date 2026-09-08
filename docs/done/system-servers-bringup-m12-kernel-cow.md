# 완료 보고: system-servers-bringup M12 — 커널 측 COW/fork 선행 작업 (M12 자체는 미완료)

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M12
**관련 결정**: ADR-140, ADR-141(둘 다 [kernel-memory.md](../design/kernel-memory.md))
**실행일**: 2026-09-09~10

**주의**: 이 문서는 M12 전체의 완료 보고가 **아니다**. M12가 실제로
요구하는 QEMU 검증 목표("initrun이 부트 디스크를 마운트해 procsrv를
찾아 기동하고, procsrv가 자기 자신을 fork/exec해 두 번째 완전한 유저
프로세스를 만든다")는 아직 손대지 않았다 — virtio-blk 클라이언트,
cpio/INI 파서, `sys_process_spawn` syscall, procsrv 자체, 부트 디스크
빌드 도구 전부 미착수다. 이 문서는 그 앞에 반드시 필요한 **커널
프리미티브 4가지**(ADR-016이 요구했지만 M4 시점엔 구현하지 않고 미뤄
뒀던 것들)만 완료·검증했음을 기록한다. 나머지 범위는 이 세션 종료
시점의 대화 응답에서 "승인 필요 항목"으로 별도 정리했다(open-items.md
OPEN-53~57 참고).

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
(이제 분리된) 커널 스택에 직접 push. 유저 스레드가 2개 이상 동시에
존재하고 하나가 블로킹된 채 다른 하나가 syscall을 거는 순간 전역
스크래치가 서로 덮어쓰는 잠재 버그를 fork() 구현 전에 미리 없앴다.

## 검증 결과 (QEMU 실측, 정직하게 보고)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh       # 46개 전부 PASS (기존 42개 + 새 [cow]/[mm:refcount] 4개)
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS (변화 없음, 회귀 없음 확인)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS (변화 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS (변화 없음)
```

QEMU 실측(발췌):

```
[mm:refcount] initial=0 after_addref=2 release1=0 release2=0 release3=1 (expect 0,2,0,0,1)
[cow] after clone: parent_write=0 parent_cow=1 child_write=0 child_cow=1 same_phys=1 refcount=1 (expect 0,1,0,1,1,1)
[pf] cow copy virt=0x8000000000 old_phys=0xe000 new_phys=0xa000
[cow] child write fault: handled=1 child_write_after=1 child_phys_changed=1 refcount_after=0 (expect 1,1,1,0)
[pf] cow fast-path (sole owner) virt=0x8000000000 phys=0xe000
[cow] parent write fault: handled=1 parent_write_after=1 parent_phys_same=1 (expect 1,1,1)
```

- **확인함**: 참조 카운트 add/release 계약, COW 클론 후 부모/자식
  양쪽 모두 read-only+cow로 낮춰짐, 자식이 먼저 쓰면 복사(refcount
  있음→fast-path 아님), 그 뒤 부모가 쓰면 복사 없이 권한만
  재설정(refcount 0→fast-path) — 두 분기 모두 실제 프로덕션 함수
  (`try_handle_cow_write_fault_for`)를 직접 실행해 확인.
- **확인함**: A4 변경 후에도 기존 M8 유저 스레드(initrun)의 실제
  SYSCALL/SYSRET IPC Call 왕복이 그대로 성공.
- **버그 발견·수정 2건**(ADR-140/141 본문에 상세 기록):
  (1) `page_fault.cpp`에서 `mm::k_page_size`(uint32_t)를 그냥
  `~(k_page_size-1)`로 마스킹해 64비트 주소의 상위 비트가 잘려나가던
  버그. (2) M8의 전역 syscall 스크래치 2개가 다중 유저 스레드에서
  깨질 잠재 버그(아직 실제로는 안 드러났지만 fork() 직전에 미리 제거).
- **확인하지 못함**: 실제 `fork()` 시스템 콜을 통한 종단 간 흐름,
  유저 스레드 2개가 동시에 존재하며 한쪽이 블로킹된 채 다른 쪽이
  syscall을 거는 시나리오(A4가 고친 대상 그 자체) — 둘 다 아직
  fork()가 없어 만들 수 없다.
- **확인하지 못함**(M12 전체 관점): virtio-blk I/O, cpio/INI 파싱,
  실제 두 번째 유저 프로세스 생성 — 전부 미착수.

## 다음 단계

승인이 필요한 항목은 [open-items.md](../design/open-items.md)
OPEN-53~57로 등록해 뒀다 — `sys_process_spawn`/fork 관련 신규 syscall
ABI, procsrv IPC 와이어 프로토콜, OPEN-52(서비스 준비 신호)의 M12용
임시 방편 선택, 계정/로그인/crypto(procsrv.md §5/7/8) 범위를 M12에서
스킵하고 프로토콜 골격만 남기는 결정, virtio-blk/cpio 구현 순서.
