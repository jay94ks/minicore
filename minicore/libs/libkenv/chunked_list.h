#ifndef MINICORE_LIBS_LIBKENV_CHUNKED_LIST_H
#define MINICORE_LIBS_LIBKENV_CHUNKED_LIST_H

#include "libkenv/types.h"

// libkenv: 청크(chunk) 단위로 저장 공간을 확보하는 범용 연결 리스트 -
// 원소 하나마다 개별 할당(단순 연결 리스트)하지도, 고정 배열(용량을
// 넘으면 더 못 담음)로 담지도 않는다. SP-04EE2A18의
// waitForMultipleSyscall/waitAnyForMultipleSyscall 설계(QU-F475C6C2
// 답변, 2026-09-14 - "UserThread::PendingSyscall 필드를 단순 배열이
// 아니라 재사용 가능한 GENERIC 컨테이너인 청크 기반 연결 리스트로
// 다중화")에서 처음 필요해졌지만, 이 헤더 자체는 그 용도에 종속되지
// 않은 순수 자료구조다 - 원소 타입과 청크 용량만 템플릿 인자로 받는다.
//
// 할당자는 함수 포인터 두 개(alloc/free)로 주입받는다 - 이 라이브러리는
// GenericSlabAllocator(minicore/libs/libkmm)보다 아래 계층(아키텍처
// 무관 early 런타임, RM-23F4B687 디렉터리 배치 규칙)이라 그 존재
// 자체를 몰라야 하기 때문이다. 이 커널 코드베이스 전역 관례대로
// placement new는 쓰지 않는다(cxxabi.cpp - 전역 operator new 자체가
// 없음) - 할당받은 원시 메모리를 reinterpret_cast로 얹고 모든 필드를
// 명시적으로 초기화한다(async_task.cpp의 AsyncTask::init과 동일한
// 패턴 - 슬랩 재사용 메모리라 기본 멤버 초기화식이 실행된다는 보장이
// 없다).

namespace kernel {

template <typename T, uint32_t ChunkCapacity>
class ChunkedList {
public:
    using AllocFn = void* (*)(uint64_t size);
    using FreeFn = void (*)(void* ptr, uint64_t size);

    struct Slot {
        T value{};
        bool used = false;
    };

    // 할당자 콜백을 등록한다 - insert()보다 먼저 준비돼 있어야 한다.
    // 이미 등록돼 있으면(재호출) 아무 효과 없이 무시한다 - 별도 생성자
    // 훅이 없는 타입(예: UserThread, 아직 전용 init()이 없음)이 여러
    // 호출 지점에서 "혹시 몰라 매번" 불러도 안전하게 하기 위함이다.
    void ensureAllocator(AllocFn allocFn, FreeFn freeFn) {
        if (_alloc == nullptr) {
            _alloc = allocFn;
            _free = freeFn;
        }
    }

    // 값 하나를 담을 슬롯을 확보해 채운다 - 기존 청크에 빈 슬롯이
    // 있으면 그걸 재사용하고, 없으면 새 청크를 하나 할당해 리스트
    // 앞에 붙인다. 실패(할당 고갈) 시 nullptr - 절대 블로킹하지 않는다
    // (GenericSlabAllocator::alloc과 동일한 비블로킹 정책을 그대로
    // 물려받는다, 호출부가 그 함수를 alloc으로 넘긴다는 전제).
    Slot* insert(const T& value) {
        for (Chunk* chunk = _head; chunk != nullptr; chunk = chunk->next) {
            for (uint32_t i = 0; i < ChunkCapacity; ++i) {
                if (!chunk->slots[i].used) {
                    chunk->slots[i].value = value;
                    chunk->slots[i].used = true;
                    return &chunk->slots[i];
                }
            }
        }

        void* mem = _alloc(sizeof(Chunk));
        if (mem == nullptr) {
            return nullptr;
        }
        Chunk* chunk = reinterpret_cast<Chunk*>(mem);
        chunk->next = _head;
        for (uint32_t i = 0; i < ChunkCapacity; ++i) {
            chunk->slots[i].used = false;
        }
        chunk->slots[0].value = value;
        chunk->slots[0].used = true;
        _head = chunk;
        return &chunk->slots[0];
    }

    // insert()가 반환한 슬롯을 비운다 - 청크 자체는 즉시 반납하지
    // 않는다(다른 슬롯이 여전히 쓰이고 있을 수 있음). 완전히 빈 청크가
    // 쌓이는 게 걱정되면 compact()를 별도로 부른다.
    void erase(Slot* slot) {
        slot->used = false;
    }

    // used인 슬롯 중 predicate(const T&)가 true인 첫 슬롯을 찾는다.
    template <typename Predicate>
    Slot* find(Predicate&& predicate) {
        for (Chunk* chunk = _head; chunk != nullptr; chunk = chunk->next) {
            for (uint32_t i = 0; i < ChunkCapacity; ++i) {
                if (chunk->slots[i].used && predicate(chunk->slots[i].value)) {
                    return &chunk->slots[i];
                }
            }
        }
        return nullptr;
    }

    // used인 슬롯을 전부 방문한다 - fn(T& value, Slot* slot).
    template <typename Fn>
    void forEach(Fn&& fn) {
        for (Chunk* chunk = _head; chunk != nullptr; chunk = chunk->next) {
            for (uint32_t i = 0; i < ChunkCapacity; ++i) {
                if (chunk->slots[i].used) {
                    fn(chunk->slots[i].value, &chunk->slots[i]);
                }
            }
        }
    }

    // 완전히 빈 청크(모든 슬롯 unused)를 리스트에서 제거하고 반납한다.
    void compact() {
        Chunk** link = &_head;
        while (*link != nullptr) {
            Chunk* chunk = *link;
            bool empty = true;
            for (uint32_t i = 0; i < ChunkCapacity && empty; ++i) {
                if (chunk->slots[i].used) {
                    empty = false;
                }
            }
            if (empty) {
                *link = chunk->next;
                _free(chunk, sizeof(Chunk));
            } else {
                link = &chunk->next;
            }
        }
    }

    // 모든 청크를 무조건 반납한다(소유자 자체가 사라질 때 - 예: 프로세스/
    // UserThread 종료 절차, PN-40E976F2가 이어받을 부분).
    void clear() {
        Chunk* chunk = _head;
        while (chunk != nullptr) {
            Chunk* next = chunk->next;
            _free(chunk, sizeof(Chunk));
            chunk = next;
        }
        _head = nullptr;
    }

private:
    struct Chunk {
        Slot slots[ChunkCapacity];
        Chunk* next = nullptr;
    };

    Chunk* _head = nullptr;
    AllocFn _alloc = nullptr;
    FreeFn _free = nullptr;
};

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_CHUNKED_LIST_H
