#ifndef MINICORE_KERNEL_SYSCALL_H
#define MINICORE_KERNEL_SYSCALL_H

#include "async_task.h"
#include "libkenv/chunked_list.h"
#include "libkenv/types.h"
#include "task.h"

namespace kernel {

class Process;  // 포인터로만 참조(UserThread::process) - 전체 정의는 process.h

// 공개 ABI로 노출되는 syscall 번호 - 커널이 부팅 시 고정 배정한다
// (동적 재배정 없음, SP-04EE2A18). AsyncCallbackRegistry가 내부적으로
// 동적 배정하는 subjectCode와는 별개의 이름 공간이다(아래
// SyscallRegistry 참고).
using SyscallEndpointId = uint32_t;

// User-Level로 격하된 Task가 자연 종료(kTaskFallingToEnd)될 때 자기
// 자신을 종료 처리해 달라고 제출하는 예약 endpoint(PL-2D3184BC "Task
// 종료 프로토콜", QU-26F9420E 설계자 답변 1번, 2026-09-14 - "자기
// 자신을 종료처리하라는 System Call 명세를 별도로 만들고 그걸로
// 제출"). **아직 이 endpoint에 등록된 핸들러가 없다**(프로세스
// 모델/ring3 데모션 메커니즘 자체가 아직 없어 이 경로가 실제로
// 트리거될 수 없음) - 그 인프라가 생길 때 실제 정리 로직(프로세스
// 자원 회수, 부모에게 종료 통지 등)을 이 endpoint의 핸들러로 등록
// 하면 된다. 그 전까지 submit()은 항상 미등록으로 실패하지만
// (SyscallRegistry::resolveSubjectCode가 false), 호출부(scheduler.cpp
// 의 kTaskOnFallingToEnd)는 그 결과를 wait하지 않으므로 무해하다.
constexpr SyscallEndpointId kSyscallEndpointSelfTerminate = 0;

// 유저 프로세스에 속한 스레드의 커널 쪽 표현(SP-04EE2A18, 설계자 지시
// 2026-09-14 - "커널 Task와 쓰레드는 다른 개념이다... 내부적으로 Task를
// 상속받아 유저 쓰레드를 구현해도 상관없다"). 이 이름 자체는 제안일
// 뿐 확정된 이름은 아니다 - 프로세스/스레드 모델을 실제로 설계할 때
// 최종 확정한다.
class UserThread : public Task {
public:
    // 대기 중(아직 wait()/waitForMultipleSyscall()/
    // waitAnyForMultipleSyscall()로 소비되지 않은) syscall 하나 -
    // endpoint/token만 담는다("소유권" 자체는 이 값이 pendingSyscalls에
    // 들어있다는 사실 자체로 표현되므로 별도 valid 플래그 불필요,
    // ChunkedList::Slot::used가 그 역할을 대신한다).
    struct PendingSyscall {
        SyscallEndpointId endpoint = 0;
        AsyncTaskManageCode token = 0;
    };

    // 한 스레드가 동시에 여러 syscall을 제출/대기할 수 있어야 한다
    // (waitForMultipleSyscall/waitAnyForMultipleSyscall, SP-04EE2A18
    // QU-31402585/QU-F475C6C2 설계자 답변, 2026-09-14) - 그래서 단일
    // 필드가 아니라 청크 기반 연결 리스트(ChunkedList, libkenv, 재사용
    // 가능한 GENERIC 컨테이너로 만들라는 설계자 지시)에 여러 개를
    // 담는다(**단순 배열 금지** - 설계자가 명시).
    //
    // 청크 용량 10을 고른 이유: Slot{PendingSyscall(16B)+bool(1B, 8B로
    // 패딩)} = 24B, Chunk{Slot[10](240B)+next 포인터(8B)} = 248B -
    // GenericSlabAllocator의 7단계 버킷(SP-D7013B26) 중 256B 버킷에
    // 8B 낭비로 거의 꽉 채워 들어간다.
    static constexpr uint32_t kPendingSyscallChunkCapacity = 10;
    ChunkedList<PendingSyscall, kPendingSyscallChunkCapacity> pendingSyscalls;

    // 이 유저 스레드가 속한 프로세스(SP-8B6B8D25 §2-B, 유저 모드 페이지
    // 폴트를 그 프로세스의 PCB에 매다는 데 필요) - process.h가
    // UserThread를 참조하는 반대 방향 관계라 순환 include를 피하려고
    // 여기서는 전방 선언 포인터로만 갖는다(async_task.h의 `struct
    // Task;`와 동일한 관례). 아직 프로세스 생성 경로 자체가 없어
    // 항상 nullptr로 남는다.
    Process* process = nullptr;
};

// endpointId(공개 ABI, 고정 슬롯) <-> AsyncTaskHandler 매핑 - 내부적으로
// AsyncCallbackRegistry에 등록하고 그 결과 subjectCode(동적 배정)를
// endpointId 슬롯에 저장해 둔다. 이렇게 간접화하면 syscall 하나가
// AsyncTask 하나로 그대로 흐르는 기존 dispatch 경로
// (kAsyncTaskEntryWrapper -> AsyncCallbackRegistry::resolve)를 전혀
// 건드리지 않고 재사용할 수 있다 - endpointId 공간과 subjectCode
// 공간이 섞이지 않는다.
class SyscallRegistry {
public:
    // 이미 채워진 슬롯에 다시 등록하면 실패(설계 실수 조기 발견용).
    static bool registerHandler(SyscallEndpointId endpointId, AsyncTaskHandler* handler);

    // Syscall::submit이 실제 AsyncTask::submit에 넘길 내부 subjectCode를
    // 얻는 데 쓴다 - 등록 안 된 endpointId면 false.
    static bool resolveSubjectCode(SyscallEndpointId endpointId, AsyncTaskSubjectCode* outSubjectCode);
};

// syscall 하나 = AsyncTask 하나(SP-04EE2A18) - 제출(submit)은 즉시
// 반환, 실제 대기는 별도의 wait() 호출이 담당한다("제출/대기 분리"
// 확정 설계). **반드시 UserThread 실행 흐름에서만 호출해야 한다**
// (Scheduler::currentTask()를 UserThread*로 취급 - RTTI가 없어 호출부
// 책임으로 강제한다). 실제 ring3 syscall 트랩 진입점이 이 두 함수를
// 그대로 호출하게 될 예정이다(아직 트랩 진입 자체는 미구현).
class Syscall {
public:
    // endpointId가 등록돼 있지 않거나 AsyncTask/목록 슬롯 확보에
    // 실패하면 0(유효하지 않은 토큰)을 반환한다 - 블로킹하지 않는다.
    // 성공하면 호출한 UserThread의 pendingSyscalls에 {endpoint, token}
    // 항목 하나를 추가하고 그 토큰을 그대로 반환한다(한 스레드가 여러
    // 번 submit()해 여러 토큰을 동시에 들고 있을 수 있다).
    static AsyncTaskManageCode submit(SyscallEndpointId endpointId, void* args);

    // token이 호출한 UserThread 자신의 pendingSyscalls에 없으면(위조/
    // 타인 토큰, 또는 이미 소비된 토큰) 즉시 false. 이미 완료돼 있으면
    // 즉시 반환하고, 아직이면 완료될 때까지 블로킹한다(도중 풀려도
    // 유저랜드가 같은 token으로 다시 부르면 되므로 - 이 함수 자체가
    // 그 "다시 부름"과 완전히 동일한 코드 경로다, 별도 재합류 API
    // 불필요). 반환값은 AsyncTaskState::Completed로 끝났으면 true,
    // Failed로 끝났으면 false. 내부적으로 waitForAnyOf(토큰 1개짜리
    // 배열)와 완전히 같은 코드 경로를 탄다.
    static bool wait(AsyncTaskManageCode token);

    enum class MultiWaitOutcome { Completed, Failed, Invalid };

    struct MultiWaitResult {
        AsyncTaskManageCode token = 0;
        MultiWaitOutcome outcome = MultiWaitOutcome::Invalid;
    };

    // waitForMultipleSyscall/waitAnyForMultipleSyscall(SP-04EE2A18,
    // QU-31402585/QU-F475C6C2 설계자 답변, 2026-09-14) - 둘 다 넘겨준
    // tokens 중 이미 끝났거나(Completed/Failed) 유효하지 않은(자기
    // 소유가 아니거나 이미 소비된) 게 있으면 그중 하나를 즉시 반환하고,
    // 전부 아직이면 그중 아무 하나가 끝날 때까지 블로킹한다 - **둘의
    // 내부 메커니즘은 완전히 동일**하고(그래서 이 헤더에서도 같은
    // private 구현(waitForAnyOf)을 공유한다), 차이는 순수하게 "호출부가
    // 몇 번 부르는가"라는 사용 관례뿐이다:
    //
    // - waitForMultipleSyscall (AND 의미): tokens로 지정한 N개를 전부
    //   드레인하려면 호출부(유저랜드)가 "아직 결과를 못 받은 토큰들"만
    //   추려 이 함수를 최대 N번 반복 호출해야 한다.
    // - waitAnyForMultipleSyscall (OR 의미): tokens 중 아무 하나가
    //   끝나면 그걸로 답이 완성되므로 한 번만 불러도 충분하다.
    static MultiWaitResult waitForMultipleSyscall(const AsyncTaskManageCode* tokens, uint32_t count);
    static MultiWaitResult waitAnyForMultipleSyscall(const AsyncTaskManageCode* tokens, uint32_t count);

private:
    // wait()/waitForMultipleSyscall()/waitAnyForMultipleSyscall() 셋
    // 다가 공유하는 공용 구현 - "주어진 토큰 집합 중 하나가 끝나길
    // 기다린다"는 하나의 메커니즘.
    static MultiWaitResult waitForAnyOf(const AsyncTaskManageCode* tokens, uint32_t count);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SYSCALL_H
