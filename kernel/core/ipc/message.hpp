// IPC 메시지 구조 + 에러 코드 (docs/spec/ipc.md §4, §6).
//
// pages[]/handles[]의 방향 규약(스펙이 명시하지 않아 M7 구현 시 정한
// 것): 송신자는 msg_in.pages[i]에 "내가 보낼 것"(vaddr/length/mode)을
// 채운다. **수신자는 sys_recv를 부르기 전에 msg_out.pages[i]에 "내가
// 받을 목적지"(vaddr/length)를 미리 채워 둬야 한다** — copy 모드는
// 커널이 송신자 페이지 내용을 수신자가 미리 지정한 버퍼로 복사하는
// 방식이기 때문이다(ADR-013/015). 전달이 끝나면 msg_out.page_count가
// 실제로 전달된 개수로 갱신된다. handles[]는 objects.md §4 그대로 —
// 수신자 쪽 handles[i].src_handle이 새로 발급된 핸들 번호로 덮어써진다.
#pragma once

#include <cstdint>

namespace ipc {

inline constexpr size_t k_message_registers = 4;
inline constexpr size_t k_max_page_descriptors = 4;
inline constexpr size_t k_max_handle_transfers = 2;

enum class transfer_mode : uint8_t {
    copy = 0,  // 기본값(ADR-015) — M7이 구현하는 유일한 모드.
    move = 1,  // 대상 엔드포인트에 CAN_MOVE 필요(ADR-029) — 미구현(이후 계획).
    map = 2,   // 대상 엔드포인트에 CAN_MAP 필요(ADR-029) — 미구현(이후 계획).
};

struct page_descriptor {
    uint64_t vaddr = 0;    // 페이지 정렬된 가상주소
    uint64_t length = 0;   // 페이지 정렬된 바이트 수
    transfer_mode mode = transfer_mode::copy;
};

struct handle_transfer {
    uint32_t src_handle = 0;   // 송신 시: 송신자 핸들. 수신 후: 수신자 쪽 새 핸들(in/out).
    uint32_t rights_mask = 0;  // 위임 시 적용할 rights 축소 마스크(AND, ADR-029)
};

struct message {
    uint32_t label = 0;
    uint32_t page_count = 0;
    uint32_t handle_count = 0;
    uint64_t regs[k_message_registers] = {};
    page_descriptor pages[k_max_page_descriptors] = {};
    handle_transfer handles[k_max_handle_transfers] = {};
};

enum class ipc_error : uint32_t {
    ok = 0,
    invalid_handle,
    wrong_object_type,
    permission_denied,
    message_too_large,  // page_count/handle_count가 배열 상한을 넘음
    page_not_mapped,    // 수신자가 해당 인덱스에 목적지 버퍼를 준비하지 않았음
    cancelled,          // 상대 스레드 종료 처리(아직 없음)가 생기면 실제로 반환된다.
};

}  // namespace ipc
