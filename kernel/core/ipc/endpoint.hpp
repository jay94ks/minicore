// Call/Reply 시스템 콜 (docs/spec/ipc.md §3, §5, docs/plan/kernel-bootstrap.md
// M6). 레지스터 기반 짧은 메시지만 다룬다 — 페이지·핸들 전달(ipc.md §4의
// handles[]/pages[])은 M7. 도네이션(ADR-028, §5)은 포함한다.
//
// "시스템 콜"이라는 이름이지만 M4와 같은 이유(objects.md §5 참고)로
// 아직 유저모드/트랩이 없어 평범한 커널 내부 함수로 구현한다.
#pragma once

#include <libk/result.hpp>

#include "ipc/message.hpp"
#include "object/handle_table.hpp"
#include "object/kernel_objects.hpp"

namespace ipc {

// 송신 + 응답 대기(블록). h는 CAN_SEND 권한이 있는 endpoint 핸들이어야
// 한다.
result<void, ipc_error> sys_call(object::handle_table& table, object::handle h,
                                  const message& msg_in, message& msg_out);

// 호출 수신 대기(블록). h는 CAN_RECV 권한이 있는 endpoint 핸들이어야
// 한다. 성공 시 호출자를 식별하는 badge를 반환한다(objects.md §3 —
// 호출에 쓰인 handle이 프록시가 아니라 소유 핸들이면 항상 0).
result<uint64_t, ipc_error> sys_recv(object::handle_table& table, object::handle h,
                                      message& msg_out);

// 가장 최근 sys_recv로 받은 호출에 응답한다. 대응하는 sys_recv가 없는
// 상태(즉 이 스레드가 아무 호출도 받은 적 없거나 이미 응답한 뒤)에
// 호출되면 아무 동작도 하지 않는다(ipc.md §3 — 오류 아님). 블록하지
// 않는다 — 응답만 전달하고 자신은 계속 실행한다.
void sys_reply(const message& msg_in);

}  // namespace ipc
