// minicore/createusertest - DC-CC83F7BE(authmgr CreateUser 권한 검사 -
// 커널 중개 방식, 설계자 답변 "(A) 커널 중개... 최종 권한 판정은
// 커널이") E2E 검증 전용 클라이언트. setuidtest/sudotest와 동일한
// out-of-tree add_subdirectory 관례 - 부팅 매니페스트에는 포함하지
// 않는다(TEMP kmain.cpp 훅으로만 자동 실행).
//
// 시나리오: (1) 이 프로세스는 init의 자손이라 uid=0(root)으로 시작 -
// root 권한으로 uid 60(parentUid=0)을 생성(항상 허용). (2) Setuid(60)
// 으로 60으로 전환(root의 특권). (3) uid=60인 채로 uid 61(parentUid=60,
// 자기 직계 자식)을 생성 - 허용돼야 한다. (4) uid=60인 채로 uid 62
// (parentUid=0=root)를 생성 시도 - 60은 root의 조상이 아니므로
// PermissionDenied로 거부돼야 한다(권한 상승 회귀 검사 - 이게 성공하면
// 심각한 버그). (5) uid=60인 채로 uid 63(parentUid=61)을 생성 - 61은
// 60의 직계 자식이고 60은 61의 조상이므로, 손자를 만드는 이 요청도
// 허용돼야 한다(조상 체인 전체 판정 확인, 직계 부모 판정만이 아님).
//
// exitCode: 0=전 구간 성공, 1/2=CreateUser(60) 단계, 3/4=Setuid(60)
// 단계, 5/6=CreateUser(61) 단계, 7/8=CreateUser(62) 단계(8=권한 상승
// 버그), 9/10=CreateUser(63) 단계.
#include "libmc/process.h"
#include "libmc/syscall.h"

namespace {

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

mc::ChannelError kCreateUser(mc::uint32_t uid, mc::uint32_t parentUid, mc::int32_t submitFailCode) {
    mc::CreateUserArgs args;
    args.uid = uid;
    args.parentUid = parentUid;
    args.gid = uid;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointCreateUser, &args);
    if (token == 0 || !mc::wait(token)) {
        kFinish(submitFailCode);
    }
    return args.error;
}

}  // namespace

extern "C" void _start() {
    // (1) root로 uid 60(parentUid=root) 생성 - 항상 허용.
    if (kCreateUser(60, 0, 1) != mc::ChannelError::None) {
        kFinish(2);
    }

    // (2) Setuid(60) - root의 특권으로 즉시 성공해야 한다.
    mc::SetuidArgs setuidArgs;
    setuidArgs.targetUid = 60;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSetuid, &setuidArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(3);
    }
    if (setuidArgs.error != mc::ChannelError::None) {
        kFinish(4);
    }

    // (3) uid=60인 채로 uid 61(parentUid=60, 직계 자식) 생성 - 허용.
    if (kCreateUser(61, 60, 5) != mc::ChannelError::None) {
        kFinish(6);
    }

    // (4) uid=60인 채로 uid 62(parentUid=0=root) 생성 시도 - 60은
    // root의 조상이 아니므로 PermissionDenied여야 한다(권한 상승
    // 회귀 검사 - 이게 성공하면 심각한 버그).
    if (kCreateUser(62, 0, 7) != mc::ChannelError::PermissionDenied) {
        kFinish(8);
    }

    // (5) uid=60인 채로 uid 63(parentUid=61) 생성 - 61은 60의 직계
    // 자식이고 60은 61의 조상이므로 허용돼야 한다(직계 부모 판정이
    // 아니라 조상 체인 전체 판정임을 확인).
    if (kCreateUser(63, 61, 9) != mc::ChannelError::None) {
        kFinish(10);
    }

    kFinish(0);
}
