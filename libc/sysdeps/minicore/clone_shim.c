// libc/sysdeps/minicore/clone_shim.c — musl의 __clone(hidden, x86_64
// 원본은 third_party/musl/src/thread/x86_64/clone.s) 대체
// (real-libc-syscall-layer.md §M37, ADR-187). 원본은 raw SYSCALL
// 명령을 직접 써서(진짜 Linux ABI, 번호=RAX) clone(2)을 호출하고,
// 자식 쪽 분기(반환값 0)를 "syscall 명령 바로 다음 지점에서 이어
// 실행"하는 방식으로 만든다 — M36의 __restore_rt와 완전히 같은 이유로
// (signal.hpp 상단 주석 참고) 이 커널의 RDI 기반 syscall ABI와 맞지
// 않는다.
//
// 이 커널에서는 그 트릭이 애초에 필요 없다 — sys_thread_create가
// 만드는 새 스레드는 처음부터 entry_rip(arg0)로 곧바로 진입한다
// (kern::sched::create_user_thread, initrun/procsrv 스폰과 완전히
// 같은 모양). 그래서 이 함수는 진짜 clone(2)의 "자식이 부모와 같은
// 명령어 스트림을 이어 간다"는 의미론을 재현할 필요가 없고, 그냥
// (func, stack, arg, tls, ctid)를 mc_thread_create_request로 포장해
// syscall 한 번 부르는 평범한 C 함수로 충분하다 — flags/ptid는
// CLONE_VM|CLONE_THREAD 류(항상 그렇게 동작)/CLONE_PARENT_SETTID
// (아래에서 직접 채운다) 둘 다 이 함수 안에서 이미 결정돼 있어 그대로
// 받되 특별히 검사하지 않는다.
//
// musl의 pthread_create.c가 넘기는 인자 순서(제일 위 선언대로):
// func, stack, flags, arg, ptid, tls, ctid. 이 프로젝트가 실제로
// 링크한 musl 소스 중 __clone을 부르는 곳은 pthread_create.c 하나뿐
// (2026-09-10 확인) — 그래서 variadic이 아니라 이 정확한 7개 고정
// 인자로 선언해도 ABI가 그대로 맞는다(SysV에서 처음 6개 정수/포인터
// 인자는 variadic이든 아니든 레지스터 배치가 같다).
#include <stdint.h>

#include <mc/syscall.h>

int __clone(int (*func)(void*), void* stack, int flags, void* arg, int* ptid, void* tls,
            int* ctid) {
    (void)flags;  // CLONE_VM|CLONE_FS|CLONE_THREAD 등 — sys_thread_create는 항상 이 의미로 동작한다.

    // 진짜 x86_64 clone.s(third_party/musl/src/thread/x86_64/clone.s)
    // 도 incoming stack을 그대로 쓰지 않는다 — `and $-16,%rsi; sub
    // $8,%rsi`로 16바이트 경계 아래로 내린 뒤 8을 빼, "누군가 call한
    // 것처럼" RSP mod 16 == 8을 만든다(SysV 관례 — 진짜로는 그 자리에
    // arg도 pop용으로 미리 써 두지만, 이 커널은 arg를 RDI로 직접
    // 넘기므로 그 부분은 필요 없다, 아래 mc_thread_create 호출의
    // arg0 참고). func(=start/start_c11, musl 소스)가 정상적인 SysV
    // 함수 진입 정렬을 기대하므로 이 계산을 빼면 그 함수 내부의 정렬
    // 요구 지역변수(예: SSE 스필)에서 잘못될 수 있다.
    uint64_t aligned_rsp = ((uint64_t)(uintptr_t)stack & ~0xFULL) - 8;

    uint64_t new_id = 0;
    uint64_t ret = mc_thread_create((uint64_t)(uintptr_t)func, aligned_rsp,
                                     (uint64_t)(uintptr_t)arg, (uint64_t)(uintptr_t)tls,
                                     (uint64_t)(uintptr_t)ctid, &new_id);
    if (ret != 0) {
        return -1;  // process_spawn_error(0이 아닌 값) — 호출자(pthread_create.c)는 ret<0만 확인한다.
    }
    if (ptid != 0) {
        // CLONE_PARENT_SETTID 흉내 — 진짜 커널 tid는 아니지만(위
        // process_ops.hpp::thread_create 주석 참고), pthread_join의
        // futex 대기 루프에는 "0이 아니면 살아있다"만 있으면 충분하다.
        *ptid = (int)new_id;
    }
    return (int)new_id;
}
