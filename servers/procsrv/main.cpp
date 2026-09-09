// servers/procsrv/main.cpp — 프로세스 서버의 M12 골격
// (docs/plan/system-servers-bringup.md §M12, docs/spec/procsrv.md).
//
// M12는 procsrv.md가 정의하는 전체 프로세스 테이블·fd 진실 공급원·
// 계정 모델을 구현하지 않는다(계획 §M12 §구현2가 이미 "프로토콜
// 골격만"으로 scope했다) — 이 마일스톤의 QEMU 검증 목표는 딱 하나,
// "procsrv가 실제 디스크(virtio-blk+cpio)에서 읽혀 sys_process_spawn
// 으로 기동된 뒤, 자기 자신을 fork/exec해 실제 두 번째 완전한 유저
// 프로세스를 만들어내는 것"이다(kernel_main.cpp가 initrun에 대해
// 이미 검증한 것과 같은 절차를 procsrv 자신에게 반복하는 것 —
// ADR-149가 그 self_info 배선을 procsrv도 받을 수 있도록 일반화해
// 둬서 가능해졌다).
//
// initrun과 다른 점 — "나는 initrun이 방금 스폰한 원본인가, 아니면
// fork/exec으로 만들어진 사본인가"를 구분할 방법이 initrun의
// boot_info(항상 비어있지 않은 포인터)와 다르다: procsrv는 일반
// sys_process_spawn으로 만들어지므로 RDI는 그냥 argv 블록 주소(또는
// 인자가 없으면 0)다. 그래서 이 규약을 쓴다 — **initrun이 procsrv를
// 처음 스폰할 때만 argv를 비워두지 않고(예: "-" 한 글자짜리 블록),
// procsrv 자신의 self-exec 호출은 항상 argv 없이(0) 한다** — initrun의
// boot_info-vs-null 구분과 정확히 같은 비대칭을 argv 유무로 재현한다.
#include <uapi.hpp>

namespace {

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

[[noreturn]] void quiet_exit() {
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace

extern "C" [[noreturn]] void _start(const void* argv_or_null) {
    if (argv_or_null == nullptr) {
        quiet_exit();  // sys_fork+sys_exec으로 만들어진 사본.
    }

    const auto* self_info =
        reinterpret_cast<const uapi::m12_self_info*>(uapi::k_m12_self_info_user_vaddr);

    uint64_t fork_ret = do_syscall(uapi::k_syscall_fork, 0, 0, 0);
    if (fork_ret == 0) {
        // 자식 — sys_exec으로 자기 자신을 다시 실행(argv 없음 = 0,
        // 위 상단 주석의 규약대로 이 경로가 다음번 _start에서 사본으로
        // 식별된다).
        uapi::exec_request exec_req{};
        exec_req.elf_data = self_info->elf_addr;
        exec_req.elf_size = self_info->elf_size;
        do_syscall(uapi::k_syscall_exec, reinterpret_cast<uint64_t>(&exec_req), 0, 0);
        quiet_exit();  // exec 실패 시에만 도달.
    }

    // 부모 — fork/exec 왕복이 성공했다는 사실 자체가 이 마일스톤의
    // 검증 목표다(커널의 process_ops.cpp가 이미 "[process] fork ok"/
    // "[process] exec ok"를 로그로 남긴다 — 어느 프로세스가 호출했는지
    // 구분하지 않는 일반 로그이므로 procsrv가 호출해도 그대로 관찰
    // 가능하다). 아직 procsrv.md의 실제 프로토콜(§2~9)은 구현하지
    // 않으므로 그 이상 할 일이 없다.
    quiet_exit();
}
