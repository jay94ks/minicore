// 핸들 테이블 + 프록시 트리 구현 (docs/spec/objects.md §3~6).
#include "object/handle_table.hpp"

#include <new>

#include <k/irq_safe.hpp>
#include <k/spinlock.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>

namespace kern::object {

namespace {
// M56(musl-userland-porting.md §M56, ADR-229) 실행 중 발견 — OPEN-68이
// 이미 "handle_table은 자체 락이 없다"고 지적해 뒀던 것이 실제
// 데이터 손상으로 처음 드러났다. M37의 pthread(sys_thread_create)는
// handle_table을 클론하지 않고 그대로 **공유**한다(ADR-212, pthread의
// 핵심) — 그런데 pthread_create() 자신이 "새 스레드를 가리키는 소유
// 핸들"을 그 공유 테이블에 만드는 동안(process_ops.cpp::thread_create()
// 의 create_owner 호출), 방금 만든 새 스레드가 다른 코어에서 이미
// 스케줄돼 같은 테이블에 자기 몫의 프록시 핸들을(예: sys_reply의
// handles[] 위임, mc_vfs_open의 응답으로 받는 fs_handle) 동시에
// 만들려 하면, allocate_slot()의 "읽고 나서 쓰는"(비원자적) 빈 슬롯
// 탐색이 두 코어에서 동시에 같은 슬롯을 "비어 있다"고 보고 하나가
// 다른 하나를 조용히 덮어쓴다 — 어느 쪽 승자든 그 핸들이 가리키는
// 실제 대상이 호출자가 기대한 것과 달라져, 이후 그 핸들로 하는 모든
// IPC가 "성공한 것처럼 보이지만 엉뚱한 곳과 대화"하게 된다(msh의
// 빌트인 cat이 겪은 정확한 증상 — open()은 성공했는데 그 직후의
// read()가 항상 0바이트를 돌려줬다, 실제로는 vfs가 아니라 다른
// 무언가에 요청이 갔던 것). M1~M55까지는 한 프로세스의 handle_table을
// 진짜로 동시에 두 코어가 건드리는 시나리오(pthread+IPC 조합)가 한
// 번도 실사용된 적이 없어(M37 자기테스트도 "두 워커 모두 IPC/핸들
// 조작을 전혀 안 해" 이 경로를 안 건드렸다고 스스로 기록해 뒀다)
// 드러나지 않았다.
//
// 수정: handle_table 전체(모든 인스턴스, 즉 이 프로세스든 다른
// 프로세스든)에 걸친 단일 전역 락 하나로 allocate_slot()이 관련된
// 모든 변경 경로(create_owner/create_proxy/close)를 감싼다. create_proxy
// 는 서로 다른 두 handle_table(호출자 테이블+dest_table)을 동시에
// 다루는데, 테이블별로 락을 따로 두면 두 create_proxy가 반대 순서로
// 두 테이블을 잠글 때 교착할 위험이 생긴다 — 이 프로젝트의 handle_table
// 연산은 전부 배열 읽기/쓰기 수준으로 짧아(장시간 보유 구간이 없다)
// 전역 락 하나로 묶는 비용이 그 복잡도를 감수할 가치보다 크다고
// 판단했다(YAGNI — 실측으로 경합이 문제가 되면 그때 테이블별 락+
// 순서 규칙으로 다시 좁힌다). allocate_slot()/cascade_revoke()는
// "호출자가 이미 이 락을 들고 있다"고 가정하는 내부 헬퍼로 남겨
// 재진입(같은 스레드가 스핀락을 두 번 잠가 자기 자신과 교착하는 것)
// 을 피한다.
spinlock g_lock;
}  // namespace

handle_table* create_handle_table() {
    // handle_table.hpp 상단 주석 참고 — M4(kernel_main.cpp)가 쓰던 것과
    // 동일한 크기(order 2 = 16KiB, sizeof(handle_table) 여유 있게 담김).
    constexpr uint32_t k_order = 2;
    auto page = kern::mm::alloc_pages(k_order, 0);
    if (!page.is_ok()) {
        return nullptr;
    }
    void* mem = kern::mm::phys_to_virt(page.value());
    return new (mem) handle_table();
}

result<handle, handle_error> handle_table::allocate_slot() {
    for (handle i = 1; i < k_max_handles; ++i) {
        if (!in_use_[i]) {
            in_use_[i] = true;
            return result<handle, handle_error>::ok(i);
        }
    }
    return result<handle, handle_error>::err(handle_error::table_full);
}

result<handle, handle_error> handle_table::create_owner(object_kind kind, uint32_t rights,
                                                          void* object) {
    scoped_lock<spinlock> guard(g_lock);
    auto slot = allocate_slot();
    if (!slot.is_ok()) {
        return result<handle, handle_error>::err(slot.error());
    }

    handle h = slot.value();
    handle_entry& e = entries_[h];
    e.kind = kind;
    e.rights = rights;
    e.object = object;
    e.valid = true;
    e.badge = 0;
    e.parent = nullptr;
    e.depth = 0;
    e.owner_table = this;
    e.self_handle = h;
    return result<handle, handle_error>::ok(h);
}

result<handle, handle_error> handle_table::create_proxy(handle src_handle, uint32_t rights_mask,
                                                          handle_table& dest_table,
                                                          uint64_t badge_override,
                                                          bool has_badge_override) {
    // this(src_handle이 사는 테이블)와 dest_table이 다른 handle_table
    // 인스턴스일 수 있다(대개 그렇다 — 서로 다른 프로세스) — 위
    // g_lock 주석대로 인스턴스별 락 대신 전역 락 하나로 묶어 둬서
    // 이 지점에서 "어느 순서로 두 테이블을 잠글까"를 고민할 필요가
    // 없다.
    scoped_lock<spinlock> guard(g_lock);
    if (src_handle == k_invalid_handle || src_handle >= k_max_handles || !in_use_[src_handle]) {
        return result<handle, handle_error>::err(handle_error::invalid_handle);
    }
    handle_entry& src = entries_[src_handle];
    if (!src.valid) {
        return result<handle, handle_error>::err(handle_error::invalid_handle);
    }
    // ADR-032: 깊이 상한. 순환은 구조적으로 불가능하다(objects.md §6
    // 말미 설명) — 새 프록시는 항상 새 노드이므로 별도 순환 검사가
    // 필요 없다.
    if (src.depth + 1 > k_max_proxy_chain_depth) {
        return result<handle, handle_error>::err(handle_error::max_depth_exceeded);
    }

    auto slot = dest_table.allocate_slot();
    if (!slot.is_ok()) {
        return result<handle, handle_error>::err(slot.error());
    }

    handle new_h = slot.value();
    handle_entry& dst = dest_table.entries_[new_h];
    dst.kind = src.kind;
    dst.rights = src.rights & rights_mask;  // 부모의 부분집합만(ADR-029).
    dst.object = src.object;
    dst.valid = true;
    // badge는 재위임 가능한 마스터 캐패빌리티에서만 새로 지정되고, 그 외에는
    // 부모 것을 그대로 상속한다(objects.md §3) — "마스터인지" 판별에
    // 필요한 신원 인코딩(ADR-084)이 아직 없어, 지금은 호출자가 명시적으로
    // 넘기는지(has_badge_override)로만 구분한다.
    dst.badge = has_badge_override ? badge_override : src.badge;
    dst.parent = &src;
    dst.depth = src.depth + 1;
    dst.owner_table = &dest_table;
    dst.self_handle = new_h;

    src.children.push_back(dst);

    return result<handle, handle_error>::ok(new_h);
}

void handle_table::cascade_revoke(handle_entry& e) {
    e.valid = false;
    for (handle_entry& child : e.children) {
        cascade_revoke(child);
    }
}

result<void, handle_error> handle_table::close(handle h) {
    scoped_lock<spinlock> guard(g_lock);
    if (h == k_invalid_handle || h >= k_max_handles || !in_use_[h]) {
        return result<void, handle_error>::err(handle_error::invalid_handle);
    }

    handle_entry& e = entries_[h];
    if (!e.valid) {
        return result<void, handle_error>::err(handle_error::already_closed);
    }

    // 1~2단계: 이 노드와 모든 자손(다른 handle_table에 있는 것 포함, 각
    // handle_entry가 owner_table로 자신이 속한 테이블을 알고 있으므로
    // cascade_revoke는 어느 테이블 소속인지 신경 쓰지 않고 순수하게
    // 트리만 따라간다)을 무효화한다.
    cascade_revoke(e);

    // 3단계: 프록시였다면 부모의 children 목록에서 제거한다.
    if (e.parent != nullptr) {
        intrusive_list<handle_entry, &handle_entry::children_hook>::erase(e);
    }
    // 4단계(소유 핸들 — 객체 자체 파괴)는 아직 구현하지 않는다. 이 파일
    // 상단 주석과 docs/done/kernel-bootstrap-m4.md 참고 — 스레드
    // 종료·주소공간 페이지테이블 해제 등 실제 생명주기 관리가 필요해지는
    // 이후 마일스톤으로 미룬다.

    return result<void, handle_error>::ok();
}

result<handle_table::info, handle_error> handle_table::handle_info(handle h) const {
    scoped_lock<spinlock> guard(g_lock);
    if (h == k_invalid_handle || h >= k_max_handles || !in_use_[h] || !entries_[h].valid) {
        return result<info, handle_error>::err(handle_error::invalid_handle);
    }
    return result<info, handle_error>::ok(info{entries_[h].kind, entries_[h].rights});
}

const handle_entry* handle_table::debug_entry(handle h) const {
    scoped_lock<spinlock> guard(g_lock);
    if (h == k_invalid_handle || h >= k_max_handles || !in_use_[h]) {
        return nullptr;
    }
    return &entries_[h];
}

}  // namespace kern::object
