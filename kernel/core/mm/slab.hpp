// 커널 힙 (슬랩, docs/spec/memory.md §6, ADR-012). 고정 크기 클래스마다
// per_node_pool(page_allocator.hpp)에서 order-0 페이지를 받아 그
// 크기로 잘라 쓰는 전통적 슬랩 구조. 부트스트랩 단계의 커널 자체
// 자료구조(handle_entry 등, 아직 없음)는 M4 이후 이 위에서 할당된다.
#pragma once

#include <cstddef>

namespace mm {

// 실제 목록은 구현 시 프로파일링으로 조정 가능 — 지금은 spec 예시
// 그대로 쓴다.
inline constexpr size_t k_slab_size_classes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
inline constexpr size_t k_slab_size_class_count =
    sizeof(k_slab_size_classes) / sizeof(k_slab_size_classes[0]);

// 가장 가까운 상위 크기 클래스에서 할당한다. size가 가장 큰 크기
// 클래스(4096)를 넘으면 nullptr을 반환한다 — 그런 큰 할당은
// alloc_pages를 직접 쓴다(§6 범위 밖).
void* slab_alloc(size_t size);

// size는 slab_alloc에 넘겼던 값과 같아야 한다(어느 크기 클래스인지
// 되찾는 데 쓴다 — memory.md §6 시그니처 그대로).
void slab_free(void* ptr, size_t size);

}  // namespace mm
