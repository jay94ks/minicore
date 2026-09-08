# C++ 코딩 컨벤션 스펙

**관련 결정**: ADR-003, ADR-010, ADR-042
**관련 설계**: [libk.md](../design/libk.md) (§4의 각 타입 상세 설계)
**적용 범위**: 커널(`kernel/`)과 시스템 서버(`servers/*`, `libk/`).
포팅된 코드(`libc/`, `userland/`, ADR-005/022)는 원본 프로젝트의
컨벤션을 따르며 이 문서의 적용 대상이 아니다.

이 문서는 실제 코드 작성이 시작되면서 계속 갱신되는 living
convention이다 — 지금은 ADR-003/010/042에서 이미 정해진 규칙을
구체적인 코드 형태로 옮겨 적은 최초 버전이다.

## 1. 언어 부분집합 (ADR-003, ADR-010)

- 컴파일 플래그: `-ffreestanding -fno-exceptions -fno-rtti
  -fno-stack-protector`.
- 허용: freestanding이 보장하는 표준 헤더 — `<cstdint>`, `<cstddef>`,
  `<type_traits>`, `<concepts>`, `<bit>`, `<limits>`, `<atomic>`,
  `<utility>`, `<new>`(placement만), `<cstdarg>`.
- 금지: 예외(`throw`/`try`/`catch`), RTTI(`dynamic_cast`, `typeid`),
  동적 할당을 전제하는 표준 컨테이너(`std::vector`, `std::string`
  등), `<iostream>`류, 전역 정적 객체의 동적 초기화(생성자 순서가
  정의되지 않은 전역 객체 금지 — 필요하면 명시적 `init()` 함수로
  지연 초기화).
- 가상 함수(virtual)는 허용한다 — RTTI 없이도 가상 디스패치는
  freestanding에서 동작한다(단, `dynamic_cast`만 못 쓸 뿐).

## 2. 네이밍 (ADR-042)

- 모든 식별자(타입, 함수, 변수, 네임스페이스, enum 값, 상수)는
  **snake_case**.
- 순수 인터페이스(모든 멤버가 pure virtual)는 `_interface` 포스트픽스:

```cpp
class block_device_interface {
public:
    virtual ~block_device_interface() = default;
    virtual result<void, io_error> read(uint64_t lba, span<uint8_t> out) = 0;
    virtual result<void, io_error> write(uint64_t lba, span<const uint8_t> in) = 0;
};
```

- 컴파일 타임 상수는 `k_` 프리픽스 + snake_case: `k_max_order`,
  `k_message_registers`.
- 매크로는 최대한 피하고 `constexpr`/`enum class`로 대체한다. 불가피한
  매크로(조건부 컴파일 등)는 `MINICORE_` 프리픽스 + SCREAMING_SNAKE_CASE.
- 파일명은 snake_case, 헤더 확장자는 `.hpp`, 구현 확장자는 `.cpp`.

## 3. 에러 처리 관례 (ADR-010)

- 실패할 수 있는 모든 함수는 `result<T, E>`(성공 시 `T`, 실패 시
  전용 `enum class` 에러 타입)를 반환한다. 예: `ipc_error`,
  `alloc_error`, `handle_error`(각 스펙 참고).
- 값이 없을 수 있지만 "실패"는 아닌 경우(예: 선택적 필드)는
  `optional<T>`.
- `[[nodiscard]]`는 강제하지 않고 관례로 권장한다 — 반환값을 버리는
  것이 명백히 의도적인 경우(로깅 등)를 위해 예외를 허용한다.
- 커널 내부에서 "일어나서는 안 되는" 상태(불변식 위반)는 `panic()`
  (커널 정지 + [debug-console.md](debug-console.md) 로그 출력)로
  처리한다 — 이는 `result` 오류 경로가 아니라 버그를 즉시 드러내기
  위한 것이다.

## 4. libk 구성 요소 (ADR-010)

`libk/include/libk/`에 위치하는 커널·서버 공용 프리스탠딩 라이브러리:

| 이름 | 역할 |
|---|---|
| `result<T, E>` | 성공/실패를 함께 표현하는 반환 타입 (§3) |
| `optional<T>` | 값이 없을 수 있는 타입 |
| `span<T>` | 소유권 없는 연속 메모리 뷰 (포인터+길이) |
| `intrusive_list` | 노드가 자신의 링크를 직접 갖는 침습적 연결 리스트 — objects.md의 프록시 트리, memory.md의 free list, scheduler.md의 run_queue가 모두 이것을 사용 |
| `atomic<T>` | `<atomic>` 위에 얹은 커널 스타일 원자적 접근 래퍼 |
| `spinlock` | ADR-033의 "필요한 곳만 최소 락" 원칙에 쓰이는 기본 스핀락 |

## 5. 예시: 이 컨벤션을 따르는 작은 선언

```cpp
namespace mm {

enum class alloc_error : uint32_t {
    out_of_memory,
    quota_exceeded,
    invalid_node,
};

class page_allocator_interface {
public:
    virtual ~page_allocator_interface() = default;
    virtual result<uint64_t, alloc_error> alloc_pages(uint32_t order, uint32_t preferred_node) = 0;
    virtual void free_pages(uint64_t physical_address, uint32_t order) = 0;
};

}  // namespace mm
```

## 아직 정하지 않은 것

- 네임스페이스 분할 규칙(서브시스템당 하나의 네임스페이스인지,
  더 세분화할지)은 실제 코드량이 늘어나면서 정한다.
- `k_` 프리픽스 외의 상수 명명(예: 전역 싱글턴 접근자 이름)은
  구현 시 선례로 정착시킨다.
