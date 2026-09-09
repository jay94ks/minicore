// Call/Reply 시스템 콜 (docs/spec/ipc.md §3~5, docs/plan/kernel-bootstrap.md
// M6~M7). 도네이션(ADR-028, §5) 포함. M7부터 message의 pages[]
// (copy 모드만, ADR-015 — move/map은 이후 계획)와 handles[](objects.md
// §4)도 실제로 전달한다 — message.hpp 상단 주석의 방향 규약(수신자가
// sys_recv 전에 목적지 페이지를 미리 채워 둬야 함)을 그대로 따른다.
//
// M13(system-servers-bringup.md §M13, ADR-151) — sys_reply가 이제
// handles[]를 실어 나른다(ADR-018의 "VFS가 open() 응답에서 FS 서버
// 핸들을 위임한다"가 실제로 이걸 요구한다) — 그래서 handle_table
// 인자를 받는다. **여전히 남은 단순화**: 응답의 pages[] "내용"
// (page_descriptor가 가리키는 실제 버퍼) 복사는 여전히 같은
// 주소공간에서만 안전하다(M6/M7 커널 스레드 데모 전제) — message
// 구조체 자체(label/regs/page_count/handle_count)는 이제 서로 다른
// 유저 주소공간 사이에서도 안전하게 오가지만(ADR-151의 vaddr 번역),
// pages[]가 가리키는 데이터 본체까지 번역하는 것은 이 마일스톤 범위
// 밖이다(M13은 짧은 값을 전부 regs[]로만 주고받아 이 경로를 아예
// 쓰지 않는다) — 실제로 필요해지면 이후 마일스톤에서 재검토.
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
// 호출되면 아무 동작도 하지 않는다(ipc.md §3 — 오류 아님, ok 반환).
// 블록하지 않는다 — 응답만 전달하고 자신은 계속 실행한다. table은
// 이 스레드(응답자) 자신의 handle_table — msg_in.handles[]가 있으면
// 그 table을 소스로 caller의 handle_table에 프록시를 만든다(이 파일
// 상단 주석의 "알려진 단순화" 참고 — pages[] 내용 자체는 아직 옮기지
// 않는다).
result<void, ipc_error> sys_reply(object::handle_table& table, const message& msg_in);

}  // namespace ipc
