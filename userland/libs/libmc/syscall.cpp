#include "libmc/syscall.h"

namespace mc {

void selfTerminate(int32_t exitCode) {
    // 실제 트랩 - ABI는 QU-E7E51931/QU-CD6F68B7로 확정(RAX=verb 0=submit,
    // RDI=endpointId, RSI=args, int 0x80). self-terminate는 커널이
    // 트랩 지점에서 이 UserThread를 즉시 끝내고 절대 ring3로 복귀시키지
    // 않는다(QU-D96B1DCE 설계자 답변 - kHandleSyscallTrap이 이 endpoint를
    // 특별 취급) - 그래서 아래 `int $0x80` 다음 줄은 정상적으로는 절대
    // 실행되지 않는다. exitCode는 args로 아직 전달하지 않는다(v1 -
    // 커널 쪽 self-terminate 핸들러가 아직 종료 코드를 안 씀, 필요해지면
    // args 포인터로 확장).
    (void)exitCode;
    const uint64_t endpointId = kSyscallEndpointSelfTerminate;
    asm volatile(
        "mov $0, %%rax\n\t"
        "mov %0, %%rdi\n\t"
        "xor %%esi, %%esi\n\t"
        "int $0x80\n\t"
        :
        : "r"(endpointId)
        : "rax", "rdi", "rsi", "memory");
    // 방어적 - 위 트랩이 정상적으로는 절대 반환하지 않으므로 도달
    // 불가하지만, 컴파일러에게 [[noreturn]] 계약을 지키려면 필요하다.
    for (;;) {
    }
}

SyscallToken submit(SyscallEndpointId endpointId, void* args) {
    // [고침, PN-BD9AAE2F devmgr end-to-end 검증 중 발견] 원래 "r"
    // 제약 두 개를 각각 별도 mov로 rdi/rsi에 옮기는 방식이었는데,
    // 컴파일러가 그 "r" 피연산자 하나를 이미 rdi(또는 rsi)에 배정해
    // 두면 두 번째 mov가 그 값을 미처 쓰기 전에 덮어써 버리는
    // 레지스터 배정 경합이 있었다(실제로 devmgr이 EnumerateDevices를
    // 제출해도 커널 핸들러의 onExec이 단 한 번도 안 불리는 것으로
    // 발견 - args 포인터가 손상돼 SyscallRegistry 조회 전에 이미
    // 깨진 값이 됐을 가능성). "D"/"S" 제약으로 컴파일러가 애초에
    // rdi/rsi에 직접 실어 주게 해 중간 mov 자체를 없앤다("+a"로
    // verb 입력과 결과 출력을 같은 rax 하나로 왕복).
    uint64_t verb = 0;  // submit
    asm volatile("int $0x80"
                 : "+a"(verb)
                 : "D"(static_cast<uint64_t>(endpointId)), "S"(reinterpret_cast<uint64_t>(args))
                 : "memory");
    return static_cast<SyscallToken>(verb);
}

bool wait(SyscallToken token) {
    // submit()과 동일한 이유로 "D" 제약 사용 - 커널 쪽은 verb=wait일 때
    // RSI를 읽지 않으므로(idt.cpp kDispatchSyscallVerb) 별도로 채우지
    // 않는다.
    uint64_t verb = 1;  // wait
    asm volatile("int $0x80" : "+a"(verb) : "D"(static_cast<uint64_t>(token)) : "memory");
    return verb != 0;
}

WaitAnyOfSyscallArgs waitAnyForMultipleSyscall(const SyscallToken* tokens, uint32_t count) {
    WaitAnyOfSyscallArgs args;
    if (count == 0 || tokens == nullptr) {
        args.resultOutcome = MultiWaitOutcome::Invalid;
        return args;
    }
    args.tokens = tokens;
    args.count = count;
    // submit()과 동일한 이유("D" 제약) - verb=2는 RDI=args 포인터 하나만
    // 쓴다(idt.cpp kSyscallVerbWaitAnyOf, RSI 불필요).
    uint64_t verb = 2;  // waitAnyOf
    asm volatile("int $0x80" : "+a"(verb) : "D"(reinterpret_cast<uint64_t>(&args)) : "memory");
    // 트랩 실패(verb==0, 예: 포인터 검증 실패)면 out 필드가 안 채워진
    // 채로 남아 있으므로 Invalid로 정리해 둔다.
    if (verb == 0) {
        args.resultOutcome = MultiWaitOutcome::Invalid;
    }
    return args;
}

void waitForMultipleSyscall(SyscallToken* tokens, uint32_t count, WaitAnyOfSyscallArgs* outResults) {
    uint32_t remaining = count;
    for (uint32_t i = 0; i < count && remaining > 0; ++i) {
        WaitAnyOfSyscallArgs result = waitAnyForMultipleSyscall(tokens, remaining);
        outResults[i] = result;
        // 완료된 토큰을 작업 배열에서 swap-remove(순서 무관 - 대기
        // 대상 "집합"일 뿐이다).
        for (uint32_t j = 0; j < remaining; ++j) {
            if (tokens[j] == result.resultToken) {
                tokens[j] = tokens[remaining - 1];
                --remaining;
                break;
            }
        }
    }
}

}  // namespace mc
