// 핸들 테이블 + 프록시 트리 구현 (docs/spec/objects.md §3~6).
#include "object/handle_table.hpp"

namespace object {

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
    if (h == k_invalid_handle || h >= k_max_handles || !in_use_[h] || !entries_[h].valid) {
        return result<info, handle_error>::err(handle_error::invalid_handle);
    }
    return result<info, handle_error>::ok(info{entries_[h].kind, entries_[h].rights});
}

const handle_entry* handle_table::debug_entry(handle h) const {
    if (h == k_invalid_handle || h >= k_max_handles || !in_use_[h]) {
        return nullptr;
    }
    return &entries_[h];
}

}  // namespace object
