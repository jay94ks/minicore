// init/initrun/main.cpp — 커널이 유저모드로 띄우는 최초 프로세스의 최소
// 데모 바이너리 (docs/plan/kernel-bootstrap.md M8, docs/spec/boot.md §4/§6).
//
// 이 마일스톤에는 콘솔·파일시스템 등 유저모드 I/O 경로가 전혀 없다 —
// initrun의 유일한 "출력"은 커널에 보내는 IPC Call 그 자체다. 커널
// 쪽 수신 스레드(kernel_main.cpp의 thread_initrun_boot_server_entry)가
// 이 호출을 받으면 klog로 "부팅 성공"을 기록한다 — 관찰은 커널 시리얼
// 로그를 통해서만 가능하다. RDI로 boot_info 포인터를 규약대로(ADR-030/
// boot.md §6) 받지만, 이 데모는 아직 그 내용을 읽지 않는다(procsrv 등
// 실제로 부팅 정보가 필요한 이후 서버가 등장할 때 의미가 생긴다).
#include <uapi.hpp>

namespace {

// 커널이 이 프로세스의 handle_table을 만들 때 이 IPC 호출용 endpoint
// 프록시 핸들을 항상 가장 먼저(그리고 유일하게) 만들어 넣는다는 M8
// 한정 약속(kernel_main.cpp의 setup_initrun_process 참고) — 진짜
// 프로세스 환경(procsrv 이후)이라면 이런 고정 번호 대신 boot_info나
// 시작 인자로 전달해야 한다. handle 0은 항상 무효(k_invalid_handle)로
// 정해져 있으므로(objects.md) 테이블의 첫 핸들은 반드시 1이다.
constexpr uint32_t k_boot_endpoint_handle = 1;
constexpr uint32_t k_boot_label = 0xB007;

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

}  // namespace

extern "C" [[noreturn]] void _start(const void* /*boot_info*/) {
    uapi::message out{};
    out.label = k_boot_label;

    uapi::message in{};
    do_syscall(uapi::k_syscall_ipc_call, k_boot_endpoint_handle,
               reinterpret_cast<uint64_t>(&out), reinterpret_cast<uint64_t>(&in));

    // ring3에서는 hlt(특권 명령)를 쓸 수 없다 — 대신 pause로 조용히
    // 도는다. M1~M8 어디에도 IDT가 없어 예외가 나면 그대로 트리플
    // 폴트한다(hlt를 잘못 썼다면 바로 그렇게 됐을 것이다) — pause는
    // 특권이 필요 없는 명령이라 안전하다.
    for (;;) {
        asm volatile("pause");
    }
}
