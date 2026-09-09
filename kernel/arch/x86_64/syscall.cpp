// SYSCALL MSR 설정 + syscall 디스패처 (docs/plan/kernel-bootstrap.md M8).
// 실제 진입/복귀(스택 전환, RCX/R11 보존, SYSRETQ)는 syscall_entry.S가
// 맡는다 — 이 파일은 그 진입을 준비하는 MSR 설정과, 진입 후 C++에서
// syscall 번호로 분기하는 부분만 다룬다.
//
// 이 milestone은 syscall 번호를 딱 하나(uapi::k_syscall_ipc_call)만
// 인식한다 — initrun 데모가 그 이상을 쓰지 않는다(kernel-bootstrap.md
// M8 목표: "IPC Call에 대한 응답을 받는다"). 알 수 없는 번호는
// invalid_handle로 취급한다(적당한 매핑이 없어 가장 가까운 기존
// ipc_error를 재사용 — 전용 syscall 에러 코드 체계는 이후 계획).
#include "syscall.hpp"

#include "gdt_selectors.hpp"
#include "process_ops.hpp"

#include <cstdint>

#include <ipc/endpoint.hpp>
#include <sched/scheduler.hpp>

#include <klog.hpp>
#include <uapi.hpp>

namespace {

constexpr uint32_t k_msr_efer = 0xC0000080;
constexpr uint32_t k_msr_star = 0xC0000081;
constexpr uint32_t k_msr_lstar = 0xC0000082;
constexpr uint32_t k_msr_fmask = 0xC0000084;
constexpr uint64_t k_efer_sce = 1ull << 0;

uint64_t rdmsr(uint32_t msr) {
    uint32_t lo;
    uint32_t hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = static_cast<uint32_t>(value);
    uint32_t hi = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" ::"c"(msr), "a"(lo), "d"(hi));
}

}  // namespace

extern "C" void syscall_entry();

namespace arch_x86_64 {

void install_syscall_entry() {
    wrmsr(k_msr_efer, rdmsr(k_msr_efer) | k_efer_sce);

    // STAR[63:48]/[47:32] — boot.S의 GDT 배치(gdt_selectors.hpp)에 맞춘
    // 값. SYSCALL: CS=STAR[47:32](k_sel_code64=0x18), SS=+8(0x20=커널
    // data64, 이미 지금 커널이 쓰는 값과 같다). SYSRET(64비트): CS=
    // STAR[63:48]+16, SS=STAR[63:48]+8 — 그래서 STAR[63:48]에는
    // user_data64 그 자체(0x28)가 아니라 8을 뺀 값(0x20)을 넣는다
    // (Intel SDM의 SYSRET 규약, boot.S GDT 주석 참고).
    uint64_t star = (static_cast<uint64_t>(k_sel_user_data64 - 8) << 48) |
                    (static_cast<uint64_t>(k_sel_code64) << 32);
    wrmsr(k_msr_star, star);

    wrmsr(k_msr_lstar, reinterpret_cast<uint64_t>(&syscall_entry));

    // SYSCALL 진입 시 RFLAGS에서 클리어할 비트: IF(0x200, 아직 인터럽트
    // 인프라가 없어 큰 의미는 없지만 관례상 맞춘다), DF(0x400, SysV
    // 호출 규약이 함수 진입 시 DF=0을 요구한다).
    wrmsr(k_msr_fmask, 0x600);
}

}  // namespace arch_x86_64

// saved_regs(M12, ADR-142) — syscall_entry.S가 %r8로 넘긴다. push 역순
// 배열: [0]=r15,[1]=r14,[2]=r13,[3]=r12,[4]=rbp,[5]=rbx,[6]=rflags,
// [7]=rip,[8]=user_rsp(syscall_entry.S 상단 주석과 정확히 대응). fork
// 외의 syscall은 이 값을 쓰지 않는다.
extern "C" uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                                      const uint64_t* saved_regs) {
    switch (num) {
        case uapi::k_syscall_ipc_call: {
            object::thread* self = sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(ipc::ipc_error::invalid_handle);
            }
            const auto* msg_in = reinterpret_cast<const ipc::message*>(a2);
            auto* msg_out = reinterpret_cast<ipc::message*>(a3);
            if (msg_in == nullptr || msg_out == nullptr) {
                return static_cast<uint64_t>(ipc::ipc_error::invalid_handle);
            }
            auto result =
                ipc::sys_call(*self->handles, static_cast<object::handle>(a1), *msg_in, *msg_out);
            return static_cast<uint64_t>(result.is_ok() ? ipc::ipc_error::ok : result.error());
        }
        case uapi::k_syscall_ipc_recv: {
            object::thread* self = sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(ipc::ipc_error::invalid_handle);
            }
            auto* msg_out = reinterpret_cast<ipc::message*>(a2);
            if (msg_out == nullptr) {
                return static_cast<uint64_t>(ipc::ipc_error::invalid_handle);
            }
            auto result =
                ipc::sys_recv(*self->handles, static_cast<object::handle>(a1), *msg_out);
            return static_cast<uint64_t>(result.is_ok() ? ipc::ipc_error::ok : result.error());
        }
        case uapi::k_syscall_ipc_reply: {
            object::thread* self = sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(ipc::ipc_error::invalid_handle);
            }
            const auto* msg_in = reinterpret_cast<const ipc::message*>(a1);
            if (msg_in == nullptr) {
                return static_cast<uint64_t>(ipc::ipc_error::invalid_handle);
            }
            auto result = ipc::sys_reply(*self->handles, *msg_in);
            return static_cast<uint64_t>(result.is_ok() ? ipc::ipc_error::ok : result.error());
        }
        case uapi::k_syscall_process_spawn: {
            auto* req = reinterpret_cast<uapi::process_spawn_request*>(a1);
            if (req == nullptr || req->elf_data == 0) {
                return static_cast<uint64_t>(arch_x86_64::process_spawn_error::invalid_argument);
            }
            if (req->inherited_handle_count > uapi::k_max_spawn_inherited_handles) {
                return static_cast<uint64_t>(arch_x86_64::process_spawn_error::invalid_argument);
            }
            auto err = arch_x86_64::process_spawn(
                reinterpret_cast<const uint8_t*>(req->elf_data), req->elf_size,
                reinterpret_cast<const uint8_t*>(req->argv_blob), req->argv_size,
                req->grant_trusted, req->create_endpoint, req->inherited_handles,
                req->inherited_handle_count, req->out_endpoint_proxy_handle,
                req->out_thread_handle);
            return static_cast<uint64_t>(err);
        }
        case uapi::k_syscall_fork: {
            return arch_x86_64::fork_current(/*rip=*/saved_regs[7], /*rflags=*/saved_regs[6],
                                              /*user_rsp=*/saved_regs[8], /*rbx=*/saved_regs[5],
                                              /*rbp=*/saved_regs[4], /*r12=*/saved_regs[3],
                                              /*r13=*/saved_regs[2], /*r14=*/saved_regs[1],
                                              /*r15=*/saved_regs[0]);
        }
        case uapi::k_syscall_exec: {
            const auto* req = reinterpret_cast<const uapi::exec_request*>(a1);
            if (req == nullptr || req->elf_data == 0) {
                return static_cast<uint64_t>(arch_x86_64::process_spawn_error::invalid_argument);
            }
            // 성공하면 이 호출은 반환하지 않는다(process_ops.hpp 참고) —
            // 실패했을 때만 아래로 떨어진다.
            auto err = arch_x86_64::exec_current(
                reinterpret_cast<const uint8_t*>(req->elf_data), req->elf_size,
                reinterpret_cast<const uint8_t*>(req->argv_blob), req->argv_size);
            return static_cast<uint64_t>(err);
        }
        case uapi::k_syscall_thread_exit: {
            sched::exit();  // noreturn.
        }
        case uapi::k_syscall_alloc_dma_buffer: {
            auto* out = reinterpret_cast<uapi::dma_buffer_result*>(a1);
            if (out == nullptr) {
                return static_cast<uint64_t>(arch_x86_64::process_spawn_error::invalid_argument);
            }
            uint64_t virt = 0;
            uint64_t phys = 0;
            auto err = arch_x86_64::alloc_dma_buffer(static_cast<uint32_t>(a2), virt, phys);
            if (err == arch_x86_64::process_spawn_error::ok) {
                out->virt_addr = virt;
                out->phys_addr = phys;
            }
            return static_cast<uint64_t>(err);
        }
        case uapi::k_syscall_debug_log: {
            const auto* str = reinterpret_cast<const char*>(a1);
            if (str == nullptr) {
                return 1;
            }
            uint64_t len = a2;
            if (len > uapi::k_max_debug_log_bytes) {
                len = uapi::k_max_debug_log_bytes;
            }
            char buf[uapi::k_max_debug_log_bytes + 1];
            for (uint64_t i = 0; i < len; ++i) {
                buf[i] = str[i];
            }
            buf[len] = '\0';
            klog::printf("%s", buf);
            return 0;
        }
        case uapi::k_syscall_map_phys: {
            auto* req = reinterpret_cast<uapi::map_phys_request*>(a1);
            if (req == nullptr) {
                return static_cast<uint64_t>(arch_x86_64::process_spawn_error::invalid_argument);
            }
            uint64_t virt = 0;
            auto err = arch_x86_64::map_phys(req->phys_addr, req->size, virt);
            if (err == arch_x86_64::process_spawn_error::ok) {
                req->out_virt_addr = virt;
            }
            return static_cast<uint64_t>(err);
        }
        case uapi::k_syscall_io_activate: {
            auto err = arch_x86_64::io_activate(static_cast<uint16_t>(a1), static_cast<uint16_t>(a2));
            return static_cast<uint64_t>(err);
        }
        case uapi::k_syscall_io_deactivate: {
            auto err = arch_x86_64::io_deactivate();
            return static_cast<uint64_t>(err);
        }
        case uapi::k_syscall_process_kill: {
            object::thread* self = sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(arch_x86_64::process_kill_error::invalid_handle);
            }
            auto err = arch_x86_64::process_kill(*self->handles, static_cast<uint32_t>(a1));
            return static_cast<uint64_t>(err);
        }
        default:
            return static_cast<uint64_t>(ipc::ipc_error::invalid_handle);
    }
}
