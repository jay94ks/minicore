// SYSCALL MSR 설정 + syscall 디스패처 (docs/plan/kernel-bootstrap.md M8).
// 실제 진입/복귀(스택 전환, RCX/R11 보존, SYSRETQ)는 syscall_entry.S가
// 맡는다 — 이 파일은 그 진입을 준비하는 MSR 설정과, 진입 후 C++에서
// syscall 번호로 분기하는 부분만 다룬다.
//
// 이 milestone은 syscall 번호를 딱 하나(MC_SYSCALL_IPC_CALL)만
// 인식한다 — initrun 데모가 그 이상을 쓰지 않는다(kernel-bootstrap.md
// M8 목표: "IPC Call에 대한 응답을 받는다"). 알 수 없는 번호는
// invalid_handle로 취급한다(적당한 매핑이 없어 가장 가까운 기존
// ipc_error를 재사용 — 전용 syscall 에러 코드 체계는 이후 계획).
#include "syscall.hpp"

#include "futex.hpp"
#include "gdt_selectors.hpp"
#include "process_ops.hpp"
#include "signal.hpp"
#include "tss.hpp"

#include <cstdint>

#include <ipc/endpoint.hpp>
#include <sched/scheduler.hpp>

#include <klog.hpp>
#include <mc/syscall.h>

namespace {

constexpr uint32_t k_msr_efer = 0xC0000080;
constexpr uint32_t k_msr_star = 0xC0000081;
constexpr uint32_t k_msr_lstar = 0xC0000082;
constexpr uint32_t k_msr_fmask = 0xC0000084;
constexpr uint32_t k_msr_kernel_gs_base = 0xC0000102;  // M34(ADR-185) — swapgs 대상.
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

namespace kern::arch::x86_64 {

void install_syscall_entry(uint64_t kernel_gs_base_slot) {
    wrmsr(k_msr_efer, rdmsr(k_msr_efer) | k_efer_sce);

    // M34(real-libc-syscall-layer.md §M34, ADR-185) — 이 코어 전용
    // g_syscall_kernel_rsp[] 슬롯의 주소를 심어 둔다. syscall_entry
    // 진입 시 swapgs가 이 값을 GS_BASE로 끌어오므로, 그 뒤 `%gs:0`은
    // 항상 "지금 이 코어"의 슬롯을 정확히 가리킨다 — 여러 코어가
    // 동시에 SYSCALL로 들어와도 서로의 슬롯을 절대 건드리지 않는다
    // (M21~M33까지는 코어가 BSP 하나뿐이라 이 구분이 필요 없었다).
    wrmsr(k_msr_kernel_gs_base, kernel_gs_base_slot);

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

}  // namespace kern::arch::x86_64

// saved_regs(M12, ADR-142) — syscall_entry.S가 %r8로 넘긴다. push 역순
// 배열: [0]=r15,[1]=r14,[2]=r13,[3]=r12,[4]=rbp,[5]=rbx,[6]=rflags,
// [7]=rip,[8]=user_rsp(syscall_entry.S 상단 주석과 정확히 대응). fork
// 외의 대부분 syscall은 이 값을 읽기만 한다 — M36(real-libc-syscall-layer.md
// §M36)의 MC_SYSCALL_RT_SIGRETURN만 예외로 이 배열에 **쓴다**(시그널
// 프레임에서 원래 실행 상태를 복원) — 그래서 포인터가 이제 non-const다.
extern "C" uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t* saved_regs) {
    switch (num) {
        case MC_SYSCALL_IPC_CALL: {
            kern::object::thread* self = kern::sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(kern::ipc::ipc_error::invalid_handle);
            }
            const auto* msg_in = reinterpret_cast<const kern::ipc::message*>(a2);
            auto* msg_out = reinterpret_cast<kern::ipc::message*>(a3);
            if (msg_in == nullptr || msg_out == nullptr) {
                return static_cast<uint64_t>(kern::ipc::ipc_error::invalid_handle);
            }
            auto result =
                kern::ipc::sys_call(*self->handles, static_cast<kern::object::handle>(a1), *msg_in, *msg_out);
            return static_cast<uint64_t>(result.is_ok() ? kern::ipc::ipc_error::ok : result.error());
        }
        case MC_SYSCALL_IPC_RECV: {
            kern::object::thread* self = kern::sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(kern::ipc::ipc_error::invalid_handle);
            }
            auto* msg_out = reinterpret_cast<kern::ipc::message*>(a2);
            if (msg_out == nullptr) {
                return static_cast<uint64_t>(kern::ipc::ipc_error::invalid_handle);
            }
            auto result =
                kern::ipc::sys_recv(*self->handles, static_cast<kern::object::handle>(a1), *msg_out);
            return static_cast<uint64_t>(result.is_ok() ? kern::ipc::ipc_error::ok : result.error());
        }
        case MC_SYSCALL_IPC_REPLY: {
            kern::object::thread* self = kern::sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(kern::ipc::ipc_error::invalid_handle);
            }
            const auto* msg_in = reinterpret_cast<const kern::ipc::message*>(a1);
            if (msg_in == nullptr) {
                return static_cast<uint64_t>(kern::ipc::ipc_error::invalid_handle);
            }
            auto result = kern::ipc::sys_reply(*self->handles, *msg_in);
            return static_cast<uint64_t>(result.is_ok() ? kern::ipc::ipc_error::ok : result.error());
        }
        case MC_SYSCALL_PROCESS_SPAWN: {
            auto* req = reinterpret_cast<mc_process_spawn_request*>(a1);
            if (req == nullptr || req->elf_data == 0) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            if (req->inherited_handle_count > MC_MAX_SPAWN_INHERITED_HANDLES) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            auto err = kern::arch::x86_64::process_spawn(
                reinterpret_cast<const uint8_t*>(req->elf_data), req->elf_size,
                reinterpret_cast<const uint8_t*>(req->argv_blob), req->argv_size,
                req->grant_trusted, req->create_endpoint, req->inherited_handles,
                req->inherited_handle_count, req->out_endpoint_proxy_handle,
                req->out_thread_handle, req->linux_abi_stack != 0,
                reinterpret_cast<const uint8_t*>(req->interp_data), req->interp_size);
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_FORK: {
            // M36(real-libc-syscall-layer.md §M36) — a1(이전에는 안 쓰던
            // 인자)은 이제 "새 자식 thread 핸들을 여기 채워 달라"는
            // 유저 가상주소 출력 슬롯이다(0이면 안 채운다 — 이 슬롯이
            // 필요 없는 호출자를 위한 하위호환). mc_fork()(libmc)가
            // 항상 채워서 부른다.
            uint32_t out_thread_handle = 0;
            auto ret = kern::arch::x86_64::fork_current(
                /*rip=*/saved_regs[7], /*rflags=*/saved_regs[6],
                /*user_rsp=*/saved_regs[8], /*rbx=*/saved_regs[5],
                /*rbp=*/saved_regs[4], /*r12=*/saved_regs[3],
                /*r13=*/saved_regs[2], /*r14=*/saved_regs[1],
                /*r15=*/saved_regs[0], out_thread_handle);
            if (a1 != 0) {
                *reinterpret_cast<uint32_t*>(a1) = out_thread_handle;
            }
            return ret;
        }
        case MC_SYSCALL_EXEC: {
            const auto* req = reinterpret_cast<const mc_exec_request*>(a1);
            if (req == nullptr || req->elf_data == 0) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            // 성공하면 이 호출은 반환하지 않는다(process_ops.hpp 참고) —
            // 실패했을 때만 아래로 떨어진다.
            auto err = kern::arch::x86_64::exec_current(
                reinterpret_cast<const uint8_t*>(req->elf_data), req->elf_size,
                reinterpret_cast<const uint8_t*>(req->argv_blob), req->argv_size,
                req->linux_abi_stack != 0);
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_THREAD_EXIT: {
            // M37(real-libc-syscall-layer.md §M37, ADR-187) —
            // CLONE_CHILD_CLEARTID 흉내(thread_create.hpp 주석 참고).
            // 이 스레드가 아직 owner_space 페이지테이블이 살아있는
            // 지금(kern::sched::exit()가 CR3를 다른 스레드로 넘기기
            // 전) 유저 가상주소에 0을 쓰고 깨운다 — musl의
            // pthread_join()이 이 신호를 기다린다.
            kern::object::thread* self = kern::sched::current();
            if (self != nullptr && self->clear_child_tid_uaddr != 0) {
                *reinterpret_cast<uint32_t*>(self->clear_child_tid_uaddr) = 0;
                kern::arch::x86_64::futex_wake(self->clear_child_tid_uaddr, 1);
            }
            kern::sched::exit();  // noreturn.
        }
        case MC_SYSCALL_ALLOC_DMA_BUFFER: {
            auto* out = reinterpret_cast<mc_dma_buffer_result*>(a1);
            if (out == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            uint64_t virt = 0;
            uint64_t phys = 0;
            auto err = kern::arch::x86_64::alloc_dma_buffer(static_cast<uint32_t>(a2), virt, phys);
            if (err == kern::arch::x86_64::process_spawn_error::ok) {
                out->virt_addr = virt;
                out->phys_addr = phys;
            }
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_DEBUG_LOG: {
            const auto* str = reinterpret_cast<const char*>(a1);
            if (str == nullptr) {
                return 1;
            }
            uint64_t len = a2;
            if (len > MC_MAX_DEBUG_LOG_BYTES) {
                len = MC_MAX_DEBUG_LOG_BYTES;
            }
            char buf[MC_MAX_DEBUG_LOG_BYTES + 1];
            for (uint64_t i = 0; i < len; ++i) {
                buf[i] = str[i];
            }
            buf[len] = '\0';
            kern::klog::printf("%s", buf);
            return 0;
        }
        case MC_SYSCALL_MAP_PHYS: {
            auto* req = reinterpret_cast<mc_map_phys_request*>(a1);
            if (req == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            uint64_t virt = 0;
            auto err = kern::arch::x86_64::map_phys(req->phys_addr, req->size, virt);
            if (err == kern::arch::x86_64::process_spawn_error::ok) {
                req->out_virt_addr = virt;
            }
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_IO_ACTIVATE: {
            auto err = kern::arch::x86_64::io_activate(static_cast<uint16_t>(a1), static_cast<uint16_t>(a2));
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_IO_DEACTIVATE: {
            auto err = kern::arch::x86_64::io_deactivate();
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_PROCESS_KILL: {
            kern::object::thread* self = kern::sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_kill_error::invalid_handle);
            }
            auto err = kern::arch::x86_64::process_kill(*self->handles, static_cast<uint32_t>(a1));
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_BRK: {
            auto* req = reinterpret_cast<mc_brk_request*>(a1);
            if (req == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            auto err = kern::arch::x86_64::brk(req->increment, req->out_old_top);
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_ARCH_PRCTL_SET_FS: {
            // M28(real-libc-syscall-layer.md §M28) — mc/syscall.h의
            // mc_arch_prctl_set_fs 주석 참고. 즉시 이 스레드의
            // fs_base를 기록하고 지금 당장 MSR에도 반영한다(다음
            // 컨텍스트 스위치에서 tss.hpp::sync_fs_base가 다시
            // 덮어쓰겠지만, 이 스레드가 SYSRET로 곧바로 유저모드로
            // 돌아가는 동안에도 유효해야 한다 — 컨텍스트 스위치 없이
            // 같은 스레드가 곧바로 TLS를 쓰는 경우).
            kern::object::thread* self = kern::sched::current();
            if (self == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            self->fs_base = a1;
            kern::arch::x86_64::sync_fs_base(*self);
            return 0;
        }
        case MC_SYSCALL_MMAP_ANON: {
            uint64_t out_vaddr = 0;
            auto err = kern::arch::x86_64::mmap_anon(a1, out_vaddr);
            return err == kern::arch::x86_64::process_spawn_error::ok ? out_vaddr : 0;
        }
        case MC_SYSCALL_MUNMAP: {
            auto err = kern::arch::x86_64::munmap_anon(a1, a2);
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_PROCSRV_PID_GET: {
            // M32(real-libc-syscall-layer.md §M32) — mc/syscall.h의
            // mc_procsrv_pid_get 주석, kernel_objects.hpp::thread::
            // procsrv_pid 주석 참고.
            kern::object::thread* self = kern::sched::current();
            return self == nullptr ? 0 : self->procsrv_pid;
        }
        case MC_SYSCALL_PROCSRV_PID_SET: {
            kern::object::thread* self = kern::sched::current();
            if (self == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            self->procsrv_pid = static_cast<uint32_t>(a1);
            return 0;
        }
        case MC_SYSCALL_SIGNAL_ACTION: {
            kern::object::thread* self = kern::sched::current();
            auto* req = reinterpret_cast<mc_signal_action_request*>(a2);
            if (self == nullptr || self->handles == nullptr || req == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            kern::arch::x86_64::signal_action(*self, static_cast<uint32_t>(a1), *req);
            return 0;
        }
        case MC_SYSCALL_SIGNAL_SEND: {
            kern::object::thread* self = kern::sched::current();
            if (self == nullptr || self->handles == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::signal_send_error::invalid_handle);
            }
            auto err = kern::arch::x86_64::signal_send(*self->handles, static_cast<uint32_t>(a1),
                                                        static_cast<uint32_t>(a2));
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_YIELD: {
            // M36(real-libc-syscall-layer.md §M36) — mc/syscall.h::
            // MC_SYSCALL_YIELD 주석 참고. kern::sched::yield()는 이미
            // 타이머 틱 핸들러가 임의의 "현재 스레드" 컨텍스트에서
            // 부르는 것과 완전히 같은 함수라, 여기서 유저 syscall
            // 컨텍스트로부터 직접 불러도 안전하다(그 자리에서 다른
            // runnable 스레드로 전환했다가, 이 스레드가 다시 뽑히면
            // 바로 이 지점으로 돌아온다).
            kern::sched::yield();
            return 0;
        }
        case MC_SYSCALL_THREAD_CREATE: {
            auto* req = reinterpret_cast<mc_thread_create_request*>(a1);
            if (req == nullptr) {
                return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
            }
            uint32_t out_id = 0;
            auto err = kern::arch::x86_64::thread_create(req->entry_rip, req->user_rsp, req->arg0,
                                                           req->tls_fs_base, req->clear_child_tid_uaddr,
                                                           out_id);
            if (err == kern::arch::x86_64::process_spawn_error::ok) {
                req->out_new_thread_id = out_id;
            }
            return static_cast<uint64_t>(err);
        }
        case MC_SYSCALL_FUTEX: {
            if (a2 == MC_FUTEX_OP_WAIT) {
                auto err = kern::arch::x86_64::futex_wait(a1, static_cast<uint32_t>(a3));
                return static_cast<uint64_t>(err);
            }
            if (a2 == MC_FUTEX_OP_WAKE) {
                return kern::arch::x86_64::futex_wake(a1, static_cast<uint32_t>(a3));
            }
            return static_cast<uint64_t>(kern::arch::x86_64::process_spawn_error::invalid_argument);
        }
        case MC_SYSCALL_RT_SIGRETURN: {
            // M36(real-libc-syscall-layer.md §M36) — signal.cpp::mc_signal_return()
            // 가 saved_regs를 그 자리에서 다시 쓴다(원래 실행 상태로
            // 복원) — syscall_entry.S의 post-dispatch 훅(check_signal_delivery)
            // 이 이 rewrite 이후에도 이어서 도는 것은 의도적이다(또 다른
            // 시그널이 대기 중이면 그것도 이 시점에 전달될 수 있다).
            return mc_signal_return(saved_regs);
        }
        default:
            return static_cast<uint64_t>(kern::ipc::ipc_error::invalid_handle);
    }
}
