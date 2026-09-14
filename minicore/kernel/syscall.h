#ifndef MINICORE_KERNEL_SYSCALL_H
#define MINICORE_KERNEL_SYSCALL_H

#include "async_task.h"
#include "libkenv/types.h"
#include "task.h"

namespace kernel {

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
    // 대기 중인 syscall 하나 - 한 스레드는 한 번에 최대 하나만 가질 수
    // 있다(동기적 모델 - 대기 중엔 그 스레드 자체가 실행되지 않는다).
    // 대기 중이 아니면 valid=false.
    struct PendingSyscall {
        SyscallEndpointId endpoint = 0;
        AsyncTaskManageCode token = 0;
        bool valid = false;
    };
    PendingSyscall pendingSyscall;
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
    // endpointId가 등록돼 있지 않거나 AsyncTask 확보에 실패하면
    // 0(유효하지 않은 토큰)을 반환한다 - 블로킹하지 않는다. 성공하면
    // 호출한 UserThread의 pendingSyscall에 {endpoint, token, valid=true}
    // 를 기록하고 그 토큰을 그대로 반환한다.
    static AsyncTaskManageCode submit(SyscallEndpointId endpointId, void* args);

    // token이 호출한 UserThread 자신의 pendingSyscall.token과 다르면
    // (위조/타인 토큰, 또는 이미 소비된 토큰) 즉시 false. 이미 완료돼
    // 있으면 즉시 반환하고, 아직이면 완료될 때까지 블로킹한다(도중
    // 풀려도 유저랜드가 같은 token으로 다시 부르면 되므로 - 이 함수
    // 자체가 그 "다시 부름"과 완전히 동일한 코드 경로다, 별도 재합류
    // API 불필요). 반환값은 AsyncTaskState::Completed로 끝났으면 true,
    // Failed로 끝났으면 false.
    static bool wait(AsyncTaskManageCode token);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SYSCALL_H
