# mmap 서브시스템 및 Maple Tree 자료구조 — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-2AAD7C8D
  status: approved
  updatedAt: 2026-09-16T08:32:19.882Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# mmap 서브시스템 및 Maple Tree 자료구조 — 설계 제안

설계자 지시(2026-09-14, 메시지 3건, 모두 RM-9B8CA541에 대한 후속) -
"가상 주소 관리자는 프로세스별 주소 할당을 담당하는 파트와 커널 영역만을
담당하는 파트로 나눠야 해. 왜냐하면, 커널 영역은 SMP에서 모든 CPU가
공통된 메모리를 봐야 하거든.", "Maple Tree를 구현할 것.
(https://www.minzkn.com/linuxkernel/pages/maple-tree.html)",
"mmap 서브시스템을 지금 설계하도록 해." **이 문서는 RM-9B8CA541("Minicore
가상주소 공간 관리자 위임 사항")이 미뤄 뒀던 mmap 서브시스템 설계를
실제로 수행한 것이다 - RM-9B8CA541은 이 문서로 대체/흡수되어 종료된다
(그 문서의 "이 위임이 끝나는 조건" 항목이 정확히 이 상황이다).**

**[확장, 2026-09-14, 설계자 후속 지시]** - "표준 파일 API들도 모두
설계에 포함시켜." §9에서 open/close/read/write/lseek/stat/readdir/
mkdir/unlink 전체를 다룬다 - VFS 라우팅(SP-7CC5693A §2.2의
`ResolvePathArgs`)과 이 문서의 프로세스 자료구조를 엮는 지점이다.

**[갱신, 2026-09-14, 설계자 후속 지시]** - "메모리 절약이 실제로
필요하지 않더라도 maple_dense/maple_leaf_64류 구분을 미리 추가해놔."
§3.1이 처음 제안했던 "v1은 단일 노드 타입으로 통일" 단순화를 철회하고,
Linux 원본과 동일하게 4가지 노드 타입 + 포인터 태깅을 처음부터
설계에 포함시켰다.

## 1. 왜 두 파트로 나누는가

설계자 지적대로, 이 프로젝트의 주소공간 구조(`Paging::createAddressSpace`,
SP-68182FBD)는 **PML4 인덱스 256-511(higher half)을 모든 프로세스가
공유**한다 - 인덱스만 복사할 뿐 그 아래 PDPT/PD/PT 물리 페이지 자체는
전부 같은 것을 가리킨다. 즉:

- **커널 영역(higher half) 매핑을 하나 바꾸면 - 새 페이지를 매핑하든,
  권한을 바꾸든 - 그 순간 그 프로세스 뿐 아니라 시스템의 다른 모든
  프로세스, 그리고 이미 각자 다른 CPU에서 실행 중인 다른 코어들도
  전부 그 변경을 보게 된다.** 이건 인덱스 공유의 자연스러운 결과이자
  동시에 위험 요소다 - 여러 코어가 동시에 커널 영역 주소를 할당/해제
  하면 전역적으로 한 곳(자료구조 자체와, 각 코어의 TLB 캐시)에서
  경합이 생긴다.
- **유저 영역(lower half, 인덱스 0-255) 매핑은 프로세스마다 완전히
  독립**이다 - 프로세스 A가 자기 유저 영역에 뭘 매핑하든 프로세스
  B나 다른 코어의 TLB에 전혀 영향이 없다(그 프로세스를 스케줄링한
  코어의 TLB만 관련).

따라서 자료구조는 같아도(§3의 Maple Tree) **인스턴스 개수와 동시성
보호 방식이 근본적으로 다르다** - 이것이 두 파트로 쪼개는 이유다.

## 2. 전체 구조

```
[ProcessAddressSpaceManager]  - Process마다 하나(인스턴스 N개, N=프로세스 수)
        내부: Maple Tree 1개 - 이 프로세스의 유저 영역(lower half) VMA
        동시성: Spinlock 1개(프로세스당) - v1은 프로세스당 스레드 1개
                뿐이라 사실상 경합 없음(SP-04EE2A18 각주 - 멀티스레드
                프로세스는 후속 과제), 락은 미래를 대비한 최소 방어.

[KernelAddressSpaceManager]   - 시스템 전체에 **딱 하나**(싱글턴)
        내부: Maple Tree 1개 - 커널 영역(higher half) 중 "동적 할당
              가능"으로 정한 하위 범위의 VMA(§5 참고 - 부팅 시 고정
              배치된 direct map/커널 이미지/스택 풀 등은 이 관리자
              범위 밖, 건드리지 않음).
        동시성: **전역 Spinlock 1개** - 모든 CPU가 이 자료구조에 대해
                직렬화된다(설계자 지적의 핵심 - "모든 CPU가 공통된
                메모리를 봐야" 하므로 프로세스별 락으로는 안 되고
                시스템에 하나뿐인 락이어야 한다).
```

### 2.1 `KernelAddressSpaceManager`의 PML4 최상위 엔트리 선점 (QU-4E9F1C36 답변 반영, 2026-09-15)

**비판적 재검토로 발견된 잠복 버그**: `Paging::createAddressSpace()`
(SP-68182FBD §1.1)는 프로세스 생성 "시점"의 현재 PML4에서 상위 절반
(인덱스 256~511)을 엔트리째로 **한 번만** 스냅샷 복사한다 - 라이브
공유가 아니다. 따라서 `kLazyZoneBase`가 속한 PML4 슬롯이 그 어떤
프로세스보다도 먼저 present 상태가 아니면, `KernelAddressSpaceManager::
mapRegion()`의 첫 실제 호출(현재는 소비자가 없어 미발현)이 하필 어떤
유저 프로세스의 CR3 위에서 일어나는 순간 그 프로세스 하나의 PML4에만
매핑이 생기고, 이미 존재하는 다른 프로세스/부팅 PML4(리액터가 쓰는
`gBootPml4Phys`)는 그 매핑을 영원히 모르게 된다.

**확정된 수정 방침(설계자 답변)**:

1. **PML4E 사전 생성** - `KernelAddressSpaceManager::init()` 시점에
   `kLazyZoneBase`가 속한 PML4 슬롯에 대해 미리 하위 페이징 구조체를
   확보한다 - 더미 매핑(`Paging::mapPage`+`unmapPage` 왕복) 또는
   `Paging`에 "PML4E→PDPT만 미리 할당하는" 경량 헬퍼를 두는 방식 모두
   유효하다. `init()`은 이미 모든 프로세스 생성보다 먼저 실행됨이
   보장돼 있다(`TlbShootdown::init()` 직후, 부팅 시퀀스). 이렇게 하면
   이후 `createAddressSpace()`로 파생되는 모든 프로세스가 그 PDPT의
   물리 주소를 스냅샷 시점부터 이미 공유한다.
2. **`mapRegion()`의 타깃 PML4 명시(방어적 보강)** - 1번으로 PML4E가
   선점되면 하위 테이블이 공유되므로 기능상으로는 `Paging::mapPage
   (..., 0)`(현재 CR3)로도 정상 동작하지만, 커널 전역 가상공간을
   관리하는 API가 "현재 활성 CR3"가 아니라 항상 마스터 PML4
   (`gBootPml4Phys`)를 명시적으로 타깃팅하도록(또는 최소한 "이 호출이
   모든 PML4가 공유하는 상위 레벨 테이블만 조작하고 있음"을 코드
   레벨에서 보장하도록) 코드/주석을 보강한다 - 우연히 맞는 동작에
   의존하지 않기 위한 방어적 설계.

**[해소, 2026-09-16, 재확인] PN-1EF2B3B3 completed** - 위 두 수정
사항(PML4E 사전 생성 + `mapRegion()` 타깃 명시)이 실제로 구현
완료됐다. 이 §2.1은 더 이상 열린 항목이 아니다.

## 3. Maple Tree 자료구조 설계

Linux 6.1의 Maple Tree(RB-Tree를 대체한 VMA 관리 구조, 참고:
https://www.minzkn.com/linuxkernel/pages/maple-tree.html)를 이
프로젝트의 freestanding 제약에 맞게 이식한다.

### 3.1 노드 레이아웃 - 4가지 타입 + 포인터 태깅 (2026-09-14 갱신)

**[재확정, 설계자 지시]** 메모리 절약 필요 여부와 무관하게 Linux
원본과 동일한 4가지 노드 타입을 처음부터 둔다 - 노드 전부 256바이트로
고정해 SP-D7013B26(Slab 할당자, 이미 구현 완료)의 256B 버킷을
`GenericSlabAllocator::alloc(256)`/`free(ptr, 256)`으로 그대로
재사용한다.

```cpp
// minicore/libs/libkenv, 가칭 maple_tree.h - freestanding 제약 준수
// (표준 정수 타입 대신 libkenv 관례 타입, <cstring> 없이 reinterpret_cast
// 또는 libkenv/mem.h의 memcpy/memset만 사용).
namespace kernel {

constexpr uint32_t kMapleRangeSlotCount = 16;   // maple_range_64/maple_leaf_64와 동일
constexpr uint32_t kMapleArangeSlotCount = 10;  // maple_arange_64와 동일(gap 배열 자리 확보)
constexpr uint32_t kMapleDenseSlotCount = 31;   // pivot 없이 8B 슬롯만 채운 최대 개수(256B/8B)

// (1) 밀집 노드 - 아주 작은/희소 구간 전용. pivot 배열이 아예 없다 -
// slot[i]가 "이 노드가 담당하는 범위의 i번째 오프셋"을 직접 가리킨다
// (범위 자체는 노드 밖 부모 pivot이 결정). 트리 전체가 작을 때(루트
// 노드가 곧 리프인 경우 등) 가장 메모리 효율적이다.
struct MapleDenseNode {
    void* parent;
    void* slot[kMapleDenseSlotCount];   // NULL=gap
};
static_assert(sizeof(MapleDenseNode) <= 256, "slab 256B 버킷 초과");

// (2) 리프 전용 64비트 범위 노드 - gap 배열 없음(부모 arange 노드가
// 이 서브트리 전체의 gap을 이미 요약해 알고 있다고 가정할 수 있을 때만
// 안전 - §3.1-A 선택 정책 참고).
struct MapleLeaf64Node {
    void* parent;
    uint64_t pivot[kMapleRangeSlotCount - 1];  // 15개 경계값
    void* slot[kMapleRangeSlotCount];          // 값 포인터(Vma* 등), NULL=gap
};
static_assert(sizeof(MapleLeaf64Node) <= 256, "slab 256B 버킷 초과");

// (3) 내부 전용 64비트 범위 노드 - gap 배열 없음, 자식 포인터만 보관.
struct MapleRange64Node {
    void* parent;
    uint64_t pivot[kMapleRangeSlotCount - 1];  // 15개 경계값
    void* slot[kMapleRangeSlotCount];          // 자식 노드 포인터(태깅된 포인터, §3.1-B)
};
static_assert(sizeof(MapleRange64Node) <= 256, "slab 256B 버킷 초과");

// (4) gap 탐색용 확장 노드 - 슬롯 수는 적지만(10개) gap 배열을 갖는다.
// findGap(§3.3)이 서브트리 단위로 건너뛸 수 있게 해 주는 핵심 노드.
struct MapleArangeNode {
    void* parent;
    uint64_t pivot[kMapleArangeSlotCount - 1];  // 9개 경계값
    void* slot[kMapleArangeSlotCount];          // 10개 - 자식(내부) 또는 값(리프)
    uint64_t gap[kMapleArangeSlotCount];        // 각 슬롯 서브트리의 최대 연속 gap 크기
    uint16_t maxGapSlot;                        // 가장 큰 gap을 가진 슬롯 인덱스(meta 역할)
};
static_assert(sizeof(MapleArangeNode) <= 256, "slab 256B 버킷 초과");

}  // namespace kernel
```

### 3.1-A. 어느 노드를 언제 쓰는가 (선택 정책, v1 제안)

- **루트를 포함한 모든 내부 노드는 `MapleArangeNode`로 시작한다** -
  이 트리의 존재 이유 자체가 `findGap`(mmap의 빈 주소 탐색)이므로,
  gap 정보가 트리 어느 깊이에서든 끊기면 안 된다 - 내부 노드에서
  `MapleRange64Node`(gap 없음)로 전환하는 것은 "그 서브트리 전체가
  꽉 차서 gap이 0으로 고정됐다"는 것이 확실할 때만 안전한 최적화다.
- **리프 노드**: 대부분의 경우도 `MapleArangeNode`를 그대로 쓴다(리프도
  "이 리프 안에 아직 빈 슬롯이 있는지"를 gap으로 표현할 수 있어야
  하므로). `MapleLeaf64Node`/`MapleRange64Node`는 "이 리프가 이미
  꽉 찼다"고 확정된 뒤에만 gap 없는 형태로 다운그레이드하는 최적화로
  제안한다.
- **`MapleDenseNode`**: 트리 전체가 한 노드에 들어갈 만큼 작을 때
  (루트 = 리프)만 사용 - 프로세스 하나가 처음 몇 개의 VMA만 가진
  전형적인 초기 상태에 해당한다.
- **정확한 전환/다운그레이드 타이밍**(예: 언제 Arange→Range64로
  바꾸는지)은 이번 문서에서 알고리즘 레벨까지 확정하지 않는다 - 노드
  "종류"는 여기서 전부 정의됐고, 실제 전환 휴리스틱은 구현 착수
  시점에 실측하며 다듬는 것을 제안한다(RM-23F4B687 §4 취지 - 숫자/
  전략 튜닝).

### 3.1-B. 포인터 태깅

Linux처럼 256B 정렬(하위 8비트가 항상 0)을 이용해 포인터 자체에
노드 타입을 인코딩한다 - `slot[]` 배열의 각 항목(자식을 가리킬 때)과
`MapleTree`가 들고 있는 루트 포인터 모두 이 태깅된 형태로 저장한다:

```cpp
enum class MapleNodeType : uintptr_t { Dense = 0, Leaf64 = 1, Range64 = 2, Arange64 = 3 };

inline MapleNodeType kMapleNodeType(void* tagged) {
    return static_cast<MapleNodeType>(reinterpret_cast<uintptr_t>(tagged) & 0x3);
}
inline void* kMapleNodePtr(void* tagged) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(tagged) & ~uintptr_t(0x3));
}
inline void* kMapleTagNode(void* raw, MapleNodeType type) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(raw) | static_cast<uintptr_t>(type));
}
```

`MapleTree`의 모든 내부 순회 함수(§3.3)는 자식 포인터를 역참조하기
전에 `kMapleNodeType()`으로 분기해 올바른 구조체로 캐스팅한다 -
값 포인터(리프의 `Vma*` 등)는 태깅하지 않는다(리프 노드 자신의
타입으로 이미 "이 slot이 값인지 자식인지"가 구분되므로 이중 태깅
불필요).

### 3.2 RCU 없음 - v1은 전역/프로세스별 락으로 대체 (중요, 설계자 확인 필요)

Linux의 Maple Tree는 **RCU 기반 lock-free 읽기**가 핵심 이점 중
하나다(§ Linux 통합 참고 - `rcu_read_lock`/`call_rcu`로 쓰기가 읽기를
막지 않음). **이 프로젝트는 RCU 인프라 자체가 없다**(quiescent state
추적, grace period, `call_rcu` 콜백 큐 등 - 전부 상당한 규모의 별도
서브시스템) - 처음부터 이걸 같이 설계하는 것은 이번 mmap 서브시스템의
범위를 크게 벗어난다.

**이 문서의 제안**: v1은 Maple Tree의 **자료구조/알고리즘(노드 분할,
gap 탐색)만 그대로 가져오고, 동시성 보호는 RCU 대신 §2의 Spinlock으로
대체**한다 - 읽기든 쓰기든 락을 잡는다(page fault 경로에서 읽기가
잦다는 점에서 RCU 대비 확실히 손해지만, 지금 이 프로젝트엔 페이지
폴트 자체가 아직 없고(§2-B 정책만 있고 구현 전), 프로세스당 스레드도
하나뿐이라 실제 경합이 거의 없다). **RCU 자체를 이 프로젝트에 도입할지
는 별도 설계 질문으로 남긴다**(§6-1 - 성능이 실제로 문제가 되는
시점까지는 락 기반으로 충분하다고 판단했으나, 이건 숫자 튜닝이 아니라
아키텍처 결정이라 확정 짓지 않고 열어 둔다).

### 3.3 핵심 연산

```cpp
namespace kernel {

// 값은 전부 void* - 리프에서는 사용자 데이터(§4의 Vma*, 태깅 없음),
// 내부에서는 §3.1-B로 태깅된 자식 노드 포인터.
class MapleTree {
public:
    void init();

    // [start, end] 범위에 value를 등록한다(겹치면 실패 - 호출부가
    // 먼저 findGap 등으로 빈 공간을 확인했다는 전제).
    bool store(uint64_t start, uint64_t end, void* value);

    // addr을 포함하는 범위를 찾는다(없으면 nullptr 반환 - "gap 안"이라는 뜻).
    void* find(uint64_t addr, uint64_t* outRangeStart, uint64_t* outRangeEnd);

    // size 이상인 연속 gap을 [searchFloor, searchCeil] 범위 안에서 찾는다
    // (§3.1의 gap 배열로 서브트리 단위 스킵 - mmap(hint=0) 구현의 핵심).
    bool findGap(uint64_t searchFloor, uint64_t searchCeil, uint64_t size,
                 uint64_t* outStart);

    // [start, end] 범위를 지운다(부분 겹침 - 범위의 일부만 지우는 것도
    // 지원해야 한다, munmap이 매핑의 중간 일부만 해제할 수 있으므로 -
    // Linux도 동일 요구사항).
    bool erase(uint64_t start, uint64_t end);
};

}  // namespace kernel
```

`store`/`erase`의 노드 분할/병합 알고리즘은 Linux 원본의 절차(임시
`maple_big_node` 버퍼에 기존 항목 + 신규 항목을 모아 분할점을 계산해
재분배)를 그대로 따르는 것을 제안한다 - 노드 타입이 4종으로 늘어난
만큼, 분할/병합 시 "이 서브트리가 지금 어느 타입이어야 하는지"를
§3.1-A 정책에 따라 다시 판정하는 단계가 추가된다(RCU가 빠진 것 외의
유일한 알고리즘 차이).

## 4. VMA 값 구조

```cpp
enum class VmaBacking : uint32_t {
    Anonymous,     // 요구 페이징(§2-B), 초기엔 매핑 없음 - 폴트 시 PageFrameAllocator로 채움
    FixedPhysical, // 물리주소가 이미 정해짐(MMIO/DMA 버퍼) - PnP §3.3/DMA 버퍼 관리자(SP-39F18E30)가 여기 해당
    FileBacked,    // §9의 파일 API로 매핑 - 파일 오프셋 범위를 이 VMA에 연결(§9.5)
};

struct Vma {
    uint64_t start, end;      // MapleTree의 키와 중복되지만 값 쪽에서도 바로 알 수 있게 보관(편의)
    uint32_t prot;            // Read/Write/Exec 비트
    VmaBacking backing;
    uint64_t fixedPhysAddr;   // backing==FixedPhysical일 때만 사용
    int32_t backingFd;        // backing==FileBacked일 때만 사용(§9.2의 fd)
    uint64_t fileOffset;      // backing==FileBacked일 때만 사용
    bool used = false;        // Slab 재사용 시 관례
};
```

`Vma` 자체도 256B 버킷보다 훨씬 작으므로 32B/64B 버킷(SP-D7013B26의
다른 버킷)에서 `GenericSlabAllocator`로 확보하는 것을 제안한다.

## 5. mmap/munmap/brk Syscall API

**[구현 완료, 2026-09-16, commit a569f27, RM-48E1E610 17-19]** 아래
스케치 그대로 `minicore/kernel/address_space.h/.cpp`에 구현됐다 -
다만 두 가지가 실제 구현에서 달라졌다: (1) 에러 타입은 `ChannelError`
재사용이 서로 무관한 서브시스템을 섞는 드래프트 단계의 실수로 판단해
`AddressSpaceError`(`{None, OutOfMemory, InvalidArgument, NotMapped}`)
를 새로 만들어 썼다, (2) `Scheduler::currentTask()`가 `onExec`
실행 시점엔 제출자를 가리키지 않는다는 실측 버그(비동기 리액터의
idle-drain 컨텍스트 문제)를 막기 위해 각 Args 구조체에 `Process*
process` 필드를 추가해 제출 시점에 캐시해 둔다. `Process::init()`에
`heapStart`/`heapBrk` 필드(4096B 최소 힙 VMA를 즉시 생성)도 함께
추가됐다. 자세한 배경은 PN-012E8C1A 참고.

```cpp
// 유저 프로세스 → 커널: ProcessAddressSpaceManager(호출자 프로세스 것)를 사용.
struct MmapArgs {
    uint64_t hintAddr;    // in: 0이면 커널이 findGap으로 아무 곳이나 고름
    uint64_t length;      // in: 페이지 단위로 올림
    uint32_t prot;        // in
    uint32_t flags;       // in: v1은 Anonymous만 지원(Fixed 힌트 강제는 후속)
    // out
    uint64_t addr;
    ChannelError error;   // OutOfMemory / InvalidArgument
};

struct MunmapArgs {
    uint64_t addr;
    uint64_t length;
    // out
    ChannelError error;   // NotMapped
};

// brk는 프로세스당 "힙 VMA" 하나를 미리 예약해 두고 그 끝점만 움직이는
// 전통적 구현을 제안한다 - Process::init() 시점에 초기 크기 0으로
// 힙 VMA를 store()해 두고, brk() 호출마다 erase+store로 끝점을 재조정.
struct BrkArgs {
    uint64_t newBrk;      // in: 0이면 "현재 brk 조회"
    // out
    uint64_t currentBrk;
    ChannelError error;   // OutOfMemory(확장 실패 - 다음 VMA와 충돌 등)
};
```

`AllocDmaBuffer`(SP-39F18E30)/`RequestIoPermission`(SP-9DD4F3EA §3.3)은
이제 이 문서의 `ProcessAddressSpaceManager`를 내부적으로 사용하도록
갱신된다(§7 참고) - 둘 다 `VmaBacking::FixedPhysical` 종류의 VMA를
등록하는 특수 경우일 뿐이다.

## 6. 아직 열려 있는 설계 영역

1. **RCU 도입 여부** (§3.2) - v1은 락 기반, 필요성이 실제로 나타나면
   (멀티스레드 프로세스 다수 + 페이지 폴트 빈도가 실측으로 문제가
   될 때) 별도 아키텍처 결정으로 재검토 - 설계자 판단 필요할 수
   있는 사안이라 숫자 튜닝이 아니라고 명시해 둔다.
2. ~~**커널 영역 TLB 샷다운**~~ - **해결됨(SP-DE19BB1C, 2026-09-14)**
   - IPI 기반 우편함+ISR+ACK 메커니즘으로 구체화됐다(신규
   `Lapic::sendFixedIpi` 포함). `KernelAddressSpaceManager`의 매핑
   해제/변경 경로는 이제 이 문서를 그대로 재사용하면 된다 - 더 이상
   이 문서의 미결 사항이 아니다.
3. **`findGap`의 정확한 시작 지점(§5-A/§5의 이전 논의 상속)**: 코드/
   스택 경계를 피해 어디서부터 유저 영역 mmap을 시작할지 - SP-8B6B8D25
   §5-A가 아직 정하지 않은 힙/mmap 경계를 이 문서가 실질적으로 확정
   짓는다: **코드 공간 위쪽 ~ 스택 하단 사이 전부를 mmap 가능 영역으로
   본다**(v1 제안, 정확한 정렬/여유 공간 상수는 구현 시점 튜닝).
4. ~~**파일 백킹 mmap**~~ - **부분 해결(§9.5)** - `fs` 서비스
   (SP-7CC5693A)가 실제로 구현되기 전까지는 실행 가능한 코드가 없지만,
   `VmaBacking::FileBacked`의 자료구조/폴트 처리 흐름 자체는 §9.5에서
   설계했다.
5. **노드 타입 전환/다운그레이드 휴리스틱(§3.1-A)**: 정확히 언제
   `MapleArangeNode`를 `MapleRange64Node`/`MapleLeaf64Node`/
   `MapleDenseNode`로 바꿀지의 알고리즘 - 노드 "종류"는 확정됐으나
   전환 시점은 구현 착수 시 실측하며 다듬는 것으로 열어 둠.

## 7. 선행 조건

- Slab 할당자(SP-D7013B26, 이미 구현 완료) - 256B/32B 버킷 재사용.
- `Spinlock`(`libkenv/spinlock.h`, 이미 구현 완료, PL-65C20380) -
  `KernelAddressSpaceManager`/`ProcessAddressSpaceManager` 동시성 보호.
- **커널 영역 TLB 샷다운 IPI**(§6-2) - **[갱신, 2026-09-15] 완료됨**
  (SP-DE19BB1C/PN-6D33BB03) - `KernelAddressSpaceManager::
  unmapRegion()`이 이미 이 경로를 재사용해 실제로 동작 중이다.
- 프로세스 모델(PN-16CA347D) - **[갱신, 2026-09-15] 완료됨** -
  `ProcessAddressSpaceManager`가 매달릴 `Process` 구조체 자체가
  이미 존재한다.
- `Paging::mapPage`/`unmapPage`(이미 구현 완료) - 실제 페이지 테이블
  조작.
- VFS 커널 서브시스템(SP-7CC5693A) - §9의 파일 API가 경로 해석에
  재사용하는 `ResolvePathArgs`(§9 자체는 MountKind::KernelDriver
  분기 반영 필요, PN-ABD23ACE 참고 - §9 착수 시점에 처리).

**[갱신, 2026-09-15] §2/§3(KernelAddressSpaceManager/
ProcessAddressSpaceManager/Vma/VmaBacking, Maple Tree 멀티레벨
분할/병합)은 PN-012E8C1A(1차 증분) + PN-38D17292(멀티레벨 노드
분할)로 이미 구현 완료됐다** - 위 선행 조건 전부가 충족된 뒤 실제로
착수돼 `minicore/kernel/address_space.h/.cpp`에 반영됐다. 이 §7이
가리키던 "아직 없음" 상태는 더 이상 유효하지 않다.

**[갱신, 2026-09-16]** §5(mmap/munmap/brk syscall API)도
commit a569f27로 구현 완료됐다(위 §5 참고) - 남은 건 §9(표준 파일
API, MountKind::KernelDriver 분기 필요)뿐이다. §6 항목 4(파일 백킹
mmap)도 §9와 같은 전제(fs 서비스)에 묶여 있다.

## 8. RM-9B8CA541과의 관계

RM-9B8CA541("Minicore 가상주소 공간 관리자 위임 사항")은 "mmap
서브시스템은 지금 안 만든다"는 결정을 기록한 문서였다 - 이번 설계자
지시로 그 결정이 뒤집혔으므로, RM-9B8CA541은 **이 문서로 대체되어
종료**된 것으로 표시한다(그 문서 자체는 "왜 한동안 미뤘었는지"의
역사적 기록으로 남겨 두되, 상태/본문에 이 문서로의 대체를 명시).
SP-39F18E30(DMA 버퍼 관리자) §3.2의 `ProcessVirtualAddressCursor`
(bump 전용 스텁)도 이제 이 문서의 `ProcessAddressSpaceManager`로
흡수된다 - `AllocDmaBuffer`는 이제 자체 커서 대신 `mmap`과 같은
`ProcessAddressSpaceManager::findGap`+`store(VmaBacking::FixedPhysical)`
경로를 재사용한다.

## 9. 표준 파일 API (2026-09-14, 설계자 지시 - "표준 파일 API들도 모두 설계에 포함시켜")

### 9.1 전체 흐름

```
유저 코드 (libmc가 감쌈)
   |  open("/sys/etc/foo.conf", ...)
   v
[Open syscall]
   |  1. ResolvePathArgs(SP-7CC5693A §2.2)로 경로 → (ownerChannelId, relPath)
   |  2. 그 Channel로 "Open(relPath, flags)" IPC 메시지 전송(PL-C8648D4D)
   |  3. fs 서비스가 FileSystemDriver::open() 호출 → 자기 내부 FileHandle 반환
   |  4. 커널이 Process의 파일 디스크립터 테이블에 {ownerChannelId, fsHandle, offset=0} 등록
   v
프로세스별 정수 fd 반환 → 이후 read/write/lseek/close는 전부 이 fd로만 참조
```

이 흐름은 PnP(SP-9DD4F3EA §3.1 "장치 열거 → IO 권한 요청")와 정확히
같은 2단계 패턴("커널이 라우팅 정보만 알려주고, 실제 작업은 그 대상과
직접 IPC")을 재사용한다.

**[비판적 재검토로 발견한 공백, 2026-09-15]** 이 흐름은 `ResolvePathArgs`
가 항상 유저랜드 fs 서비스의 `ownerChannelId`를 낸다고 전제하는데,
SP-7CC5693A §2.4가 이후 `MountKind::KernelDriver`(livefs 등 커널
자체 구현 마운트 - Channel/IPC가 아예 없음)를 추가하면서 이 전제가
깨졌다. 그 마운트에 대한 `Open`은 2단계(IPC 전송)를 건너뛰고 커널이
`KernelFsDriver::open()`을 그 자리에서 직접 호출해야 한다 - 아래
§9.2의 `FileDescriptor`도 `ownerChannelId` 하나만으로는 "이 fd가
Channel 소비자인지 커널 드라이버 소비자인지" 구분할 수 없어 필드
확장이 필요하다(예: §2.1의 `MountKind`를 그대로 `FileDescriptor`에도
싣기). §9 착수 시 SP-7CC5693A §2.4/§2.1과 함께 반영해야 한다 -
아직 코드가 없는 설계 단계라 지금 확정하지 않고 착수 시점으로
남겨 둔다(RM-23F4B687 §4 취지).

### 9.2 프로세스 파일 디스크립터 테이블

```cpp
// minicore/kernel/process.h에 추가 제안 - DmaBuffer(SP-39F18E30 §4)와
// 동일하게 ChunkedList 재사용.
struct FileDescriptor {
    uint64_t ownerChannelId;  // 이 fd를 처리하는 fs 서비스의 Channel(SP-7CC5693A §2.2)
    uint64_t fsHandle;        // 그 fs 서비스 내부 FileSystemDriver::open()의 반환값
    uint64_t offset;          // POSIX 관례 커서 - read/write가 명시적 offset을 안 줄 때 이 값을 쓰고 자동 전진
    bool isDirectory;
    bool used = false;
};

// Process에 추가
ChunkedList<FileDescriptor, kFileDescriptorChunkCapacity> fileDescriptors;
```

**offset은 커널(fd 테이블)이 갖고, `FileSystemDriver::read/write`
(SP-7CC5693A §3.2)는 매번 명시적 offset을 받는 무상태 오퍼레이션이다**
- 이렇게 나누면 fs 서비스 드라이버 구현이 커서 상태를 신경 쓸 필요가
없고(POSIX `pread`/`pwrite`와 동일한 결), `dup()`류로 fd를 복제해도
어느 쪽이 offset을 소유하는지가 fd 테이블 하나로 명확하다.

### 9.3 Syscall API

```cpp
enum class OpenFlags : uint32_t {
    ReadOnly = 1 << 0,
    WriteOnly = 1 << 1,
    ReadWrite = ReadOnly | WriteOnly,
    Create = 1 << 2,
    Truncate = 1 << 3,
    Append = 1 << 4,
    Directory = 1 << 5,  // 디렉터리로 열기(readdir 전용)
};

struct OpenArgs {
    const char* path;      // in: 절대 경로
    uint32_t pathLen;
    uint32_t flags;        // in: OpenFlags 조합
    // out
    int32_t fd;            // 실패 시 -1
    ChannelError error;    // NotFound / PermissionDenied / AlreadyExists(Create+exclusive류 후속)
};

struct CloseArgs {
    int32_t fd;
    ChannelError error;    // InvalidHandle
};

struct ReadArgs {
    int32_t fd;
    void* buf;
    uint32_t len;
    // out
    uint32_t bytesRead;    // 0이면 EOF
    ChannelError error;
};

struct WriteArgs {
    int32_t fd;
    const void* buf;
    uint32_t len;
    // out
    uint32_t bytesWritten;
    ChannelError error;
};

enum class SeekWhence : uint32_t { Set, Current, End };

struct LseekArgs {
    int32_t fd;
    int64_t offset;
    SeekWhence whence;
    // out
    uint64_t newOffset;
    ChannelError error;
};

struct StatArgs {
    const char* path;      // in (fd 기반 fstat은 후속 변형으로 추가 가능)
    uint32_t pathLen;
    // out
    StatBuf stat;          // SP-7CC5693A §3.2 참고
    ChannelError error;
};

struct ReaddirArgs {
    int32_t fd;             // Directory 플래그로 연 fd
    // out
    DirEntry entry;         // SP-7CC5693A §3.2 참고 - 호출마다 다음 엔트리 하나
    bool hasMore;
    ChannelError error;
};

struct MkdirArgs {
    const char* path;
    uint32_t pathLen;
    ChannelError error;
};

struct UnlinkArgs {
    const char* path;      // 파일이면 unlink, 빈 디렉터리면 rmdir 라우팅(§9.4)
    uint32_t pathLen;
    ChannelError error;
};
```

`Read`/`Write`/`Lseek`/`Close`/`Readdir`는 전부 `fd`를 fd 테이블에서
찾아 `ownerChannelId`로 IPC 메시지를 보내는 동일한 패턴이다 - 명시적
offset이 필요한 `FileSystemDriver::read/write` 호출 시 fd 테이블의
`offset` 필드를 넘기고, 성공하면 `bytesRead`/`bytesWritten`만큼 그
필드를 전진시킨다(Append 플래그면 항상 파일 끝 기준으로 재계산 -
세부는 구현 시점).

### 9.4 `Mkdir`/`Unlink`가 `Stat`과 다른 점 - 디스크립터 없이 경로만으로 동작

`mkdir`/`unlink`/`rmdir`/`stat`은 파일을 "열지" 않고 경로만으로
수행되는 오퍼레이션이라, fd 테이블을 거치지 않고 `ResolvePathArgs`로
얻은 채널에 직접 1회성 메시지를 보내는 것으로 충분하다(§9.1의 4단계
흐름에서 3~4단계, "fd 등록"이 필요 없음) - `FileSystemDriver`(SP-7CC5693A
§3.2)의 `stat`/`mkdir`/`rmdir`/`unlink`가 전부 `FileHandle`이 아니라
`relPath`를 직접 받는 것도 이 때문이다.

### 9.5 파일 백킹 mmap과의 연결 (§4-4, §6-4 참고)

`mmap`(§5)에 `flags`로 "파일 백킹" 옵션이 추가되면(v1 이후, `fs`
서비스가 실제로 동작하는 시점), `Vma::backing = FileBacked`로
`backingFd`/`fileOffset`을 채워 등록한다 - 유저 모드 페이지 폴트
(SP-8B6B8D25 §2-B)가 이 VMA를 만나면, 이미 있는 `FileDescriptor`의
`ownerChannelId`로 그 오프셋 범위를 `Read`해 물리 페이지를 채우는
흐름이 된다(Anonymous VMA의 "PageFrameAllocator로 그냥 0으로 채움"과
갈라지는 지점 - 파일 백킹은 실제 데이터를 읽어와야 한다). 이 흐름의
정확한 캐시/dirty 페이지 되쓰기(writeback) 정책은 `fs` 서비스가
실제로 존재하기 전까지는 확정할 수 없어 후속 과제로 남긴다.

### 9.6 아직 열려 있는 설계 영역 (§9 전용)

1. **`dup`/`dup2`류 fd 복제**: 여러 fd가 같은 `FileDescriptor` 엔트리
   (또는 offset을 공유하는 엔트리)를 가리키는 경우의 참조 카운트 -
   `AtomicU32`(libkenv/spinlock.h, 이미 구현 완료) 재사용 제안, 세부는
   실제 착수 시점.
2. **경로 오프닝의 심볼릭 링크/`..`/`.` 정규화**: `ResolvePathArgs`
   (SP-7CC5693A §2.2)가 지금은 단순 접두사 매칭만 가정 - 심볼릭 링크는
   각 `fs` 드라이버가 있어야 의미가 생기므로 그때 같이 설계.
3. **`Open`의 `Create` 플래그 경합(동시에 두 프로세스가 같은 파일을
   O_CREAT|O_EXCL로 열 때)**: `fs` 서비스 쪽 드라이버의 원자성 보장
   범위 - 각 드라이버(ext4 등) 착수 시점의 구현 세부.

