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

namespace kern::ipc {

inline constexpr size_t k_message_registers = 4;
inline constexpr size_t k_max_page_descriptors = 4;
inline constexpr size_t k_max_handle_transfers = 2;

// ADR-159/161(kernel-ipc-objects.md, kernel-memory.md ADR-160 슬롯 4) —
// 유저 프로세스 수신자에게 pages[]를 매핑으로 전달할 때 쓰는 고정
// 슬롯. k_max_page_descriptors(4)페이지 예산 — 슬롯당 정확히 1페이지
// (endpoint.cpp의 deliver_message가 강제)라 인덱스 i의 페이지는 항상
// 여기서 i*4096만큼 떨어진 자리에 매핑된다.
//
// M56(musl-userland-porting.md §M56, ADR-229) 실행 중 발견 — 위 설명은
// "이 프로세스에 스레드가 하나뿐"이라는 M7~M55의 전제를 깔고 있었다.
// M37의 pthread는 owner_space(따라서 이 고정 가상주소도)를 그대로
// 공유하는데(ADR-212), 두 pthread가 동시에 각자 다른 IPC 응답으로
// pages[]를 받으면 **둘 다 정확히 같은 물리주소**(이 상수 자체가
// 스레드 구분이 없다)로 매핑을 시도해 서로의 응답 페이지를 덮어썼다
// — msh의 빌트인 cat이 open()엔 성공했는데 그 직후 read()가 항상
// 0바이트를 돌려주는 것으로 처음 드러났다(다른 pthread의 응답
// 매핑이 먼저 도착해 있다가 cat 자신의 응답이 오기 전에 이미
// 지워졌거나, 반대로 cat의 매핑이 다른 스레드 것으로 덮어써졌다).
// 그래서 이 슬롯을 **스레드별로** 나눈다 — 슬롯 4 전체 예산이
// 1MiB(kernel-memory.md ADR-160)인데 스레드 하나가 실제로 쓰는
// 건 k_max_page_descriptors(4)페이지(16KiB)뿐이라, 그 안에 최대
// k_max_ipc_mapped_pages_threads(64)개의 독립된 스레드별 부분 슬롯을
// 그대로 채워 넣을 수 있다(64 * 16KiB = 1MiB, 슬롯 4 예산과 정확히
// 맞아떨어진다 — 새 최상위 슬롯 번호가 필요 없다).
inline constexpr uint64_t k_ipc_mapped_pages_user_vaddr = 0x0000700000400000ull;
inline constexpr uint32_t k_max_ipc_mapped_pages_threads = 64;
inline constexpr uint64_t k_ipc_mapped_pages_thread_slot_bytes =
    static_cast<uint64_t>(k_max_page_descriptors) * 4096ull;

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

}  // namespace kern::ipc
