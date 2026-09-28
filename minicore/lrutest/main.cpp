// minicore/lrutest - PN-B6DB692C(사용자 신원 트리) "검증" 절이 남겨 둔
// 미검증 항목 실측 - "캐시 1024개 초과 시 LRU 축출이 올바른 항목을
// 대체하는지" 확인하려 했으나, 실측(2026-09-28) 결과 **그 코드
// 경로 자체가 현재 구조에서는 도달 불가능함을 확인했다**: authmgr의
// libkvdb Store(`gUserRecordsByUid`)도 커널 `UserRecordCache`와 정확히
// 같은 용량(1024, kvdb.h 문서 주석에 "PN-B6DB692C 기준"이라고 이미
// 명시돼 있었음)이라, 1024번째 신규 uid를 만들려는 순간 authmgr
// 자신의 저장소가 먼저 `StoreFull`로 거부한다(`ChannelError::
// ServiceUnavailable`로 커널까지 전파, 매 시도마다 일관되게 실패 -
// 일시적 경쟁이 아니라 영구적 용량 한계) - 커널 캐시가 축출을
// 고려하기도 전에 이미 "그 uid는 시스템 전체에 존재할 수 없다"로
// 끝난다. 즉 이 시스템은 지금 **root 포함 최대 1024명**이라는 전역
// 상한이 있고, 그 상한에서의 동작(정직하게 거부, 데이터 손상/false
// success 없음)이 실제 확인 대상이 됐다 - 원래 의도(LRU 축출
// 실측)는 이 상한을 authmgr 쪽이 늘리기 전까지 검증 불가능하다.
//
// setuidtest/sudotest/createusertest와 동일한 out-of-tree
// add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
// kmain.cpp 훅으로만 자동 실행).
//
// 시나리오: root로 uid 1..kMaxNewUsers(1023 = 시스템 전체 상한 1024
// 에서 root 1명을 뺀 값)를 순차 생성 - 전부 성공해야 한다. 그 다음
// uid kMaxNewUsers+1(=1024)을 생성 시도 - `ServiceUnavailable`로
// 거부돼야 한다(성공하거나 다른 에러면 회귀).
//
// exitCode: 0=전부 기대대로, 1..kMaxNewUsers=그 반복에서 예상 밖
// 실패, 9999=경계에서 예상과 다른 결과(성공하거나 다른 에러코드).
#include "libmc/process.h"
#include "libmc/syscall.h"

namespace {

// authmgr의 libkvdb Store와 커널 UserRecordCache 둘 다 용량 1024
// (root 포함) - 새로 만들 수 있는 최대 인원은 그 나머지.
constexpr mc::uint32_t kMaxNewUsers = 1023;

// 이 프로세스가 authmgr보다 먼저 뜰 수 있어(다른 기존 테스트
// 클라이언트들과 동일한 이유), 커널의 authmgr_client.h 연결이 아직
// 준비 안 된 채로 첫 CreateUser가 도착하면 ServiceUnavailable로
// 실패할 수 있다(실측 확인, 2026-09-28) - 이 경쟁은 부팅 극초반
// 한 번뿐이므로 작은 재시도 예산이면 충분하다. **경계값 테스트
// (kMaxNewUsers+1)에는 이 재시도를 적용하지 않는다** - 그 실패는
// 재시도로 없어지는 경쟁이 아니라 영구적 용량 한계이기 때문(실측
// 확인 - 2000회 재시도 전부 동일하게 실패).
constexpr mc::uint32_t kMaxRetriesForBootRace = 20;

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

mc::ChannelError kCreateUserOnce(mc::uint32_t uid) {
    mc::CreateUserArgs args;
    args.uid = uid;
    args.parentUid = 0;  // root의 직계 자식 - 이 프로세스는 root이므로 항상 허용
    args.gid = uid;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointCreateUser, &args);
    if (token == 0 || !mc::wait(token)) {
        kFinish(static_cast<mc::int32_t>(uid));
    }
    return args.error;
}

}  // namespace

extern "C" void _start() {
    for (mc::uint32_t i = 1; i <= kMaxNewUsers; ++i) {
        mc::ChannelError lastError = mc::ChannelError::None;
        bool ok = false;
        for (mc::uint32_t attempt = 0; attempt < kMaxRetriesForBootRace; ++attempt) {
            lastError = kCreateUserOnce(i);
            if (lastError == mc::ChannelError::None) {
                ok = true;
                break;
            }
            if (lastError != mc::ChannelError::ServiceUnavailable) {
                break;  // 재시도로 해결될 실패가 아님 - 즉시 포기
            }
        }
        if (!ok) {
            kFinish(static_cast<mc::int32_t>(i));
        }
    }

    // 경계값 - 시스템 전체 상한(root 포함 1024) 바로 다음 uid는
    // 항상, 확실하게, 재시도 없이도 ServiceUnavailable이어야 한다.
    if (kCreateUserOnce(kMaxNewUsers + 1) != mc::ChannelError::ServiceUnavailable) {
        kFinish(9999);
    }

    kFinish(0);
}
