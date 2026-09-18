#ifndef MINICORE_KERNEL_USER_SYNC_H
#define MINICORE_KERNEL_USER_SYNC_H

#include "channel.h"
#include "mutex_core.h"
#include "process.h"
#include "semaphore_core.h"
#include "syscall.h"

namespace kernel {

// [SP-0666DB3C §17, PN-E82744B1] 그룹 8(Sync, RM-48E1E610) - 유저
// syscall Mutex/Semaphore 노출. §17.1이 확정한 대로 항상
// AsyncMutex/AsyncSemaphore(YieldingPolicy)만 쓴다 - syscall 핸들러가
// 코어 공유 리액터 Task 위에서 실행되므로 Mutex/Semaphore(ParkingPolicy)
// 를 쓰면 그 코어 전체가 멈춘다.
constexpr SyscallEndpointId kSyscallEndpointMutexCreate = kMakeSyscallEndpointId(8, 0);
constexpr SyscallEndpointId kSyscallEndpointMutexDestroy = kMakeSyscallEndpointId(8, 1);
constexpr SyscallEndpointId kSyscallEndpointMutexLock = kMakeSyscallEndpointId(8, 2);
constexpr SyscallEndpointId kSyscallEndpointMutexUnlock = kMakeSyscallEndpointId(8, 3);
constexpr SyscallEndpointId kSyscallEndpointSemaphoreCreate = kMakeSyscallEndpointId(8, 4);
constexpr SyscallEndpointId kSyscallEndpointSemaphoreDestroy = kMakeSyscallEndpointId(8, 5);
constexpr SyscallEndpointId kSyscallEndpointSemaphoreWait = kMakeSyscallEndpointId(8, 6);
constexpr SyscallEndpointId kSyscallEndpointSemaphorePost = kMakeSyscallEndpointId(8, 7);

// [정정, SP-9CB55C5B/SP-CA3C3E57가 이미 두 번 확정한 패턴 재사용,
// PN-CE6A04AB 여파] §17.2 원안의 "핸들 = 포인터값"은 Channel이 이미
// 겪은 취약점과 같은 모양이라 채택하지 않는다 - 대신 세대 태그 슬롯
// 테이블(user_sync.cpp) + 안전한 resolve 함수로만 실제 포인터에
// 접근한다(Channel의 kResolveChannelId()/Process의
// kResolveProcessId()와 동일한 관례, 이 값 자체를 직접
// reinterpret_cast하지 않는다).
using MutexHandle = uint64_t;
using SemaphoreHandle = uint64_t;

// [SP-0666DB3C §17.2/§17.4] Mutex는 개념상 소유자가 있는 타입이므로
// (설계자 답변 "커널 내부에서는 소유자가 있는 모든 타입에서 검증을
// 해야돼") MutexUnlock이 소유자를 검증해야 한다 - `AsyncMutex` 코어
// 자체(§13, 재진입 정책과의 오버헤드 분리를 위해 의도적으로
// 비재진입 경로엔 소유자 추적을 안 붙임)는 그대로 두고, 이 syscall
// 레벨 래퍼가 마지막 lock 성공자의 ProcessId를 별도로 기억한다.
struct UserMutex {
    AsyncMutex core;
    ProcessId owner = kInvalidProcessId;  // lock 보유 중이 아니면 무효값
};

// 생성 - 블로킹 없음(즉시 완료), 소유자는 없음(핸들을 아는 모든
// 프로세스가 동등하게 lock/unlock 가능 - Channel의 "이름 아는 사람은
// 누구나 connect 가능"과 같은 신뢰 모델, 별도 ACL 없음). [정정,
// PN-E82744B1] 원 설계 스니펫은 `error` 필드를 생략했으나, 슬랩/핸들
// 테이블 고갈(ResourceExhausted) 실패를 보고할 길이 필요해 이 코드베이스
// 다른 모든 Create류 syscall과 동일하게 추가한다(순수 구현 세부).
struct MutexCreateArgs {
    MutexHandle handle = 0;
    ChannelError error = ChannelError::None;
};
struct MutexDestroyArgs {
    MutexHandle handle = 0;
    ChannelError error = ChannelError::None;  // NotFound
};

// lock/unlock - submit/wait 분리(SP-04EE2A18) 그대로. MutexLock은
// UserMutex::core.lock()을 그 syscall의 AsyncTask::onExec 안에서
// 직접 호출한다 - 경합이 없으면 즉시 완료, 있으면 §17.1대로
// AsyncTask::yield()로 리액터에 양보했다가 재개된다(waitForSyscall로
// 관찰). lock 성공 직후 onExec이 handle->owner = callingProcessId를
// 기록한다.
struct MutexLockArgs {
    MutexHandle handle = 0;
    ChannelError error = ChannelError::None;  // NotFound
};
// [확정, SP-0666DB3C §17.4] Unlock은 반드시 owner를 검증한다 - onExec이
// owner != callingProcessId면 core.unlock()을 아예 호출하지 않고
// NotOwner로 즉시 실패(락 상태 불변 유지). 검증 통과 시 owner를 먼저
// kInvalidProcessId로 리셋한 뒤 core.unlock()을 호출한다. **[순서
// 정정, PN-E82744B1]** 원 설계 스니펫은 "release() 호출 후 owner
// 리셋" 순서였으나, 그 순서는 unlock() 성공 직후~owner 리셋 사이에
// 다른 대기자가 lock()에 성공해 owner를 자기 값으로 채운 뒤 이
// 스레드가 뒤늦게 kInvalidProcessId로 덮어써 그 새 소유자 기록을
// 지워버리는 경쟁을 만든다 - "owner 리셋 먼저, unlock 나중"이 유일한
// 안전한 순서다(리셋 시점엔 아직 락이 안 풀려 있어 다른 대기자가
// owner를 건드릴 수 없음).
struct MutexUnlockArgs {
    MutexHandle handle = 0;
    ChannelError error = ChannelError::None;  // NotFound / NotOwner
};

// Semaphore는 §16.2가 이미 확정한 대로 소유자 개념 자체가 성립하지
// 않는다(복수의 서로 다른 소유자가 동시에 보유하는 게 정상 동작) -
// 검증 없이 그대로 유지. `AsyncSemaphore*` 값을 세대 태그 테이블에
// 등록해 핸들로 노출한다(UserMutex 같은 별도 래퍼 구조체 불필요 -
// 소유자를 안 담으므로).
struct SemaphoreCreateArgs {
    uint32_t initialCount = 0;
    SemaphoreHandle handle = 0;
    ChannelError error = ChannelError::None;
};
struct SemaphoreDestroyArgs {
    SemaphoreHandle handle = 0;
    ChannelError error = ChannelError::None;
};
struct SemaphoreWaitArgs {  // acquire, 카운트 0이면 yield
    SemaphoreHandle handle = 0;
    ChannelError error = ChannelError::None;
};
struct SemaphorePostArgs {  // release
    SemaphoreHandle handle = 0;
    ChannelError error = ChannelError::None;
};

// 취소 처리(프로세스 종료 시 lock/wait 대기 중이던 syscall)는
// SP-04EE2A18 "종료 시 취소 처리"가 이미 다루는 일반 메커니즘을
// 그대로 쓴다 - 전용 특수 취소 로직 없음. lock을 보유한 채로 죽으면
// 그 Mutex는 영원히 잠긴 채로 남는다(robust mutex 아님, v1이 명시적
// 으로 허용하는 실패 모드 - §17.3 원 설계 그대로).
class UserSyncService {
public:
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_USER_SYNC_H
