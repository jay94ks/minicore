// Notification 시스템 콜 (docs/spec/ipc.md §7, docs/plan/kernel-bootstrap.md
// M7). "시스템 콜"이라는 이름이지만 M4/M6과 같은 이유로 아직 유저모드/
// 트랩이 없어 평범한 커널 내부 함수로 구현한다.
//
// ipc.md §3의 표는 sys_notify를 void, sys_wait를 맨 u64로 적어
// "실패 없음"을 강조하지만, 핸들 자체가 무효/다른 종류일 수는 있다 —
// 이 구현은 sys_call/sys_recv(M6)와 일관되게 result<>로 그 경우를
// 표현한다(핸들이 유효한 뒤부터는 spec 그대로 절대 실패하지 않는다).
#pragma once

#include <libk/result.hpp>

#include "ipc/message.hpp"
#include "object/handle_table.hpp"

namespace ipc {

// 대상 비트셋에 bits를 OR한다. 대기 중인 sys_wait가 있으면 즉시
// 깨운다. h는 object_kind::notification 핸들이어야 한다(rights 검사는
// 없다 — objects.md가 notification 전용 rights 비트를 정의하지 않았다).
result<void, ipc_error> sys_notify(object::handle_table& table, object::handle h, uint64_t bits);

// 현재 비트셋이 0이 아니면 즉시 그 값을 반환하고 원자적으로 0으로
// clear한다. 0이면 0이 아닌 값이 도착할 때까지 블록한다.
result<uint64_t, ipc_error> sys_wait(object::handle_table& table, object::handle h);

}  // namespace ipc
