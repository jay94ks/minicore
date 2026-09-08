# 완료 보고: kernel-bootstrap M4 — 객체/핸들 테이블

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M4
**관련 스펙**: [objects.md](../spec/objects.md), [scheduler.md](../spec/scheduler.md) §2
**관련 결정**: ADR-002, 011, 016, 023, 029, 032, 042, 063, 078, 085, 118
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M4도 M3처럼 "목표: ... 출력" 형태의 단일 완료 기준 문장이 없다 —
"핸들 테이블/프록시 트리 + cascade revoke 절차"와 "페이지테이블
조작 API" 구현이 완료 기준이다. 둘 다 QEMU 왕복으로 확인했다.

```
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => 14개 항목 모두 PASS (M1~M3 7개 + M4 7개: 프록시 위임/cascade
#    revoke/이중 close, 페이지테이블 map 거부/protect/unmap)
```

## 수행한 작업

### 1. kernel/core/object — 핸들 테이블 + 프록시 트리 (objects.md §1~3, §5~6)

- [kernel/core/object/handle_table.hpp](../../kernel/core/object/handle_table.hpp)/[.cpp](../../kernel/core/object/handle_table.cpp) —
  `object_kind`, `handle_entry`(spec 구조체 그대로 + cascade revoke에
  필요한 `owner_table`/`self_handle` 역참조 추가), `handle_table`
  클래스: `create_owner`, `create_proxy`(§4의 IPC `handles[]` 처리
  로직을 IPC 없이 직접 호출 가능한 형태로), `close`(§6 cascade
  revoke), `handle_info`, `debug_entry`.
- [kernel/core/object/kernel_objects.hpp](../../kernel/core/object/kernel_objects.hpp) —
  `address_space`(trusted, confinement_tier, page_table_root)와
  `thread`(scheduler.md §2의 `thread_sched_fields` 임베드 +
  run_queue용 `list_hook`).
- **"시스템 콜"이라는 이름의 재해석**: objects.md §5는
  `sys_handle_close`/`sys_handle_info`를 시스템 콜로 부르지만, 아직
  유저모드/IDT/트랩 진입점이 전혀 없다(M8 이후 범위) — 지금은 평범한
  커널 내부 함수로 구현했다. 실제 syscall ABI 연결은 유저모드가
  실제로 생기는 마일스톤의 몫이다.
- **§4 4단계(super badge의 jail/guest 유입 차단)는 구현하지 않았다** —
  `identity_badge`(ADR-084)와 confinement 판별 로직이 security-model.md
  영역이라 M1~M8 범위를 명백히 벗어난다. 나머지 1~3, 5~7단계(유효성
  검사, 권한 축소, 깊이 제한, 트리 등록, 반환)는 구현했다.
- **알려진 단순화**:
  - 핸들 슬롯은 재사용하지 않는다 — `close()`된 슬롯도 `in_use`는
    계속 `true`로 남는다. `handle_error::already_closed`를 정확히
    구분하려면 이게 가장 단순했고, 골격 상한(`k_max_handles=64`)
    범위의 데모/자체 테스트에서는 문제가 안 된다. 실제 프로세스가
    오래 살아남는 시점에는 재사용(free-list)이 필요해질 것이다.
  - 소유 핸들을 닫아도 **객체 자체(스레드/주소공간)는 파괴하지
    않는다** — 스레드 종료가 스케줄러(M5)에서 run_queue 제거를,
    주소공간 파괴가 페이지테이블 전체 해제를 요구하는데 둘 다
    아직 실제로 연결되지 않았다. `close()`는 트리 전체를
    무효화(`valid=false`)하는 것까지만 하고 메모리 회수는 이후
    마일스톤으로 미뤘다 — 코드 주석에 명시.
  - `k_max_proxy_chain_depth`(ADR-032, 기본 64)는 상수로 고정했다 —
    이를 조정할 커널 설정 syscall이 아직 없다.
  - 프록시의 `badge` 값을 "재위임 가능한 마스터 캐패빌리티"에서만
    지정하도록 강제하는 검사(objects.md §3)는 아직 없다 — 신원
    인코딩(ADR-084)이 없어 지금은 호출자가 넘기는 플래그
    (`has_badge_override`)로만 구분한다.

### 2. kernel/arch/x86_64/page_table — 페이지테이블 조작 API

- [kernel/arch/x86_64/page_table.hpp](../../kernel/arch/x86_64/page_table.hpp)/[.cpp](../../kernel/arch/x86_64/page_table.cpp) —
  임의 주소공간(PML4)에 대한 `map_page`/`unmap_page`/`protect_page`/
  `query_page`, 그리고 `create_address_space_root()`(새 PML4를
  만들고 커널 higher-half 매핑(`physmap`+커널 이미지, M1의
  `pml4[256]`/`pml4[511]`)을 그대로 복사 — 어떤 주소공간으로 CR3를
  바꿔도 커널 코드/데이터는 계속 접근 가능해야 하므로). 중간
  레벨(PDPT/PD/PT)이 없으면 `mm::alloc_pages(order=0)`로 새로 만든다.
- **COW(ADR-016) "자료구조 골격"에 대하여**: 실제 COW(쓰기 폴트 처리,
  프레임 참조 카운트)는 페이지 폴트 핸들러(IDT, M4 범위 밖)와
  프레임별 참조 카운트 저장소(M3가 "free 블록에만 메타데이터를 둔다"고
  선택해 아직 없음, kernel-bootstrap-m3.md 참고) 둘 다 필요해 이번엔
  만들지 않았다 — 대신 **COW clone이 실제로 구현될 때 쓸 프리미티브
  (`map_page`로 같은 물리 페이지를 read-only로 자식에게 매핑,
  `protect_page`로 부모 쪽도 read-only로 낮춤)를 준비하는 것으로
  이 항목을 해석**했다. 눈에 보이는 "COW용 구조체"를 별도로 만들지
  않은 이유는, 아직 아무것도 호출하지 않는 빈 구조체/함수를 두는
  것은 "half-finished 구현"이 되어 오히려 정직하지 않다고 판단했기
  때문이다 — 이 판단 근거를 `page_table.hpp` 상단 주석에 남겼다.
- TLB 무효화(`invlpg`)는 구현하지 않았다 — M4는 만든 주소공간을
  실제로 활성화(CR3 전환)하지 않고 `phys_to_virt`로 테이블만
  들여다보므로 당장 관찰 가능한 문제가 없다. 실제 다중 주소공간
  전환이 생기는 시점(M8 이후)에 처리해야 한다.

## 실행 중 재확인한 결정(ADR)

- **ADR-118 재발** — `handle_table`을 처음에 전역(`static`) 변수로
  선언했더니 M3에서 이미 겪은 것과 같은 증상(`_GLOBAL__sub_I_*`
  심벌 발생 → crt0가 없어 실행되지 않음 → 필드가 의도한 값 대신
  `.bss` 0으로 남음)이 다시 나타났다 — `nm`으로 확인 후 즉시 고쳤다.
  이번엔 `atomic<T>`를 constexpr로 고치는 이전 해법이 아니라
  **애초에 전역으로 두지 않는** 쪽을 택했다: `handle_table`은
  64개 항목마다 `intrusive_list` 센티널을 포함해 상당히 크고
  (스택에 두기엔 8KiB 부트 스택 예산도 빠듯하다), `mm`이 이미
  초기화된 뒤 실행되는 함수 안에서 `mm::alloc_pages` +
  placement new로 만들면 "정적 초기화가 필요한지"라는 질문 자체가
  성립하지 않는다(진짜 런타임 코드로 생성되므로) — ADR-010이 원래
  권장한 "명시적 init 함수로 지연 초기화" 그 자체다. `nm | grep
  GLOBAL__sub_I`가 비어 있음을 재확인했다.
- 이 패턴(생성자가 복잡한 nested 타입을 전역에 두지 말고, mm 초기화
  이후 시점에 명시적으로 힙에 만들 것)은 앞으로도 재사용할 만한
  선례로 남긴다 — 새 ADR을 추가하지는 않았다(ADR-118이 이미 "매번
  `nm`으로 확인하는 습관을 들인다"고 규정해 두었고, 이번 사례는
  그 규정을 실제로 적용해 문제를 예방한 사례이지 새로운 정책 결정이
  아니기 때문이다).

## 검증 결과 (정직하게 보고)

- **확인함**: `create_owner` → `create_proxy`(권한 0b111→0b001 축소
  확인) → `close(owner)` → 프록시가 **다른 handle_table에 있음에도**
  cascade revoke로 무효화됨 → 재-`close`가 `already_closed`를
  정확히 반환함, 전부 QEMU 실행으로 확인(반복 실행 시 동일 결과).
- **확인함**: `map_page` → 같은 주소 재매핑 시 `already_mapped` 거부
  → `query_page`로 실제 물리주소·권한 비트(write/user/exec) 확인 →
  `protect_page`로 write 비트 제거 확인 → `unmap_page` 후
  `present=0` 확인.
- **확인하지 못함**: 프록시 체인 깊이 제한(`k_max_proxy_chain_depth=64`)
  — 데모가 위임을 1단계만 하므로 한도 초과 케이스를 실행으로
  검증하지 않았다. 코드 리뷰 수준(단순 정수 비교)으로만 확인.
- **확인하지 못함**: 새로 만든 주소공간을 실제로 활성화(`mov cr3,
  reg`)했을 때 커널 코드가 계속 정상 실행되는지 — `create_address_space_root`가
  복사한 higher-half 엔트리가 "이론상 맞다"는 것만 확인했고, 실제
  CR3 전환은 시도하지 않았다(다음 실패 시 트리플 폴트로 이어질
  위험이 있어 M4 범위에서는 시도하지 않기로 판단 — page_table.hpp
  주석 참고).
- 스모크 테스트 타임아웃을 5초 → 12초로 올렸다 — M4에서 출력 줄
  수가 늘어나며 QEMU 실행 속도 편차(이 개발 머신의 부하에 따라)로
  5~8초에서 간헐적으로 출력이 잘리는 것을 실제로 관찰했다(로그가
  중간 줄에서 끊김). 12초로는 반복 실행에서 안정적으로 통과함을
  확인했다.

## 다음 마일스톤과의 접점

- M5(스케줄러)가 `object::thread`의 `run_queue_hook`을 실제
  `run_queue`(intrusive_list)에 연결하고, `thread_sched_fields`를
  실제로 스케줄링에 사용하기 시작한다.
- M6(IPC)가 `handle_table::create_proxy`를 `sys_call`/`sys_reply`의
  `handles[]` 처리 경로에서 재사용한다 — 이번에 만든 함수 시그니처가
  이미 그 재사용을 염두에 두고 있다(단일 handle 단위 처리, in/out
  느낌의 반환값).
- COW의 실제 구현(쓰기 폴트 처리, 프레임 참조 카운트)은 페이지
  폴트 핸들러(IDT)가 생기는 시점과 mm의 프레임 메타데이터 모델이
  확장되는 시점 둘 다가 선행 조건이다 — 둘 중 어느 마일스톤에서
  먼저 다룰지는 아직 정해지지 않았다.
- 핸들 슬롯 재사용(free-list)과 소유 핸들 close 시 실제 객체 파괴는
  실제 프로세스 생명주기가 등장하는 시점(procsrv, M8 이후)에
  재검토해야 한다.
