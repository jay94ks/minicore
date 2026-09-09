// 핸들 테이블 + 프록시 트리 (docs/spec/objects.md §1~3, §5~6, ADR-011,
// ADR-023, ADR-029, ADR-032). 프로세스별로 하나씩 존재할 자료구조이지만,
// M1~M8 시점엔 아직 실제 프로세스(procsrv, M8 이후)가 없으므로 이
// 마일스톤(M4)은 메커니즘 자체를 완성하고 kernel_main의 자체 테스트로
// 검증한다 — 실제 프로세스 생성 syscall과의 연결은 이후 마일스톤.
//
// "시스템 콜"(sys_handle_close/sys_handle_info, objects.md §5)이라는
// 이름이지만, 아직 유저모드/트랩(IDT, SYSCALL 등, M8 이후 범위)이 없어
// 지금은 평범한 커널 내부 함수로 구현한다 — ABI/트랩 연결은 실제
// 유저모드 진입이 생기는 마일스톤에서 다룬다.
#pragma once

#include <cstdint>

#include <libk/intrusive_list.hpp>
#include <libk/result.hpp>

namespace object {

enum class object_kind : uint32_t {
    thread = 0,
    address_space = 1,
    endpoint = 2,
    notification = 3,
    reg_table = 4,
};

enum class handle_error : uint32_t {
    ok = 0,
    invalid_handle,
    already_closed,
    table_full,
    max_depth_exceeded,
};

using handle = uint32_t;
inline constexpr handle k_invalid_handle = 0;

// ADR-032: 설정 가능해야 하는 값이지만(빌드 타임 기본값 64), 이를 조정할
// 커널 설정 syscall이 아직 없다(M4 범위 밖) — 지금은 상수로 시작한다.
inline constexpr uint32_t k_max_proxy_chain_depth = 64;

// 골격 상한 — 동적 확장은 다루지 않는다(핸들 테이블 자체를 슬랩/페이지
// 할당자 위로 옮기는 것은 실제 프로세스가 생기는 시점에 재검토).
inline constexpr uint32_t k_max_handles = 64;

class handle_table;

// M12(system-servers-bringup.md §M12) — kernel_main.cpp::create_handle_table()
// (M4)가 처음 만든, "슬랩이 아니라 별도 페이지에 placement-new로
// handle_table을 만드는" 패턴을 공용화한다. handle_table은 커다란
// 고정 크기 배열(k_max_handles개의 handle_entry, 각각 intrusive_list
// 센티널 포함)이라 스택에 두기엔 너무 크고, 전역으로 두면 ADR-118이
// 우려한 "동적 초기화 필요" 판정을 컴파일러가 내리기 쉽다 — 이
// 함수처럼 mm이 이미 초기화된 뒤 명시적으로 호출되는 자리에서
// placement new로 만들면 그 문제 자체가 생기지 않는다. 실패 시
// nullptr(페이지 할당 실패).
handle_table* create_handle_table();

struct handle_entry {
    object_kind kind;
    uint32_t rights;
    void* object = nullptr;  // 커널 내부 포인터. 유저에게 노출하지 않는다.
    bool valid = false;
    uint64_t badge = 0;  // endpoint 프록시에서만 의미 있음. 소유 핸들은 항상 0.

    // 트리 메타데이터 (소유 핸들은 parent == nullptr, depth == 0).
    handle_entry* parent = nullptr;
    list_hook children_hook;
    intrusive_list<handle_entry, &handle_entry::children_hook> children;
    uint32_t depth = 0;

    // cascade revoke·슬롯 반납에 필요한 역참조 — objects.md 스펙 구조체에는
    // 없지만, 프록시가 소유 핸들과 다른 handle_table(다른 프로세스)에 살 수
    // 있어(§4) 이 정보 없이는 재귀 중에 "어느 테이블의 몇 번 슬롯을 반납할지"
    // 알 수 없다.
    handle_table* owner_table = nullptr;
    handle self_handle = k_invalid_handle;
};

class handle_table {
public:
    // 새 소유 핸들을 만든다. object_kind/rights/객체 포인터를 그대로
    // 담는다 — 객체 자신은 호출자가 이미 만들어 둔 것을 넘겨준다
    // (예: mm::slab_alloc으로 만든 thread/address_space).
    result<handle, handle_error> create_owner(object_kind kind, uint32_t rights, void* object);

    // objects.md §4(ipc.md의 handles[] 처리)의 핵심 로직 — IPC 자체는
    // 아직 없으므로(M6) 지금은 직접 호출 가능한 함수로 둔다. §4의
    // 4단계(super badge의 jail/guest 유입 차단)는 security-model.md의
    // badge/confinement 메커니즘이 아직 없어(M1~M8 범위 밖) 생략한다.
    result<handle, handle_error> create_proxy(handle src_handle, uint32_t rights_mask,
                                               handle_table& dest_table, uint64_t badge_override,
                                               bool has_badge_override);

    // §6의 철회 절차. 소유 핸들이면 모든 자손 프록시(다른 handle_table에
    // 있는 것 포함)를 재귀적으로 무효화한 뒤 이 테이블의 슬롯을 반납한다.
    // 객체 자체의 파괴(스레드 종료, 주소공간 페이지테이블 해제 등)는
    // 아직 구현하지 않는다 — kernel-bootstrap.md M4 "골격" 범위 밖
    // (실제 생명주기 관리가 필요해지는 이후 마일스톤에서 채운다).
    result<void, handle_error> close(handle h);

    struct info {
        object_kind kind;
        uint32_t rights;
    };
    result<info, handle_error> handle_info(handle h) const;

    // 디버그/검증용 — h가 가리키는 항목을 직접 노출한다(커널 내부에서만
    // 쓴다, 유저에게는 절대 노출하지 않는다 — handle_entry.object 참고).
    const handle_entry* debug_entry(handle h) const;

private:
    // 슬롯은 재사용하지 않는다 — close()된 슬롯도 in_use_는 계속 true로
    // 남아 handle_error::already_closed를 정확히 구분할 수 있게 한다
    // (objects.md §5). k_max_handles가 골격 상한인 이유이기도 하다 —
    // 실제 프로세스가 오래 살아남는 시점에는 슬롯 반납·재사용이
    // 필요해질 것이다.
    result<handle, handle_error> allocate_slot();
    static void cascade_revoke(handle_entry& e);

    handle_entry entries_[k_max_handles];
    bool in_use_[k_max_handles] = {};
};

}  // namespace object
