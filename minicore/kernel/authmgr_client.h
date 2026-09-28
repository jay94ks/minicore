#ifndef MINICORE_KERNEL_AUTHMGR_CLIENT_H
#define MINICORE_KERNEL_AUTHMGR_CLIENT_H

#include "channel.h"
#include "user_record.h"

namespace kernel {

// [DC-90A66932, 방향 B - 설계자 답변 2026-09-28 "완전히 별도의 경량
// 커널 전용 Channel 클라이언트 신설. 이게 성능 측면에서 나아"]
// authmgr에 대한 커널 전용 Channel 클라이언트 - 기존 ConnectChannel/
// ChannelRead/WriteHandler(channel.cpp)를 전혀 재사용하지 않는다:
// 그 핸들러들은 (1) 유저 주소공간 전용 검증(kValidateUserBuffer)을
// 무조건 거치고 (2) 호출자별 OpenBridgeList에서 핸들을 재조회하는데,
// 커널 자신이 제출자면 (1)은 커널 버퍼를 유저 주소공간 검사에 태워
// 항상 실패시키고, (2)는 "커널이 스스로 Channel 클라이언트가 되는"
// 이번이 첫 사례라 애초에 맞는 개념이 아니었다(둘 다 DC-90A66932가
// 발견한 구조적 문제).
//
// 연결은 부팅 후 처음 필요해질 때 한 번 수립해 전역 상태(정적
// SharedPtr<BridgePipe>)로 계속 재사용하고, 끊어지면(쓰기/읽기 실패
// 또는 BrokenPipe) 다음 호출 시 자동으로 재연결을 시도한다(설계자
// 답변 "고정 연결이 수립되어 있고, 끊어지면 자동으로 재연결" 그대로).
//
// **호출 규약**: 아래 함수들은 전부 어떤 AsyncTaskHandler::onExec()
// 코루틴의 실행 컨텍스트 안에서만 호출할 수 있다 - Channel I/O가
// 내부적으로 쓰는 AsyncTask::yield()/AsyncTaskCoroAwaiter 둘 다
// AsyncTask::current()가 유효해야 한다(async_task.cpp). 이 모듈
// 자신은 코루틴을 새로 만들지 않는다(AsyncExecCoro는 다른 AsyncExecCoro
// 를 co_await로 합성할 수 없다는 이 프로젝트의 기존 제약 - ext4/fat32
// VFS 통합이 이미 겪은 것과 동일 - "평탄화" 패턴을 따른다) - 호출부
// 코루틴(예: user_record.cpp의 kSetuidOnExecImpl)이 아래 함수들을
// 순서대로 부르고 필요한 지점(연결 대기)에서만 co_await
// AsyncTaskCoroAwaiter(...)를 직접 쓴다.
//
// 동시 호출 직렬화: 고정 연결 하나를 여러 호출자가 동시에 쓸 수
// 있으므로(예: 여러 UserThread가 동시에 Setuid 캐시미스), 호출부가
// kAuthmgrClientLock()/kAuthmgrClientUnlock()으로 반드시 감싸야 한다
// (busy-yield 기반, 이미 이 파일의 연결/읽기/쓰기 로직과 같은 관례).

// 부팅 시 1회 호출 - 이 클라이언트가 소유할 전용 KernelThread(안정된
// Task 신원)를 스폰한다. authmgr의 AcceptFromChannelHandler(수정 없이
// 그대로 재사용)가 연결된 BridgePipe를 "제출자 Task의 OpenBridgeList"
// 에 강제로 걸어 두므로, 이 커널만큼 수명이 긴 안정적인 소유자가
// 필요하다(devmgr/fs와 동일한 패턴 - 실제로는 아무 작업도 안 하고
// 존재하기만 한다).
void kInitAuthmgrClient();

// 동시 호출 직렬화 - 반드시 짝지어 호출한다(둘 다 busy-yield 가능).
void kAuthmgrClientLock();
void kAuthmgrClientUnlock();

// 이미 연결돼 있으면 nullptr(더 할 일 없음, 바로 다음 단계로 진행).
// 아니면 연결 시도용 AsyncTask*를 반환한다 - 호출부가 완료를 기다린
// 뒤 kAuthmgrFinishConnect()를 불러야 한다.
// **`co_await AsyncTaskCoroAwaiter(...)`로 기다리지 않는다**(실측으로
// 발견한 버그, 2026-09-28) - 이 헤더의 나머지 함수들(WriteLookupRequest/
// ReadLookupResponse 등)이 내부적으로 raw `AsyncTask::yield()`(스택풀
// 방식)를 쓰는데, 호출부 코루틴이 `co_await`로 한 번이라도 진짜
// 정지하면 그 AsyncTask는 `drainOnce()`의 coroutine-handle 재개
// 모드로 영구 전환돼(async_task.cpp 문서 주석) 이후의 raw yield가
// 더 이상 쓰지 않는 스택풀 재개 지점으로 잘못 점프한다(실행 중복/
// 오염 - user_record.cpp kSetuidOnExecImpl에서 실측). 대신
// `AsyncTaskAwaiter(connectTask).await()`를 쓴다 - 이 클래스는
// 매 yield 직전 스스로를 submitCompletion()으로 재제출하는 스택풀
// 전용 범용 대기자라(async_task.cpp) 위 문제를 겪지 않는다("코루틴
// 안에서 부르면 무한 대기"라는 그 클래스의 경고는 호출자 자신이
// 이미 co_await로 coroHandle 모드에 들어간 뒤에만 해당 - Ext4Driver
// 사례. 이 헤더의 함수들을 쓰는 호출부가 co_await를 전혀 안 쓰는 한
// 안전하다, user_record.cpp kSetuidOnExecImpl 실사용 참고).
AsyncTask* kAuthmgrBeginConnect();
// kAuthmgrBeginConnect()가 반환한 태스크의 대기가 끝난 뒤 호출 -
// 연결 결과를 내부 상태에 반영한다(성공 시 이후 read/write가 그
// 연결을 쓴다). 반환값은 "이제 연결돼 있는지" 여부.
bool kAuthmgrFinishConnect(AsyncTask* connectTask);

// [신규, 2026-09-28] 위 BeginConnect/AsyncTaskAwaiter::await()/
// FinishConnect 세 단계를 한 번에 묶은 편의 함수 - 이미 연결돼
// 있으면 즉시 true, 아니면 연결을 시도하고 결과를 반환한다. 이제
// 호출부(LookupByUid 재시도, CheckSudoPermission 둘 다)가 매번 이
// 세 단계를 손으로 다시 쓰지 않는다.
bool kAuthmgrEnsureConnected();

// LookupByUid 요청 전송 - 연결돼 있다고 가정(호출 전 kAuthmgrBeginConnect/
// FinishConnect로 확인), busy-yield로 전송 완료까지 돈다(별도 대기
// 불필요 - co_await 아님). 실패 시 연결을 끊어진 것으로 표시하고
// false를 반환한다(다음 kAuthmgrBeginConnect 호출이 재연결을 시도).
bool kAuthmgrWriteLookupRequest(Uid uid);
// 응답을 읽는다(위와 동일한 이유로 co_await 아님, busy-yield). 성공
// (요청한 uid가 실제로 존재)이면 *outRecord를 채우고 true. uid가
// 없다는 응답(NotFound)이거나 프로토콜/연결 오류면 false(연결
// 자체는 NotFound일 땐 살아있는 채로 유지 - 오류일 때만 끊어진
// 것으로 표시).
bool kAuthmgrReadLookupResponse(UserRecord* outRecord);

// [신규, 2026-09-28, DC-34764C25 항목1 답변] callerUid가 targetUid로
// sudo/su할 자격이 있는지 authmgr의 화이트리스트에 질의한다 - 위
// LookupByUid 함수들과 동일한 호출 규약(연결돼 있다고 가정, 호출 전
// kAuthmgrBeginConnect/FinishConnect로 확인, busy-yield). 통신 자체가
// 실패하면(연결 끊김 등) 안전하게 false(불허)로 처리한다 - 권한
// 승격 경로라 통신 실패를 "허용"으로 잘못 해석하면 안 된다(fail-closed).
bool kAuthmgrCheckSudoPermission(Uid callerUid, Uid targetUid);

// [신규, 2026-09-28, DC-CC83F7BE 답변("(A) 커널 중개") 반영] authmgr에
// 새 UserRecord 생성을 요청한다 - 이 함수를 부르는 시점엔 이미 호출부
// (user_record.cpp의 kCreateUserOnExecImpl)가 caller uid 기준 조상-자손
// 판정을 끝낸 뒤다(권한 판정은 커널 책임, authmgr은 그대로 실행만).
// CheckSudoPermission과 동일한 이유로 write+read를 한 번에 묶는다
// (응답 본문 없음, error 하나뿐). 통신 오류는 false(실패)로 접는다.
bool kAuthmgrCreateUser(const UserRecord& record);

}  // namespace kernel

#endif  // MINICORE_KERNEL_AUTHMGR_CLIENT_H
