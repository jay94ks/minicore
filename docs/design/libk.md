# 설계 결정: libk (커널·서버 공용 프리스탠딩 코어 라이브러리)

**대상 spec**: [cxx-conventions.md](../spec/cxx-conventions.md) §4
**관련 결정**: ADR-010(§4 목록의 근거), ADR-042(네이밍)

[← 설계 문서 색인](index.md)

---

## ADR-066. libk 구성 원칙: 헤더 전용 + 전역 네임스페이스 + 타입당 헤더 1개

- **상태**: 확정 (2026-09-08)
- **결정**:
  - `libk`는 **헤더 전용(header-only)** 라이브러리다 — 모든 타입이 템플릿이거나
    템플릿에 준하는 소품(스핀락 등)이므로 별도 `.cpp` 컴파일 단위가 필요 없다.
    `repo-layout.md`의 "헤더 전용/정적 라이브러리로 취급"이라는 표현 중
    **헤더 전용 쪽으로 확정**한다.
  - 심볼은 **전역 네임스페이스**에 둔다 — `libk::result`처럼 감싸지 않는다.
  - 근거: 기존 spec 문서들(`cxx-conventions.md`, `ipc.md`, `memory.md`,
    `objects.md`, `scheduler.md`)의 코드 예시가 이미 `result<T,E>`,
    `optional<T>`, `span<T>`를 `namespace mm { ... }` 안에서조차
    **아무 접두사 없이** 쓰고 있다 — 이는 이 타입들이 처음부터 전역
    스코프로 전제되었다는 뜻이다. 지금 와서 `libk::` 네임스페이스를
    추가하면 이미 작성된 모든 spec 예시를 소급 수정해야 한다.
  - 파일 구성은 **타입 1개당 헤더 1개**: `libk/include/libk/result.hpp`,
    `optional.hpp`, `span.hpp`, `intrusive_list.hpp`, `atomic.hpp`,
    `spinlock.hpp`, `panic.hpp`(ADR-067). 각 헤더는 self-contained
    (필요한 표준 헤더를 스스로 include, 다른 libk 헤더에 대한 의존은
    최소화 — 예: `optional.hpp`는 `panic.hpp`만 의존).
  - include 경로는 `<libk/result.hpp>` 형태(디렉토리 접두사)를 쓰되, 이는
    **파일 경로 조직 규칙일 뿐 네임스페이스와는 무관**하다 — `klog`처럼
    서브시스템 자체가 네임스페이스인 경우와 구분한다.
- **영향**:
  - 새 libk 타입을 추가할 때도 전역 스코프 + 파일 1개 규칙을 따른다.
  - CMake `libk` 타깃은 `INTERFACE` 라이브러리로 유지(이미 스캐폴딩된 상태와 일치,
    [scaffold-repo-skeleton.md](../done/scaffold-repo-skeleton.md) 참고).

## ADR-067. libk 실행 환경 독립성: panic 훅 추상화

- **상태**: 확정 (2026-09-08)
- **결정**: `libk`는 커널(특권 모드)과 시스템 서버(유저 프로세스, ADR-007/008)
  양쪽에서 링크된다. 두 환경은 "복구 불가능한 상태에서 즉시 정지"의 구현이
  다르다 — 커널은 [debug-console.md](../spec/debug-console.md)의 `klog`로
  로그를 남기고 `hlt`/`wfi` 루프로 정지하지만, 서버는 그런 특권 명령이 없다.
  `libk`는 어느 쪽 구현도 알지 못하므로, **선언만 하고 정의는 링크 시점에
  각 환경이 제공**하는 훅으로 분리한다.

  ```cpp
  // libk/include/libk/panic.hpp
  namespace libk_detail {
      // libk는 이 함수를 선언만 한다. 정의는 kernel/ 또는 servers/ 쪽 링크
      // 대상이 반드시 정확히 하나 제공해야 한다 (링크 타임 계약, weak 심볼 아님 —
      // 정의 누락은 링크 에러로 즉시 드러나야 한다).
      [[noreturn]] void panic_hook(const char* file, int line, const char* msg);
  }

  #define LIBK_PANIC(msg) ::libk_detail::panic_hook(__FILE__, __LINE__, (msg))
  ```

- **근거**: `result<T,E>::value()`를 오류 상태에서 호출하는 등 "일어나서는
  안 되는" 상태(cxx-conventions.md §3)는 `panic()`으로 처리하기로 이미
  정해져 있다. 하지만 그 `panic()`의 실제 동작(콘솔 출력 방식, 정지 방식)은
  환경마다 다르므로 `libk`가 하드코딩할 수 없다.
- **영향**:
  - 커널은 `kernel/core`(또는 arch 계층)에 `panic_hook`을 정의하고
    내부적으로 `klog::printf` + 정지 루프를 호출한다(구체적 연결은
    커널 부팅 계획의 후속 마일스톤에서 다룬다 — 이 ADR은 인터페이스만 고정).
  - 서버는 자신의 프로세스 서버 클라이언트 계층에서 `panic_hook`을 정의하고
    (예: 표준 에러 출력 + `exit`/`abort` 계열 syscall) 구현한다 — 구체적
    구현은 procsrv/libc 설계 시점으로 미룬다.
  - `libk` 자체의 호스트 네이티브 단위 테스트(ADR-077)를 돌리려면
    테스트 바이너리도 `panic_hook`을 정의해야 한다(예: 테스트 실패로 변환).

## ADR-068. `result<T, E>` 설계

- **상태**: 확정 (2026-09-08)
- **결정**: 성공 값 `T` 또는 실패 값 `E` 중 정확히 하나를 보관하는
  태그드 유니온으로 직접 구현한다(`<variant>`는 §1 허용 헤더 목록에
  없으므로 사용하지 않는다).

  ```cpp
  template <typename T, typename E>
  class result {
  public:
      static result ok(T value);
      static result err(E error);

      bool is_ok() const;
      bool is_err() const;
      explicit operator bool() const { return is_ok(); }

      T& value();              // is_err()이면 LIBK_PANIC
      const T& value() const;
      E& error();               // is_ok()이면 LIBK_PANIC
      const E& error() const;

      T value_or(T fallback) &&;   // is_err()이면 fallback을 move해서 반환

      result(result&&) = default;
      result(const result&) = default;   // T, E가 둘 다 복사 가능할 때만 (requires)
      ~result();
  private:
      alignas(T, E) unsigned char storage_[...];   // 수동 배치 new/명시적 소멸
      bool has_value_;
  };

  // T = void 특수화: 값 저장 없이 E + has_value_ 플래그만 유지.
  template <typename E> class result<void, E>;
  ```

  - 저장은 `alignas` 고정 버퍼 + placement `new`로 수동 관리하고,
    소멸자에서 `has_value_`를 보고 `T`/`E` 중 실제로 존재하는 쪽만 명시적으로
    소멸시킨다(둘 다 trivially destructible이면 소멸자 자체가 no-op으로
    최적화되도록 `if constexpr`로 분기).
  - 복사 생성자는 `T`, `E`가 둘 다 복사 가능(`is_copy_constructible`,
    `<type_traits>` 사용)할 때만 활성화한다 — 핸들처럼 이동 전용인 성공값도
    많으므로(objects.md) 이동은 항상 지원한다.
  - `value()`/`error()`를 잘못된 상태에서 호출하면 ADR-067의
    `LIBK_PANIC`으로 즉시 정지한다 — 조용한 UB를 만들지 않는다
    (ADR-001의 "정확성 우선" 원칙).
  - `[[nodiscard]]`는 클래스 자체에 붙이는 것을 권장하되 ADR-010처럼
    강제하지는 않는다(로깅 등 명백히 의도적으로 버리는 경우 허용).
- **근거**: freestanding에서 쓸 수 있는 대체재(`std::variant`,
  `std::expected`)가 §1 허용 헤더 목록 밖이므로 직접 구현이 유일한 선택지다.
  이동 전용 지원은 objects.md의 핸들 소유권 이전과 직접 맞닿아 있다.
- **영향**: 모든 실패 가능 함수(ipc.md, memory.md, objects.md, scheduler.md의
  `sys_*`)가 이 타입을 바로 쓸 수 있어야 하므로, `libk/include/libk/result.hpp`가
  M1 이전에 존재해야 한다(kernel-bootstrap.md M3에서 이미 "선행 조건"으로 명시됨).

## ADR-069. `optional<T>` 설계

- **상태**: 확정 (2026-09-08)
- **결정**: `result<T,E>`와 동일한 수동 저장 기법을 쓰되 실패값 없이
  `bool has_value_` 플래그만 둔다(`<optional>`도 §1 허용 목록 밖이므로
  직접 구현).

  ```cpp
  template <typename T>
  class optional {
  public:
      optional();                       // 빈 상태
      optional(T value);
      static optional none() { return optional(); }

      bool has_value() const;
      explicit operator bool() const { return has_value(); }

      T& value();                       // 빈 상태면 LIBK_PANIC
      const T& value() const;
      T value_or(T fallback) &&;

      template <typename... Args>
      T& emplace(Args&&... args);       // 기존 값 있으면 먼저 소멸 후 재구성
      void reset();

      ~optional();
  private:
      alignas(T) unsigned char storage_[sizeof(T)];
      bool has_value_ = false;
  };
  ```

- **근거**: cxx-conventions.md §3이 이미 "값이 없을 수 있지만 실패는 아닌
  경우"를 `optional<T>`로 못박아 두었다. `result<T,E>`와 저장 기법을
  통일하면 두 타입의 버그(소멸자 누락 등)를 같은 방식으로 검증할 수 있다.
- **영향**: `result.hpp`와 `optional.hpp`가 공통 저장 패턴을 공유하지만,
  코드 중복을 피하겠다고 조기에 공통 베이스 템플릿으로 추상화하지는
  않는다 — 두 타입뿐이므로 지금은 각자 직접 구현하고, 세 번째 유사 타입이
  필요해지면 그때 공통화를 판단한다.

## ADR-070. `span<T>` 설계

- **상태**: 확정 (2026-09-08)
- **결정**: 포인터+길이만 갖는 소유권 없는 뷰로 직접 구현한다(`<span>`도
  §1 목록 밖).

  ```cpp
  template <typename T>
  class span {
  public:
      span() = default;
      span(T* data, size_t count);
      template <size_t N> span(T (&arr)[N]);

      T& operator[](size_t i) const;    // 범위 밖 접근은 LIBK_PANIC (경계 검사 항상 수행)
      T* data() const;
      size_t size() const;
      size_t size_bytes() const { return size() * sizeof(T); }
      bool empty() const { return size() == 0; }

      span subspan(size_t offset, size_t count) const;

      T* begin() const;
      T* end() const;
  private:
      T* data_ = nullptr;
      size_t size_ = 0;
  };
  ```

  - `span<uint8_t>` → `span<const uint8_t>`로의 암시적 변환(비-const에서
    const로)은 허용하지만 반대 방향은 금지 — 생성자 오버로드로 처리.
  - 경계 검사는 릴리스 빌드에서도 항상 수행한다(끄는 매크로를 두지 않음) —
    커널 코드에서 범위 밖 접근은 곧 메모리 안전성 문제이므로 성능보다
    정확성을 우선한다(ADR-001).
- **근거**: `block_device_interface::read`(cxx-conventions.md §2 예시)처럼
  IPC로 오가는 원시 버퍼를 표현하는 데 필수적이다. `std::span`과 API를
  최대한 비슷하게 유지해 낯설지 않게 한다.
- **영향**: 없음(다른 libk 타입과 의존 관계 없음, 가장 먼저 완성 가능).

## ADR-071. `intrusive_list` 설계: 멤버 포인터 기반 훅 + 센티널 순환 리스트

- **상태**: 확정 (2026-09-08)
- **결정**: Linux 커널 `list_head` 스타일 — 노드 타입에 침습적으로 박아넣는
  범용 훅(`list_hook`)과, "이 타입의 어느 멤버가 훅인지"를 멤버 포인터로
  지정하는 `intrusive_list<T, Hook>`로 구성한다.

  ```cpp
  struct list_hook {
      list_hook* prev = this;
      list_hook* next = this;
  };

  template <typename T, list_hook T::*Hook>
  class intrusive_list {
  public:
      void push_back(T& node);
      void push_front(T& node);
      void erase(T& node);          // O(1), 리스트 소속 여부는 호출자가 보장
      bool empty() const { return sentinel_.next == &sentinel_; }

      class iterator { /* list_hook* 순회, operator*는 T&로 역변환 */ };
      iterator begin();
      iterator end();
  private:
      list_hook sentinel_;
      static T* node_of(list_hook* h);   // 멤버 포인터 Hook으로 오프셋 역산
  };
  ```

  - **센티널 기반 순환 리스트**를 쓴다(빈 리스트도 `sentinel_.next ==
    &sentinel_`로 표현) — 삽입/삭제에서 "빈 리스트"를 특수 케이스로
    분기할 필요가 없어진다.
  - 한 노드 타입이 **여러 리스트에 동시에 속할 수 있다** — 훅을 여러 개
    멤버로 두고 각각 다른 `Hook` 멤버 포인터로 `intrusive_list`를
    인스턴스화하면 된다(스레드가 run_queue와 wait_queue에 동시에 속하는
    경우 등, scheduler.md).
  - `node_of`는 `Hook` 멤버 포인터로부터 오프셋을 역산해 `list_hook*`를
    `T*`로 되돌린다. 이는 표준이 엄밀히 보장하는 연산은 아니지만(비
    standard-layout 타입에 대한 `offsetof` 확장은 컴파일러 지원 관용구),
    Clang/GCC 모두 이 패턴을 지원하며 리눅스 커널을 포함한 실제 OS
    구현체들이 널리 쓰는 관용구다 — ADR-020에서 이미 Clang/GCC 두 컴파일러만
    지원 대상으로 정했으므로 이식성 우려가 없다.
  - **스레드 안전성 없음** — 락은 `intrusive_list` 내부가 아니라 이를
    감싸는 상위 구조(run_queue, free_list 등)가 ADR-033("필요한 곳만
    최소 락")에 따라 소유한다.
- **근거**: cxx-conventions.md §4가 이미 "objects.md의 프록시 트리,
  memory.md의 free list, scheduler.md의 run_queue가 모두 이것을 사용"이라고
  명시했다 — 이 세 사용처 모두 "하나의 커널 객체가 여러 리스트에 동시에
  속함"이 자연스러운 상황(스레드는 run_queue이자 소속 프로세스의 스레드
  목록에도 속함)이라 멤버 포인터 기반 다중 훅 설계가 CRTP 단일 훅보다 맞다.
- **영향**: `thread`, `page_descriptor` 등 커널 객체 구조체는 필요한 수만큼
  `list_hook` 멤버를 이름 붙여 포함해야 한다(예: `list_hook run_queue_hook;
  list_hook process_threads_hook;`) — 이 네이밍 관례는 실제 구조체를
  작성하는 M3~M5 시점에 선례로 정착시킨다.

## ADR-072. `atomic<T>` 래퍼 설계: 메모리 순서를 함수명에 고정

- **상태**: 확정 (2026-09-08)
- **결정**: `<atomic>`(§1 허용 헤더) 위에 얹되, `std::memory_order`를
  호출부마다 인자로 넘기는 대신 **연산+순서 조합을 메서드 이름에 고정**한다.

  ```cpp
  template <typename T>
  class atomic {
  public:
      T load_relaxed() const;
      T load_acquire() const;
      void store_relaxed(T v);
      void store_release(T v);
      T fetch_add_relaxed(T delta);
      T exchange_acq_rel(T v);
      bool compare_exchange_strong_acq_rel(T& expected, T desired);
      // 필요해지는 조합을 실제 사용처에서 추가한다 — 처음부터 std::memory_order의
      // 6개 값 전 조합을 다 만들지 않는다.
  private:
      std::atomic<T> inner_;
  };
  ```

- **근거**: 커널 동시성 코드에서 메모리 순서는 "실수로 기본값(seq_cst)을
  쓰다가 나중에 성능 문제로 발견"되는 대표적 버그원이다. 순서를 함수명에
  박아 넣으면 호출부만 읽어도 의도가 드러나고, `grep fetch_add_relaxed`처럼
  잘못된 순서 사용처를 찾기도 쉽다(Fuchsia/Zircon의 유사 관용구를 참고했다).
  `std::atomic<T>`을 그대로 노출하지 않고 감싸는 이유는 이것뿐이며, 그 외
  동작(원자성 보장, lock-free 여부)은 전부 표준 구현에 위임한다.
- **영향**:
  - 처음에는 실제로 필요한 조합만 구현하고(위 예시 목록), 새 조합이
    필요해지면 그때 메서드를 추가한다 — 미리 전부 만들지 않는다.
  - `spinlock`(ADR-073)의 내부 상태(`atomic<bool>` 또는 `atomic<uint32_t>`)가
    이 래퍼의 첫 실사용처가 된다.

## ADR-073. `spinlock` 설계: TTAS + irq 제어는 환경별 훅으로 분리

- **상태**: 대체됨 (→ ADR-076)
- **결정**: 두 가지 타입을 제공한다.

  ```cpp
  class spinlock {
  public:
      void lock();     // test-and-test-and-set, pause 힌트로 스핀
      bool try_lock();
      void unlock();
  private:
      atomic<uint32_t> state_;   // 0 = unlocked, 1 = locked
  };

  // 커널 전용 — 인터럽트 컨텍스트와의 데드락을 막기 위해 락 구간 동안
  // 로컬 코어 인터럽트를 비활성화한다.
  class irq_spinlock {
  public:
      void lock();      // libk_detail::arch_irq_save() 후 spinlock::lock()
      void unlock();    // spinlock::unlock() 후 libk_detail::arch_irq_restore(state)
  private:
      spinlock inner_;
  };

  namespace libk_detail {
      // libk는 선언만 한다 — arch 계층(커널 전용)이 정의를 제공한다.
      // 서버는 이 심볼을 정의하지 않으므로 irq_spinlock을 링크할 수 없다
      // (실수로 유저 프로세스 코드가 이걸 쓰면 링크 에러로 즉시 드러난다).
      using irq_state = uintptr_t;
      irq_state arch_irq_save();
      void arch_irq_restore(irq_state);
  }

  template <typename Lock>
  class scoped_lock {
  public:
      explicit scoped_lock(Lock& l) : lock_(l) { lock_.lock(); }
      ~scoped_lock() { lock_.unlock(); }
      scoped_lock(const scoped_lock&) = delete;
  private:
      Lock& lock_;
  };
  ```

  - 알고리즘은 **test-and-test-and-set(TTAS)**: 스핀 중에는 원자적
    read-modify-write 대신 일반 로드로 상태를 확인하다가 unlocked로
    보일 때만 실제 CAS를 시도해 캐시라인 경합을 줄인다. 스핀 사이에는
    `atomic<T>`(ADR-072)에 추가할 아키텍처별 pause 힌트(x86 `pause`,
    aarch64 `yield`)를 호출한다.
  - `irq_spinlock`은 `debug-console.md`의 전역 로그 스핀락처럼 "인터럽트
    핸들러도 잡을 수 있는 락"에 쓴다. `spinlock`(평범한 버전)은 인터럽트가
    이미 비활성화된 구간 안이거나,애초에 인터럽트라는 개념이 없는
    유저 프로세스(서버)에서 쓴다.
  - 링크 계약은 ADR-067의 `panic_hook`과 동일한 패턴 — `libk`는
    `arch_irq_save`/`arch_irq_restore`를 선언만 하고, 커널 arch 계층만
    정의를 제공한다.
- **근거** (2026-09-08 시점, ADR-076으로 대체됨): ADR-033("필요한 곳만
  최소 락")과 debug-console.md §3의 "전역 스핀락 1개로 직렬화" 요구를
  직접 구현하는 최소 단위였다. 처음부터 ticket lock이나 MCS lock 같은
  확장성 있는 락을 만들지 않았던 이유는 M1~M8(kernel-bootstrap.md)이
  애초에 단일 코어로 실행되어(ADR-035) 경합 자체가 없기 때문이었다 —
  하지만 사용자가 세 가지 락을 모두 처음부터 구현하기로 결정해 ADR-076이
  이를 대체한다.

## ADR-076. `spinlock`/`ticket_lock`/`mcs_lock` 3종 모두 구현 + 용도별 적용 기준 (ADR-073 대체, 해결: OPEN-31)

- **상태**: 확정 (2026-09-08)
- **결정**: ADR-073의 TTAS `spinlock`만으로 시작하고 ticket/MCS는
  M9 이후로 미루기로 했던 결정을 뒤집는다 — **세 가지 락을 모두 처음부터
  구현**하고, 각각을 적절한 용도에 적용한다.

  ```cpp
  class spinlock {           // TTAS (ADR-073과 동일한 설계 유지)
  public:
      void lock();
      bool try_lock();
      void unlock();
  private:
      atomic<uint32_t> state_;
  };

  class ticket_lock {        // FIFO 공정성 보장
  public:
      void lock();     // my_ticket = next_.fetch_add_relaxed(1);
                         // now_serving_.load_acquire() == my_ticket 될 때까지 스핀
      void unlock();    // now_serving_.store_release(now_serving_.load_relaxed() + 1)
  private:
      atomic<uint32_t> next_;
      atomic<uint32_t> now_serving_;
  };

  class mcs_lock {           // 각 대기자가 "자기 자신의" 메모리만 스핀 — 캐시라인 경합 최소
  public:
      struct qnode {
          atomic<qnode*> next{nullptr};
          atomic<bool>   locked{false};
      };
      void lock(qnode& my_node);     // 호출자가 스택에 qnode를 들고 있어야 함
      void unlock(qnode& my_node);
  private:
      atomic<qnode*> tail_{nullptr};
  };

  // 인터럽트 컨텍스트와도 경합하는 락은 어떤 락 종류든 이 래퍼로 감싼다
  // (ADR-073의 전용 irq_spinlock 클래스를 범용 템플릿으로 일반화).
  template <typename Lock>
  class irq_safe : private Lock {
  public:
      void lock()   { state_ = libk_detail::arch_irq_save(); Lock::lock(); }
      void unlock() { Lock::unlock(); libk_detail::arch_irq_restore(state_); }
  private:
      libk_detail::irq_state state_;
  };
  ```

  - `spinlock`(TTAS): **기본 선택**. 경합이 드문 짧은 임계구역 —
    엔드포인트별 IPC 대기열 락(ADR-033), 디버그 콘솔 전역 로그 락
    (ADR-037, `irq_safe<spinlock>`로 사용) 등.
  - `ticket_lock`: **공정성(기아 방지)이 필요한 중간 경합** — 예:
    NUMA 노드별 run_queue 락(ADR-034). 같은 노드의 여러 코어가
    경합할 때 특정 코어가 계속 밀리지 않도록 FIFO 순서를 보장한다.
  - `mcs_lock`: **다수/전체 코어가 공유하는 구조**에서 경합이 심할 때 —
    예: 전역 물리 메모리 풀의 노드 간 폴백 경로(ADR-054), M9(ADR-055)
    이후 AP가 늘어났을 때 접근이 몰리는 전역 자료구조. 각 대기자가
    자기 소유의 `qnode`만 스핀하므로 코어 수가 늘어도 캐시라인
    바운싱이 늘지 않는다.
  - `irq_safe<Lock>`은 ADR-073의 `irq_spinlock` 전용 클래스를
    일반화한 것 — 세 락 종류 중 어느 것이든 인터럽트 핸들러와
    경합할 수 있는 문맥에서 이 템플릿으로 감싸 쓴다. `arch_irq_save`/
    `arch_irq_restore`의 링크 계약(커널 arch 계층만 정의 제공)은
    ADR-073과 동일하게 유지한다.
- **근거**: 락 종류별 트레이드오프(TTAS의 단순함, ticket의 공정성,
  MCS의 대규모 경합 확장성)는 잘 알려진 결과이므로, M9까지 미룰
  이유 없이 처음부터 세 가지를 다 준비해두고 각 사용처에서 적절한
  것을 고르는 편이 나중에 락 구현을 교체하는 리팩터링 비용을 없앤다.
  `irq_safe<Lock>`으로 일반화하면 세 락 각각에 대해 "irq 버전"을
  중복 구현할 필요도 없다.
- **영향**:
  - `libk/include/libk/spinlock.hpp`, `ticket_lock.hpp`, `mcs_lock.hpp`,
    `irq_safe.hpp`(공용 `libk_detail::arch_irq_save`/`restore` 선언과
    `irq_safe<Lock>` 템플릿, ADR-066의 "타입 1개당 헤더 1개" 원칙에서
    `scoped_lock`/`irq_safe`처럼 특정 타입에 종속되지 않는 유틸리티는
    별도 공용 헤더에 둔다) 네 헤더가 필요하다.
  - `kernel-scheduler.md`(ADR-034 run_queue)와 `kernel-memory.md`
    (ADR-054 NUMA 폴백)의 실제 락 선택은 코드 작성 시점에 이 ADR의
    기준(공정성 필요 → ticket, 전역·고경합 → MCS)을 따른다 — 두
    설계 문서 자체를 지금 수정할 필요는 없다(아직 코드가 없으므로
    구체적 락 배치는 실제 구현 시점의 선례로 정착시킨다).
  - OPEN-31("다중 코어 실경합 시 TTAS 확장성, ticket/MCS 전환 필요성")은
    이 ADR로 해소된다 — 세 가지를 처음부터 갖추므로 "전환 필요성 판단"
    자체가 의미 없어진다.

## ADR-077. libk 검증 전략: 호스트 네이티브 단위 테스트 + QEMU 경로는 디스어셈블리·map 파일 추적 (해결: OPEN-33)

- **상태**: 확정 (2026-09-08)
- **결정**:
  1. `libk`의 모든 타입은 **호스트 네이티브 컴파일 + 실행**으로 단위
     테스트한다 — QEMU 부팅을 거치지 않는다. 이유는 QEMU 부팅
     경로로 단위 테스트 규모를 감당하려 하면 왕복 주기가 너무
     느려지고 테스트 인프라 자체가 비대해지기 때문이다.
  2. 호스트 테스트 빌드는 커널 전용 플래그(`-ffreestanding` 등) 없이
     개발 머신의 네이티브 툴체인으로 별도 빌드하며, ADR-067의
     `panic_hook`을 "테스트 실패로 변환"하는 테스트 전용 구현으로
     링크한다.
  3. `libk` 코드 중 **QEMU(실제 커널 실행 환경)에서만 검증 가능한
     부분**(`arch_irq_save`/`arch_irq_restore`가 실제로 인터럽트를
     막는지, `mcs_lock`/`ticket_lock`이 실제 다중 코어 경합에서
     기대대로 동작하는지 등)은 단위 테스트 규모를 인위적으로 키우지
     않는다 — 대신 커널 빌드 산출물의 **디스어셈블리 출력**
     (`objdump -d`류)과 **링커 map 파일**을 근거로, 그 로직이 기대한
     코드 경로로 컴파일·배치되었는지를 추적·확인하는 방식으로
     대체한다.
- **근거**: 호스트 네이티브 테스트는 커널 부팅 왕복 없이 훨씬 빠른
  반복 주기를 제공한다. 반면 레지스터 직접 조작이 필요한 커널 전용
  코드는 애초에 호스트에서 재현 불가능하므로, 그 부분까지 단위
  테스트로 흉내 내려 하기보다 "실제로 의도한 코드가 생성·배치됐는가"를
  정적으로(디스어셈블리/맵 파일) 확인하는 편이 현실적이다.
- **영향**:
  - `libk`에 호스트용 테스트 빌드 타깃(예: `libk/tests/`, 크로스
    컴파일 트리와 독립된 네이티브 CMake 빌드)이 추가되어야 한다.
  - `arch_irq_save`/`arch_irq_restore`(ADR-076) 같은 커널 전용 훅은
    호스트 테스트에서 스텁(no-op 또는 호출 횟수 카운터)으로 대체해
    링크한다 — 실제 인터럽트 차단 동작 자체는 이 방식으로는
    검증되지 않는다는 한계를 명시적으로 인지한다.
  - `tools/`에 커널 빌드의 디스어셈블리·map 파일 생성을 표준화하는
    스크립트(예: `tools/dump-kernel.*`)가 필요할 수 있다 — 구체
    도구화는 M1 이후 실제로 추적이 필요해지는 시점에 정한다.

## ADR-118. libk 타입은 전역에서 쓰이려면 constexpr 생성자를 가져야 한다 (ADR-010/ADR-072 보강)

- **상태**: 확정 (2026-09-08)
- **결정**: `docs/plan/kernel-bootstrap.md` M3 구현 중, `atomic<T>`(ADR-072)의
  기본 생성자가 constexpr이 아니었던 탓에 이를 멤버로 둔 `spinlock`
  등을 포함하는 전역 변수(예: 슬랩 힙의 크기 클래스별 상태 배열)가
  "동적 초기화가 필요하다"고 컴파일러가 판단해, 실행되지 않는
  `.init_array`(C++ 전역 생성자 호출 목록 — 이 freestanding 빌드에는
  이를 호출하는 crt0가 없다)에 초기화를 떠넘기는 것을 확인했다. 그
  결과 0이 아니어야 할 필드(청크 크기 32 등)가 `.bss`의 0으로 남아
  나눗셈 예외(#DE)로 트리플 폴트가 났다 — QEMU로 실제 재현·확인함.
  원인은 `atomic<T>`가 `mutable T value_` + 암시적(비constexpr)
  생성자를 썼기 때문이다. `atomic<T>`의 생성자를 `constexpr`로
  명시하고 `mutable` 대신 `const_cast`(load 계열 const 메서드에서)로
  바꿔 해결했다 — 이제 `atomic`/`spinlock`/`ticket_lock`/`mcs_lock`을
  포함하는 전역은 상수 초기화(컴파일 타임, `.data`/`.bss`에 값이 직접
  박힘)로 처리되어 crt0 없이도 올바른 초기값을 갖는다.
  **앞으로 libk에 추가하는 모든 타입은 전역 변수에 담겨도 안전하도록
  생성자를 constexpr로 만들어야 한다** — 이는 cxx-conventions.md §1의
  "전역 정적 객체의 동적 초기화 금지"를 문자 그대로 지키기 위한
  필요조건이다.
- **근거**: `result`/`optional`도 저장 기법(alignas 버퍼 + placement
  new)이 비슷해 같은 함정에 빠지기 쉽지만, 이 둘은 지금까지 지역
  변수·반환값으로만 쓰였고 전역으로 선언된 적이 없어 이번에
  실제로는 드러나지 않았다 — 잠재적으로 같은 문제가 있을 수 있다는
  점은 인지해 둔다(전역으로 선언되는 순간 같은 방식으로 검증 필요).
  `mutable` 자체가 항상 상수 초기화를 막는 것은 아니지만, 이번 사례는
  `mutable` 제거 + 명시적 `constexpr`로 문제가 사라지는 것을 실제
  빌드 산출물(`_GLOBAL__sub_I_*` 심벌 소멸을 `nm`으로 확인)로 검증했다.
- **영향**:
  - `libk/include/libk/atomic.hpp`가 `mutable` 대신 `const_cast`
    기반으로 바뀌었다 — API·의미론은 동일하게 유지된다.
  - 앞으로 libk에 전역으로 쓰일 가능성이 있는 타입을 추가할 때마다
    (M4~M6의 objects/sched/ipc 자료구조가 유력한 후보), 빌드 후
    `nm <elf> | grep GLOBAL__sub_I`로 숨은 동적 초기화 요구가
    생기지 않았는지 확인하는 습관을 들인다 — 이 ADR이 그 확인
    방법의 근거를 남긴다.
  - kernel/core/mm(`g_node_pools`, `g_cpu_caches`, 크기 클래스 배열)과
    kernel/core/klog.cpp(`g_log_lock`)는 이 수정 이전에도 우연히
    관찰 가능한 오류가 없었다 — 그 필드들의 "의도한 초기값"이
    마침 전부 0/nullptr이라 `.bss` 제로 초기화와 우연히 일치했기
    때문이다(스핀락의 "unlocked"가 0인 것 등). `size_class_state.chunk_size`
    (0이 아닌 16/32/64/...)만 이 우연에서 벗어나 있어 처음으로
    문제가 드러났다.

## 아직 정하지 않은 것

- **OPEN-32**: `result<T,E>`/`optional<T>`의 `[[nodiscard]]` 강제 여부 최종
  확정 시점 — ADR-010은 "관례로 시작"이라고만 했고 이 문서도 그 입장을
  유지한다. 실제 코드량이 늘어나 버려지는 반환값이 버그를 낸 사례가
  나오면 강제로 전환한다.
