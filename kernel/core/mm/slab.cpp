// 슬랩 힙 구현 (docs/spec/memory.md §6). 각 슬랩은 order-0 페이지
// 하나(4096바이트) — 앞부분에 slab_header를, 나머지를 고정 크기
// 청크로 잘라 침습적 free list로 엮는다(별도 비트맵 없이 빈 청크 자체
// 메모리에 다음 빈 청크 포인터를 써 넣는 전통적 기법).
//
// 알려진 단순화(M3 "최소 구현" 범위):
//   - 완전히 빈 슬랩을 alloc_pages로 반납하지 않는다 — 한 번 늘어난
//     슬랩 개수는 줄지 않는다.
//   - slab_alloc은 각 크기 클래스의 "머리" 슬랩만 보고, 머리가
//     가득 찼으면 나머지 슬랩에 빈 자리가 있어도 새 슬랩부터
//     늘린다(뒤쪽 슬랩을 훑지 않음) — 정확성 문제는 아니고 페이지를
//     다소 낭비할 수 있는 비효율이다.
#include "mm/slab.hpp"

#include <cstdint>

#include <k/irq_safe.hpp>
#include <k/panic.hpp>
#include <k/spinlock.hpp>

#include "mm/page_allocator.hpp"
#include "mm/phys_map.hpp"

namespace kern::mm {

namespace {

// alignas(16)(M9): sizeof(slab_header) 자체가 16의 배수여야 그 뒤에
// 오는 첫 청크(area = header + sizeof(slab_header))가 16바이트 정렬을
// 유지한다 — 페이지 자체는 항상 4096(=16의 배수) 정렬이므로, 그 뒤에
// 얹는 오프셋도 16의 배수면 각 크기 클래스(전부 16의 배수, slab.hpp
// k_slab_size_classes)의 모든 청크가 자연히 16바이트 정렬된다. 이
// alignas가 없으면 세 멤버(8+8+4=20바이트, 8바이트 정렬 요구라 24로
// 패딩)가 24바이트가 되어 16의 배수가 아니게 되고, 모든 청크가
// 8바이트만큼 어긋난다 — kern::object::thread::fxsave_area(ADR-127, FXSAVE/
// FXRSTOR 요구)가 이 문제를 QEMU에서 실제 #GP로 처음 드러냈다(M1~M8은
// 16바이트 정렬을 요구하는 어떤 것도 slab에 넣은 적이 없었다).
struct alignas(16) slab_header {
    slab_header* next_slab = nullptr;
    void* free_chunk_list = nullptr;
    uint32_t free_count = 0;
};

struct size_class_state {
    size_t chunk_size;
    slab_header* slabs = nullptr;
    spinlock lock;
};

size_class_state g_size_classes[k_slab_size_class_count] = {
    {k_slab_size_classes[0], nullptr, {}}, {k_slab_size_classes[1], nullptr, {}},
    {k_slab_size_classes[2], nullptr, {}}, {k_slab_size_classes[3], nullptr, {}},
    {k_slab_size_classes[4], nullptr, {}}, {k_slab_size_classes[5], nullptr, {}},
    {k_slab_size_classes[6], nullptr, {}}, {k_slab_size_classes[7], nullptr, {}},
    {k_slab_size_classes[8], nullptr, {}},
};

int size_class_index(size_t size) {
    for (size_t i = 0; i < k_slab_size_class_count; ++i) {
        if (size <= k_slab_size_classes[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 새 order-0 페이지 하나를 받아 slab_header + 고정 크기 청크들로
// 자르고 slabs 리스트 머리에 얹는다. 호출자가 state.lock을 들고
// 있어야 한다.
bool grow_locked(size_class_state& state) {
    auto page = alloc_pages(0, /*preferred_node=*/0);
    if (!page.is_ok()) {
        return false;
    }

    auto* header = static_cast<slab_header*>(phys_to_virt(page.value()));
    header->next_slab = state.slabs;
    header->free_chunk_list = nullptr;
    header->free_count = 0;

    auto* area = reinterpret_cast<unsigned char*>(header) + sizeof(slab_header);
    size_t area_size = k_page_size - sizeof(slab_header);
    size_t chunk_count = area_size / state.chunk_size;

    for (size_t i = 0; i < chunk_count; ++i) {
        void* chunk = area + i * state.chunk_size;
        *reinterpret_cast<void**>(chunk) = header->free_chunk_list;
        header->free_chunk_list = chunk;
    }
    header->free_count = static_cast<uint32_t>(chunk_count);

    state.slabs = header;
    return chunk_count > 0;
}

}  // namespace

void* slab_alloc(size_t size) {
    int idx = size_class_index(size);
    if (idx < 0) {
        return nullptr;  // 4096바이트 초과 — alloc_pages를 직접 쓴다.
    }

    size_class_state& state = g_size_classes[idx];
    scoped_lock<spinlock> guard(state.lock);

    if (state.slabs == nullptr || state.slabs->free_count == 0) {
        if (!grow_locked(state)) {
            return nullptr;
        }
    }

    slab_header* header = state.slabs;
    void* chunk = header->free_chunk_list;
    header->free_chunk_list = *reinterpret_cast<void**>(chunk);
    --header->free_count;
    return chunk;
}

void slab_free(void* ptr, size_t size) {
    int idx = size_class_index(size);
    if (idx < 0) {
        LIBK_PANIC("kern::mm::slab_free: size exceeds largest slab size class");
    }

    size_class_state& state = g_size_classes[idx];
    scoped_lock<spinlock> guard(state.lock);

    auto* header = reinterpret_cast<slab_header*>(reinterpret_cast<uintptr_t>(ptr) &
                                                    ~(static_cast<uintptr_t>(k_page_size) - 1));

    *reinterpret_cast<void**>(ptr) = header->free_chunk_list;
    header->free_chunk_list = ptr;
    ++header->free_count;
}

}  // namespace kern::mm
