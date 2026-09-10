// 완전한 signal 계층 (real-libc-syscall-layer.md §M36, ADR-186).
// SIGKILL은 이 파일이 전혀 모른다 — 그대로 process_ops.hpp::process_kill
// (ADR-178)의 몫이다(ADR-186 §결정4, 일반화하지 않는다). 이 파일은
// 나머지 표준 시그널(1~31)만 다룬다.
#pragma once

#include <cstdint>

#include <mc/syscall.h>

namespace kern::object {
class handle_table;
struct thread;
}  // namespace kern::object

namespace kern::arch::x86_64 {

enum class signal_send_error : uint32_t {
    ok = 0,
    invalid_handle,      // 핸들이 없거나 이미 닫힘.
    wrong_object_type,   // object_kind::thread가 아님.
    permission_denied,   // k_right_can_signal 없음.
    is_sigkill,          // SIGKILL은 이 경로를 거부한다 — sys_process_kill을 쓴다.
};

// sys_signal_send(a1=대상 handle, a2=signal_number) — 대상의
// pending_signals에 비트를 세운다. 즉시 아무 것도 실행하지 않는다
// (ADR-178의 kill_requested 관례 그대로) — 실제 전달은 대상이 다음
// syscall에서 리턴할 때 check_signal_delivery()가 확인한다.
signal_send_error signal_send(kern::object::handle_table& caller_handles, uint32_t h,
                               uint32_t signal_number);

// sys_signal_action(a1=signal_number, a2=req 유저 가상주소) — 호출한
// 스레드 **자신**의 시그널 처리기를 등록/조회한다(프로세스=스레드
// 1:1이라 "자기 자신"이 곧 "이 프로세스"). SIGKILL/신호 0/32는
// 조용히 무시한다(등록 시도해도 아무 효과 없음, 조회는 항상
// handler=0을 돌려준다) — ADR-186 §결정4.
void signal_action(kern::object::thread& self, uint32_t signal_number,
                    mc_signal_action_request& req);

}  // namespace kern::arch::x86_64

// syscall_entry.S가 `call syscall_dispatch` 직후, 유저모드로 SYSRET
// 하기 **전에** 부른다(return-to-user 경계, ADR-186 §결정3) —
// saved_regs(그 시점의 %rsp, 즉 &saved_regs[0])가 가리키는 9워드
// 블록을 필요하면 그 자리에서 다시 써서(rip/user_rsp 등) 이 스레드가
// 실제로는 등록된 핸들러로 먼저 들어가게 만든다. 반환값의 rax_value
// 는 SYSRET 시점 RAX(보통 dispatch_ret 그대로, 시그널 전달이
// 일어나도 핸들러는 RAX를 안 보므로 상관없다)로, rdi_value는 RDI
// (전달 시 시그널 번호, 아니면 0)로 각각 쓰인다 — 작은 두 필드짜리
// POD 구조체라 SysV ABI가 RAX:RDX로 자동 반환한다(추가 스택 조작
// 없이 syscall_entry.S에서 그대로 옮겨 쓸 수 있다).
struct mc_signal_dispatch_result {
    uint64_t rax_value;
    uint64_t rdi_value;
};

extern "C" mc_signal_dispatch_result check_signal_delivery(uint64_t* saved_regs,
                                                             uint64_t dispatch_ret);

// MC_SYSCALL_RT_SIGRETURN(syscall.cpp)이 부른다 — signal.cpp 상단
// 주석 참고. saved_regs를 그 자리에서 다시 써서 시그널 프레임에
// 저장된 원래 실행 상태를 복원하고, 그 원래 syscall의 반환값(RAX로
// 쓸 값)을 돌려준다.
extern "C" uint64_t mc_signal_return(uint64_t* saved_regs);
