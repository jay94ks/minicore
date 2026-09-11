// 완전한 signal 계층 구현 (signal.hpp 상단 주석 참고).
#include "signal.hpp"

#include <cstdint>

#include <klog.hpp>
#include <object/handle_table.hpp>
#include <object/kernel_objects.hpp>
#include <sched/scheduler.hpp>

namespace kern::arch::x86_64 {

namespace {

constexpr uint32_t k_sigkill = 9;

bool valid_signal_number(uint32_t sig) { return sig >= 1 && sig < kern::object::thread::k_max_signal; }

// syscall_entry.S의 saved_regs 배열([0]=r15..[8]=user_rsp, syscall.cpp
// 상단 주석과 동일)과 짝을 맞춘 시그널 프레임 — 핸들러가 끝난 뒤
// sys_rt_sigreturn이 이 값들을 그대로 읽어 원래 실행을 복원한다.
// 이 레이아웃은 이 파일(쓰는 쪽: check_signal_delivery, 읽는 쪽:
// signal_return) 밖으로 새어나갈 필요가 없다 — musl의 실제
// ucontext_t/sigcontext 레이아웃을 흉내내지 않는다(이 프로젝트만의
// 자체 ABI, ADR-183 §결정1과 같은 정신 — 실제 Linux 바이너리 호환은
// 목표가 아니다).
struct signal_frame {
    uint64_t saved_r15;
    uint64_t saved_r14;
    uint64_t saved_r13;
    uint64_t saved_r12;
    uint64_t saved_rbp;
    uint64_t saved_rbx;
    uint64_t saved_rflags;
    uint64_t saved_rip;
    uint64_t saved_user_rsp;
    uint64_t saved_rax;       // 인터럽트된 syscall 자신의 반환값 — sigreturn이 RAX에 복원해야 한다.
    uint64_t signal_number;
};

}  // namespace

signal_send_error signal_send(kern::object::handle_table& caller_handles, uint32_t h,
                               uint32_t signal_number) {
    if (!valid_signal_number(signal_number)) {
        return signal_send_error::invalid_handle;  // 알 수 없는 번호 — 조용히 무시할 이유(전달 대상 자체가 무의미)로 재사용.
    }
    if (signal_number == k_sigkill) {
        return signal_send_error::is_sigkill;
    }
    const kern::object::handle_entry* e = caller_handles.debug_entry(h);
    if (e == nullptr || !e->valid) {
        return signal_send_error::invalid_handle;
    }
    if (e->kind != kern::object::object_kind::thread) {
        return signal_send_error::wrong_object_type;
    }
    if ((e->rights & kern::object::k_right_can_signal) == 0) {
        return signal_send_error::permission_denied;
    }
    auto* target = static_cast<kern::object::thread*>(e->object);
    target->pending_signals |= (1ull << (signal_number - 1));
    return signal_send_error::ok;
}

void signal_action(kern::object::thread& self, uint32_t signal_number,
                    mc_signal_action_request& req) {
    if (!valid_signal_number(signal_number) || signal_number == k_sigkill) {
        // SIGKILL/잘못된 번호 — 조회는 항상 "등록 없음"으로, 등록
        // 시도는 조용히 무시한다(ADR-186 §결정4 — SIGKILL은 이
        // 메커니즘을 전혀 모른다).
        req.out_old_handler = 0;
        req.out_old_restorer = 0;
        return;
    }
    kern::object::thread::signal_action& slot = self.sigactions[signal_number];
    if (req.want_old != 0) {
        req.out_old_handler = slot.handler;
        req.out_old_restorer = slot.restorer;
    }
    if (req.set_new != 0) {
        slot.handler = req.new_handler;
        slot.restorer = req.new_restorer;
    }
}

}  // namespace kern::arch::x86_64

namespace {
// M55(musl-userland-porting.md §M55, ADR-226) — SIGINT 하나만
// "기본 동작(SIG_DFL)=진짜 종료"로 하드코딩한다. 나머지 31개
// 시그널은 여전히 ADR-211의 "SIG_DFL=무시" 단순화 그대로다
// (OPEN-75) — 이 라운드가 실제로 보낼 시그널이 SIGINT 하나뿐이라
// 함께 확장할 이유가 없다. check_signal_delivery()가 kern::arch::
// x86_64 네임스페이스 밖(extern "C")이라 위 익명 네임스페이스의
// k_sigkill과 같은 자리에 둘 수 없어 여기 따로 둔다.
constexpr uint32_t k_sigint = 2;
}  // namespace

extern "C" mc_signal_dispatch_result check_signal_delivery(uint64_t* saved_regs,
                                                            uint64_t dispatch_ret) {
    kern::object::thread* self = kern::sched::current();
    if (self == nullptr) {
        return {dispatch_ret, 0};
    }

    uint64_t deliverable = self->pending_signals & ~self->signal_mask;
    for (uint32_t sig = 1; sig < kern::object::thread::k_max_signal; ++sig) {
        uint64_t bit = 1ull << (sig - 1);
        if ((deliverable & bit) == 0) {
            continue;
        }
        self->pending_signals &= ~bit;  // 이 시그널은 소비한다(전달하든 버리든).

        uint64_t handler = self->sigactions[sig].handler;
        if (handler == 0 && sig == k_sigint) {
            // M55(ADR-226) — 핸들러를 등록하지 않은 SIGINT는 진짜로
            // 이 스레드를 종료시킨다. sys_process_kill(ADR-178)이
            // 이미 쓰는 것과 정확히 같은 비동기 종료 요청이다 —
            // "대기열에 갇힌 대상은 다시 깨우지 않으면 안 폐기된다"
            // 는 기존 한계(OPEN-65)도 그대로 물려받는다, 새로 풀지
            // 않는다.
            kern::sched::request_kill(*self);
            continue;
        }
        if (handler == 0 || handler == 1) {
            // SIG_DFL(0, SIGINT 제외)/SIG_IGN(1) — 이 라운드는 둘 다
            // "무시"로 단순화한다(진짜 기본 동작 — 대부분 프로세스
            // 종료 — 는 ADR-186이 이미 범위 밖으로 남겼다). 다음
            // 대기 중인 시그널이 있는지 계속 살펴본다.
            continue;
        }

        // 이 시그널을 실제로 전달한다 — 유저 스택에 손으로 프레임을
        // 쌓고, 이 syscall이 SYSRET 대신 핸들러로 "점프"하게 만든다
        // (그 핸들러가 정상적으로 ret하면 restorer(musl의
        // __restore_rt)로 떨어지고, 그 restorer가 sys_rt_sigreturn을
        // 불러 이 프레임을 다시 읽어 원래 실행을 복원한다).
        uint64_t user_rsp = saved_regs[8];
        uint64_t raw = user_rsp - sizeof(kern::arch::x86_64::signal_frame) - 8;
        // SysV: 핸들러 진입 시점(= "누군가 call한 것처럼 보여야 함")
        // RSP mod 16 == 8이어야 한다 — 8을 뺀 값을 16-정렬로
        // 내림한 뒤 다시 8을 더하면 항상 raw 이하이면서 mod16==8이다.
        uint64_t new_rsp = ((raw - 8) & ~0xFULL) + 8;

        auto* frame = reinterpret_cast<kern::arch::x86_64::signal_frame*>(new_rsp + 8);
        frame->saved_r15 = saved_regs[0];
        frame->saved_r14 = saved_regs[1];
        frame->saved_r13 = saved_regs[2];
        frame->saved_r12 = saved_regs[3];
        frame->saved_rbp = saved_regs[4];
        frame->saved_rbx = saved_regs[5];
        frame->saved_rflags = saved_regs[6];
        frame->saved_rip = saved_regs[7];
        frame->saved_user_rsp = user_rsp;
        frame->saved_rax = dispatch_ret;
        frame->signal_number = sig;
        *reinterpret_cast<uint64_t*>(new_rsp) = self->sigactions[sig].restorer;

        saved_regs[7] = handler;   // rip <- 핸들러.
        saved_regs[8] = new_rsp;   // rsp <- 새로 쌓은 프레임.

        return {dispatch_ret, sig};  // RDI <- 시그널 번호(핸들러의 int sig 인자).
    }
    return {dispatch_ret, 0};
}

// M36 — sys_rt_sigreturn(syscall.cpp의 MC_SYSCALL_RT_SIGRETURN 케이스가
// 부른다). 이 syscall을 부른 시점의 유저 rsp(saved_regs[8])는
// 정확히 &signal_frame이다 — restorer(musl의 __restore_rt, 단
// 두 줄: mov $15,%rax; syscall)가 rsp를 전혀 안 건드리기 때문이다.
extern "C" uint64_t mc_signal_return(uint64_t* saved_regs) {
    const auto* frame =
        reinterpret_cast<const kern::arch::x86_64::signal_frame*>(saved_regs[8]);
    saved_regs[0] = frame->saved_r15;
    saved_regs[1] = frame->saved_r14;
    saved_regs[2] = frame->saved_r13;
    saved_regs[3] = frame->saved_r12;
    saved_regs[4] = frame->saved_rbp;
    saved_regs[5] = frame->saved_rbx;
    saved_regs[6] = frame->saved_rflags;
    saved_regs[7] = frame->saved_rip;
    saved_regs[8] = frame->saved_user_rsp;
    return frame->saved_rax;
}
