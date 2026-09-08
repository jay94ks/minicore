// IPC 메시지 구조 + 에러 코드 (docs/spec/ipc.md §4, §6). M6은 label+regs
// (레지스터 계층, ADR-013)만 다룬다 — page_descriptor/handle_transfer
// (페이지·핸들 계층)는 M7에서 채운다. 그때까지 message는 spec 구조체의
// 부분집합이다.
#pragma once

#include <cstdint>

namespace ipc {

inline constexpr size_t k_message_registers = 4;

struct message {
    uint32_t label = 0;
    uint64_t regs[k_message_registers] = {};
    // page_count/pages[]/handle_count/handles[] — M7에서 추가.
};

enum class ipc_error : uint32_t {
    ok = 0,
    invalid_handle,
    wrong_object_type,
    permission_denied,
    message_too_large,  // M7(페이지 전달)부터 실제로 반환된다.
    page_not_mapped,    // M7부터 실제로 반환된다.
    cancelled,          // 상대 스레드 종료 처리(아직 없음)가 생기면 실제로 반환된다.
};

}  // namespace ipc
