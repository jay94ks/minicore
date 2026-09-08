// Call/Reply 시스템 콜 (docs/spec/ipc.md §3~5, docs/plan/kernel-bootstrap.md
// M6~M7). 도네이션(ADR-028, §5) 포함. M7부터 message의 pages[]
// (copy 모드만, ADR-015 — move/map은 이후 계획)와 handles[](objects.md
// §4)도 실제로 전달한다 — message.hpp 상단 주석의 방향 규약(수신자가
// sys_recv 전에 목적지 페이지를 미리 채워 둬야 함)을 그대로 따른다.
//
// **알려진 단순화**: sys_reply는 스펙 시그니처(objects.md §5,
// ipc.md §3)에 handle_table 인자가 없어 handles[] 위임을 처리할 수
// 없다 — 응답에는 pages[]/handles[]를 싣지 않는다(label+regs만).
// 응답에도 페이지·핸들을 실어야 한다면 시그니처 자체를 다시 봐야
// 한다 — 이 마일스톤은 그 재검토를 하지 않는다.
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
// 한다. msg_in.pages[i]/handles[i]가 있으면 전달을 시도한다 —
// msg_out.pages[i]를 미리 채워 두지 않았으면(message.hpp 방향 규약)
// ipc_error::page_not_mapped로 실패한다(블록하지 않고 즉시 반환).
result<void, ipc_error> sys_call(object::handle_table& table, object::handle h,
                                  const message& msg_in, message& msg_out);

// 호출 수신 대기(블록). h는 CAN_RECV 권한이 있는 endpoint 핸들이어야
// 한다. 성공 시 호출자를 식별하는 badge를 반환한다(objects.md §3 —
// 호출에 쓰인 handle이 프록시가 아니라 소유 핸들이면 항상 0). 페이지를
// 받으려면 호출 전에 msg_out.page_count/pages[]에 목적지 버퍼를 미리
// 채워 둬야 한다.
result<uint64_t, ipc_error> sys_recv(object::handle_table& table, object::handle h,
                                      message& msg_out);

// 가장 최근 sys_recv로 받은 호출에 응답한다. 대응하는 sys_recv가 없는
// 상태(즉 이 스레드가 아무 호출도 받은 적 없거나 이미 응답한 뒤)에
// 호출되면 아무 동작도 하지 않는다(ipc.md §3 — 오류 아님). 블록하지
// 않는다 — 응답만 전달하고 자신은 계속 실행한다. label+regs만
// 전달한다(이 파일 상단 주석의 "알려진 단순화" 참고).
void sys_reply(const message& msg_in);

}  // namespace ipc
