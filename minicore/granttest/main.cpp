// minicore/granttest - DC-2B22FBF0(authmgr GrantSudoPermission 권한
// 검사 - 커널 중개 방식, 설계자 답변 "(A-2) 조상-자손 규칙 재사용")
// E2E 검증 전용 클라이언트. createusertest와 동일한 out-of-tree
// add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
// kmain.cpp 훅으로만 자동 실행).
//
// 시나리오: (1) root로 uid 80(parentUid=0)과 uid 83(parentUid=0, 80과
// 무관한 형제)을 생성. (2) root인 채로 GrantSudoPermission(targetUid=83)
// - root는 항상 허용돼야 한다. (3) Setuid(80)으로 80으로 전환(root의
// 특권). (4) uid=80인 채로 uid 82(parentUid=80, 자기 직계 자식)를
// 생성 - 허용. (5) uid=80인 채로 GrantSudoPermission(targetUid=82) -
// 82는 80의 자손이므로 허용돼야 한다. (6) uid=80인 채로
// GrantSudoPermission(targetUid=83) 시도 - 83은 80의 자손이 아니므로
// PermissionDenied여야 한다(권한 상승 회귀 검사 - 이게 성공하면
// 심각한 버그, CreateUser의 동일한 회귀 검사와 같은 성격).
//
// exitCode: 0=전 구간 성공, 1/2=CreateUser(80), 3/4=CreateUser(83),
// 5/6=GrantSudoPermission(83, root), 7/8=Setuid(80),
// 9/10=CreateUser(82), 11/12=GrantSudoPermission(82, uid80),
// 13/14=GrantSudoPermission(83, uid80, 14=권한 상승 버그).
//
// 이 프로세스가 authmgr보다 먼저 뜰 수 있어(다른 TEMP 테스트 클라이언트
// 들과 동일한 이유), 커널의 authmgr_client.h 연결이 아직 준비 안 된
// 채로 첫 요청이 도착하면 ServiceUnavailable로 실패할 수 있다(lrutest/
// createusertest와 동일한 실측 확인 부팅 경합) - 작은 재시도 예산으로
// 흡수한다(lrutest와 동일한 패턴 - ServiceUnavailable일 때만 재시도,
// PermissionDenied 등 그 외 결과는 즉시 반환하므로 회귀 검사(83,
// uid80)의 기대값 확인에는 영향 없다).
#include "libmc/process.h"
#include "libmc/syscall.h"

namespace {

constexpr mc::uint32_t kMaxRetriesForBootRace = 20;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

mc::ChannelError kCreateUser(mc::uint32_t uid, mc::uint32_t parentUid, mc::int32_t submitFailCode) {
    mc::ChannelError lastError = mc::ChannelError::None;
    for (mc::uint32_t attempt = 0; attempt < kMaxRetriesForBootRace; ++attempt) {
        mc::CreateUserArgs args;
        args.uid = uid;
        args.parentUid = parentUid;
        args.gid = uid;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointCreateUser, &args);
        if (token == 0 || !mc::wait(token)) {
            kFinish(submitFailCode);
        }
        lastError = args.error;
        if (lastError != mc::ChannelError::ServiceUnavailable) {
            break;
        }
    }
    return lastError;
}

mc::ChannelError kGrantSudoPermission(mc::uint32_t targetUid, mc::int32_t submitFailCode) {
    mc::ChannelError lastError = mc::ChannelError::None;
    for (mc::uint32_t attempt = 0; attempt < kMaxRetriesForBootRace; ++attempt) {
        mc::GrantSudoPermissionArgs args;
        args.targetUid = targetUid;
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointGrantSudoPermission, &args);
        if (token == 0 || !mc::wait(token)) {
            kFinish(submitFailCode);
        }
        lastError = args.error;
        if (lastError != mc::ChannelError::ServiceUnavailable) {
            break;
        }
    }
    return lastError;
}

}  // namespace

extern "C" void _start() {
    // (1) root로 uid 80/83(둘 다 parentUid=root, 서로 무관한 형제) 생성.
    if (kCreateUser(80, 0, 1) != mc::ChannelError::None) {
        kFinish(2);
    }
    if (kCreateUser(83, 0, 3) != mc::ChannelError::None) {
        kFinish(4);
    }

    // (2) root인 채로 GrantSudoPermission(83) - root는 항상 허용.
    if (kGrantSudoPermission(83, 5) != mc::ChannelError::None) {
        kFinish(6);
    }

    // (3) Setuid(80) - root의 특권으로 즉시 성공해야 한다.
    mc::SetuidArgs setuidArgs;
    setuidArgs.targetUid = 80;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointSetuid, &setuidArgs);
    if (token == 0 || !mc::wait(token)) {
        kFinish(7);
    }
    if (setuidArgs.error != mc::ChannelError::None) {
        kFinish(8);
    }

    // (4) uid=80인 채로 uid 82(parentUid=80, 직계 자식) 생성 - 허용.
    if (kCreateUser(82, 80, 9) != mc::ChannelError::None) {
        kFinish(10);
    }

    // (5) uid=80인 채로 GrantSudoPermission(82) - 82는 80의 자손이므로 허용.
    if (kGrantSudoPermission(82, 11) != mc::ChannelError::None) {
        kFinish(12);
    }

    // (6) uid=80인 채로 GrantSudoPermission(83) 시도 - 83은 80의
    // 자손이 아니므로 PermissionDenied여야 한다(권한 상승 회귀 검사 -
    // 이게 성공하면 심각한 버그).
    if (kGrantSudoPermission(83, 13) != mc::ChannelError::PermissionDenied) {
        kFinish(14);
    }

    kFinish(0);
}
