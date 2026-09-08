// 커널과 유저모드(초기에는 initrun만)가 공유하는 최소 syscall ABI
// (docs/plan/kernel-bootstrap.md M8, docs/spec/boot.md §6). repo-layout.md가
// "유저랜드와 공유하는 커널 ABI 헤더"라고 정의한 자리가 kernel/include다.
//
// message는 kernel/core/ipc/message.hpp의 ipc::message와 **필드 순서·
// 타입이 완전히 동일**해야 한다 — 유저(initrun)와 커널이 서로 다른
// 컴파일 단위(별도 실행파일)에서 각자 이 레이아웃을 알고 있어야 하기
// 때문에, kernel-internal 헤더를 유저 실행파일에 직접 include하는 대신
// 이렇게 ABI 전용으로 복제해 둔다(실제 커널들이 uapi 헤더를 이렇게
// 분리해 쓰는 것과 같은 이유). ipc::message가 바뀌면 이 파일도 함께
// 갱신해야 한다.
#pragma once

#include <cstdint>

namespace uapi {

// syscall 번호. RDI(1번 인자)에 싣는다 — syscall.S/syscall.cpp 참고.
inline constexpr uint64_t k_syscall_ipc_call = 0;  // ipc::sys_call 그대로 노출.

inline constexpr uint32_t k_message_registers = 4;
inline constexpr uint32_t k_max_page_descriptors = 4;
inline constexpr uint32_t k_max_handle_transfers = 2;

enum class transfer_mode : uint8_t {
    copy = 0,
    move = 1,
    map = 2,
};

struct page_descriptor {
    uint64_t vaddr = 0;
    uint64_t length = 0;
    transfer_mode mode = transfer_mode::copy;
};

struct handle_transfer {
    uint32_t src_handle = 0;
    uint32_t rights_mask = 0;
};

// ipc::message와 바이트 단위로 동일한 레이아웃 — kernel/core/ipc/message.hpp
// 상단 주석 참고(pages[]/handles[]의 방향 규약도 그대로 적용된다).
struct message {
    uint32_t label = 0;
    uint32_t page_count = 0;
    uint32_t handle_count = 0;
    uint64_t regs[k_message_registers] = {};
    page_descriptor pages[k_max_page_descriptors] = {};
    handle_transfer handles[k_max_handle_transfers] = {};
};

}  // namespace uapi
