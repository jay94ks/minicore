#ifndef MINICORE_LIBS_LIBKENV_CHUNKED_LIST_H
#define MINICORE_LIBS_LIBKENV_CHUNKED_LIST_H

#include "libkenv/mem.h"
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
        // [수정, 2026-09-17, PN-584DB994 조사 중 실측 발견] 예전엔
        // `used`만 false로 명시적으로 초기화하고 `value`는 그대로
        // 뒀다 - `T`가 POD/raw 포인터일 땐 무해했지만(주석 상단이
        // 원래 약속한 "모든 필드를 명시적으로 초기화"가 실제로는
        // `value`엔 안 지켜지고 있었음), `T=SharedPtr<U>`/`WeakPtr<U>`
        // 처럼 대입 연산자가 **"현재 값을 먼저 정리(release)한 뒤"**
        // 새 값을 쓰는 타입에서는 이 슬랩 원시 메모리에 남아있던
        // 쓰레기 바이트를 `_block`으로 오인해 `releaseStrong()`/
        // `releaseWeak()`를 쓰레기 주소에 대고 호출하는 미정의 동작으로
        // 이어진다 - 이 Chunk의 **첫 슬롯에 처음 값을 넣는 바로 이
        // 대입문(`chunk->slots[0].value = value` 등)** 자체가 그 트리거다.
        // `memset(0)`으로 전체 Chunk를 밀어 두면 `T*` 계열 멤버가
        // 전부 nullptr(NSDMI와 동일한 상태)가 돼 안전해진다
        // (Process::allocate()가 전체 memset(0)으로 자신의 WeakPtr
        // 필드들을 안전하게 만드는 것과 동일한 근거). 이미 배포된
        // `Process::children`/`openBridges`가 이 함수를 거치는 실재
        // 메모리 안전성 버그다(PN-A8D235E7).
        //
        // [walk-back, 2026-09-17] 최초엔 이 버그가 이 커널의 유일한
        // 미해결·간헐적(4-6%) 실측 버그(PN-584DB994, SMP4 init 페이지
        // 폴트)의 근본 원인일 가능성이 높다고 적어 뒀었으나, 그 문서
        // 자신의 "8회 재현 전부 레지스터 값 완전 동일" 실측 근거와
        // 대조해보니 근거가 안 맞는다고 판단해 정정한다 - 쓰레기
        // 메모리 내용에 좌우되는 버그라면 재현마다 세부가 달라져야
        // 하는데 그렇지 않았고(오히려 결정론적 로직 버그 프로파일),
        // 게다가 PN-584DB994의 재현 시나리오(GRUB SMP4, initrd 없음)
        // 에서는 `children`/`openBridges` 둘 다 애초에 채워지지 않아
        // (init/서비스 프로세스는 SpawnProcess 경로를 안 타 자식이
        // 없고 Channel 연결도 없음) 이 함수의 발동 조건 자체가 그
        // 시나리오에서 논리적으로 성립하지 않는다 - 이 수정은 그
        // 문서와 무관한 별개의 독립적인 버그다(PN-A8D235E7 참고,
        // 근본 원인 재검토는 PN-584DB994 자신에 기록됨).
        memset(chunk, 0, sizeof(Chunk));
        chunk->next = _head;
        chunk->slots[0].value = value;
        chunk->slots[0].used = true;
        _head = chunk;
        return &chunk->slots[0];
    }

    // insert()가 반환한 슬롯을 비운다 - 청크 자체는 즉시 반납하지
    // 않는다(다른 슬롯이 여전히 쓰이고 있을 수 있음). 완전히 빈 청크가
    // 쌓이는 게 걱정되면 compact()를 별도로 부른다.
    //
    // [수정, 2026-09-17, PN-E2A114C1 실측 중 발견] `value = T{}`를
    // 먼저 대입해 옛 값을 명시적으로 정리한 뒤에야 `used`를 끈다 -
    // 이 컨테이너는 지금까지 전부 POD/raw 포인터 T로만 쓰여서
    // "옛 값을 그냥 내버려 둬도 무해하다"는 전제가 성립했지만,
    // `T=SharedPtr<Process>`처럼 실제 소유권을 쥔 값을 담으면 옛
    // 값을 안 지우고 `used=false`만 하는 건 그 슬롯이 들고 있던 강한
    // 참조를 영원히 누수시킨다(erase가 "이 슬롯은 이제 안 쓴다"는
    // 뜻이지 "값이 사라진다"는 뜻이 아니었으므로) - `T{}` 대입이 그
    // 값의 `operator=`(SharedPtr라면 기존 참조를 내려놓고 스스로도
    // 빈 상태가 됨)를 거치게 해 해결한다. 기존 POD/raw 포인터
    // 소비자는 관찰 가능한 차이 없음(어차피 `used=false`인 슬롯의
    // 값은 아무도 다시 안 읽음 - 그 값을 0/nullptr로 되돌리는 대입
    // 자체가 부작용이 없다).
    void erase(Slot* slot) {
        slot->value = T{};
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
    //
    // [수정, 2026-09-17, PN-E2A114C1 실측 중 발견] 청크 메모리를 그냥
    // `_free()`로 반납하기 전에, 아직 `used`인 슬롯의 값을 먼저
    // `T{}`로 되돌린다 - erase()와 동일한 이유(비POD T의 소유권 해제).
    // 이 프로젝트는 placement new/실제 소멸자를 안 쓰는 관례라 이
    // 대입이 유일하게 "값의 정리 로직을 실행시키는" 지점이다 - 이
    // 루프 없이 그냥 Chunk 메모리를 반납하면 `T=SharedPtr<Process>`
    // 같은 경우 그 안에 살아있던 강한 참조가 (가리키던 컨트롤 블록
    // 자체는 계속 존재하는 채로) 조용히 누수된다.
    void clear() {
        Chunk* chunk = _head;
        while (chunk != nullptr) {
            Chunk* next = chunk->next;
            for (uint32_t i = 0; i < ChunkCapacity; ++i) {
                if (chunk->slots[i].used) {
                    chunk->slots[i].value = T{};
                    chunk->slots[i].used = false;
                }
            }
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
