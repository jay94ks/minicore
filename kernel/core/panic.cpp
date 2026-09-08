// libk_detail::panic_hook 커널 쪽 정의 (ADR-067) — libk가 "일어나서는
// 안 되는" 상태(cxx-conventions.md §3)에서 호출한다. klog로 로그를
// 남긴 뒤 정지 루프로 멈춘다.
//
// arch 헤더를 include하지 않는다(ADR-002 HAL 경계) — "인터럽트 대기
// 정지" 명령은 모든 지원 아키텍처가 갖고 있어(x86_64 hlt, aarch64
// wfi), 컴파일러가 미리 정의하는 __x86_64__/__aarch64__ 매크로로
// 분기하는 정도는 libk의 cpu_relax()(spinlock.hpp)와 같은 수준의
// 예외로 취급한다.
#include "klog.hpp"

#include <cstdint>

#include <libk/panic.hpp>

namespace libk_detail {

namespace {

// ADR-126: 프레임포인터(rbp/x29) 체인을 걸어 return address만 raw hex로
// 찍는다. 함수명 해석은 커널 안에서 하지 않는다 — 커널 이미지에 심볼
// 테이블을 심는 건 별도 ADR 대상(지금은 llvm-addr2line -e <kernel.elf>
// <addr>로 사후 변환하는 워크플로를 전제한다, docs/spec/debug-console.md
// 참고). 두 아키텍처 모두 프레임 레코드 레이아웃이 [fp+0]=saved fp,
// [fp+8]=return address로 같아 arch 분기 없이 동작한다.
constexpr int k_max_backtrace_frames = 16;

void print_backtrace() {
    auto* fp = reinterpret_cast<uint64_t*>(__builtin_frame_address(0));

    klog::printf("[PANIC] backtrace:\n");
    for (int i = 0; i < k_max_backtrace_frames; ++i) {
        // fp가 NULL이거나 정렬이 안 맞으면(스택 손상) 더 걷지 않는다 —
        // 프레임포인터 체인은 컴파일러가 무결성을 보증하는 구조가
        // 아니므로, 손상된 값을 따라가다 폴트나는 것보다는 여기서
        // 멈추는 쪽이 안전하다.
        if (fp == nullptr || (reinterpret_cast<uint64_t>(fp) & 0x7) != 0) {
            break;
        }
        uint64_t saved_fp = fp[0];
        uint64_t ret_addr = fp[1];
        if (ret_addr == 0) {
            break;
        }
        klog::printf("  #%d 0x%lx\n", i, static_cast<unsigned long>(ret_addr));

        // 스택은 아래로 자라므로 다음 프레임은 항상 더 높은 주소여야
        // 한다 — 아니면 체인이 손상됐거나 순환하는 것이니 멈춘다.
        if (saved_fp <= reinterpret_cast<uint64_t>(fp)) {
            break;
        }
        fp = reinterpret_cast<uint64_t*>(saved_fp);
    }
}

}  // namespace

[[noreturn]] void panic_hook(const char* file, int line, const char* msg) {
    klog::printf("[PANIC] %s:%d: %s\n", file, line, msg);
    print_backtrace();

    for (;;) {
#if defined(__x86_64__)
        asm volatile("cli; hlt");
#elif defined(__aarch64__)
        asm volatile("wfi");
#endif
    }
}

}  // namespace libk_detail
