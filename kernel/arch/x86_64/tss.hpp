// TSS(Task State Segment) 최소 설정 — RSP0만 채운다 (docs/plan/
// system-servers-bringup.md §M12, ADR-143). usermode.S가 M8 시점에
// 이미 "TSS(RSP0)는 ring3→ring0 방향(인터럽트로 되돌아올 때)에만
// 필요하다"고 정확히 지적해 뒀지만, 그 방향(유저모드에서 실제로
// 예외/인터럽트가 발생하는 경우)이 M8~M11엔 한 번도 일어나지 않아
// (모든 예외 테스트가 커널 스레드=ring0에서만 있었다) 미뤄 둘 수
// 있었다. M12의 COW 쓰기 폴트가 실제 유저 스레드(ring3)에서 처음으로
// 발생하면서 더 이상 미룰 수 없게 됐다 — TSS.RSP0가 없으면(또는
// GDT에 TSS 디스크립터 자체가 없으면) CPU가 ring3→ring0 전환 시
// 어느 커널 스택으로 갈지 알 수 없어 그 자체로 결함(#GP/#DF, 최악의
// 경우 트리플 폴트)이 난다.
#pragma once

#include <cstdint>

namespace kern::object {
struct thread;  // kernel_objects.hpp(core) — 전방 선언만 필요(ADR-002와 같은 정신, 헤더 의존 최소화).
}

namespace kern::arch::x86_64 {

// GDT를 TSS 디스크립터(셀렉터 0x38)를 포함한 확장판으로 다시 구성하고
// LTR로 적재한다. kern::mm::init() 이후, 첫 유저 스레드가 뜨기 전에 호출해야
// 한다(install_syscall_entry()와 같은 시점 — kernel_main.cpp 참고).
void init_tss();

// M14(system-servers-bringup.md §M14, ADR-154, OPEN-58 해소) — t가
// (kern::object::thread::io_port_base/count로) 활성화해 둔 I/O 포트 범위를
// TSS IOPB에 반영한다 — **diff 기반**: 지금 IOPB에 실제로 프로그램된
// 범위(이 파일 내부에서 기억)와 다를 때만, 이전 범위를 재차단(1)하고
// 새 범위를 개방(0)한다. 매 호출마다 8KiB 전체를 만지지 않는다.
// init_tss() 이후에만 호출 가능. 두 자리에서 쓴다: (1) 컨텍스트
// 스위치마다(core/sched/scheduler.cpp의 extern "C" 훅
// arch_sync_io_permission을 통해 — 다음 스레드로 전환하기 직전),
// (2) sys_io_activate/sys_io_deactivate가 **지금 실행 중인 스레드
// 자신**에 대해 즉시 반영할 때(process_ops.cpp).
//
// ADR-147 시절의 grant_io_port_range()(전역 1회성, 스레드 구분 없음)
// 를 대체한다 — 이제 IOPB는 "지금 스케줄된 스레드가 활성화해 둔
// 범위만" 열린다.
void sync_io_permission(const kern::object::thread& t);

// M21(general-purpose-completion.md §M21, ADR-177) — t가 (다시)
// g_current가 될 때마다 TSS.RSP0를 t 전용 커널 스택(t.syscall_kernel_rsp,
// create_user_thread/create_forked_thread가 이미 만들어 둔 것)으로
// 맞춘다. **왜 필요해졌는가**: init_tss()가 RSP0를 딱 하나의 전역
// 스택으로 고정했던 이유(tss.cpp 주석 — "예외 처리는 항상 순차적,
// 처리 끝나면 곧바로 IRETQ")가 M21부터 깨진다 — 타이머 ISR이
// kern::sched::on_timer_tick() → yield()로 다른 스레드에게 전환할 수 있어,
// 이 인터럽트의 IRETQ가 "곧바로"가 아니라 "이 스레드가 나중에 다시
// 스케줄될 때"에야 일어난다. 그 사이에 다른 유저 스레드가 ring3에서
// 또 선점되면(전역 RSP0가 그대로였다면) **같은 물리 스택**의 같은
// 자리를 다시 밀어써 앞선 스레드의 보류 중인 인터럽트 프레임을
// 깨끗이 뭉갠다 — 실제로 QEMU에서 세 유저 스레드(busy/counter/initrun)
// 가 서로 선점하면서 스택이 오염돼 임의 위치에서 #PF로 죽는 것을
// 재현·확인했다. g_syscall_kernel_rsp를 전역 → 스레드별로 바꾼
// M12(ADR-141)의 같은 문제, 같은 해법이다 — 다만 이번엔 SYSCALL이
// 아니라 CPU 인터럽트 게이트(TSS.RSP0)가 대상이다. 커널 스레드
// (owner_space==nullptr)는 절대 ring3에서 실행되지 않으므로 RSP0가
// 읽힐 일이 없다(같은 특권 수준 인터럽트는 스택을 바꾸지 않는다) —
// sync_syscall_kernel_rsp()와 똑같이 그 경우는 그냥 건드리지 않는다.
void sync_exception_stack(const kern::object::thread& t);

// M28(real-libc-syscall-layer.md §M28, ADR-183) — t.fs_base(sys_arch_prctl_set_fs가
// 채운 값, 기본 0)를 IA32_FS_BASE MSR에 WRMSR로 반영한다. musl의
// __init_tp(src/env/__init_tls.c)가 __set_thread_area(ARCH_SET_FS)를
// TLS/errno 접근의 전제조건으로 무조건 호출하므로(실패하면 a_crash()),
// 원래 M30으로 계획됐던 이 기능을 M28로 앞당겼다(done 보고 참고).
// arch_sync_io_permission/arch_sync_exception_stack과 같은 자리
// (컨텍스트 스위치 4곳)에서 함께 호출한다 — 커널 스레드도 항상 0으로
// 덮어써 이전 유저 스레드의 FS_BASE가 새어 나가지 않게 한다(다른 두
// 훅과 달리 owner_space==nullptr 체크를 하지 않는 이유 — 커널 스레드는
// FS_BASE를 쓰지 않으므로 0으로 두는 것이 안전하다).
void sync_fs_base(const kern::object::thread& t);

}  // namespace kern::arch::x86_64
