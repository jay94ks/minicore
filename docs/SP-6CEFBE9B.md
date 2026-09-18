# Minicore 물리 페이지 프레임 메타데이터 — PageFrame 구조체

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-6CEFBE9B
  status: approved
  updatedAt: 2026-09-18T10:05:32.137Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# Minicore 물리 페이지 프레임 메타데이터 — `PageFrame` 구조체

## 배경

설계자 opinion(2026-09-18, `SP-8D206F11` 대상): "Page 단위 추상화
(Linux의 page 구조체)도 함께 도입할텐데 그 내용도 작성하고, 별도
문서가 필요하다면 별도 문서로 작성하라." 이 내용은 캐시 관리 정책
문서보다 물리 메모리 관리자(`PageFrameAllocator`) 자체의 범위라
별도 문서로 분리한다.

**처음부터 새로 발명하는 자료구조가 아니다** - `SP-6BEAE0C1` §11
항목3("COW 참조 카운트 배치")이 이미 최소한의 프레임별 메타데이터를
구현해 뒀다: `PageFrameAllocator::init()`이 `Paging::directMapLimit()`
기준으로 프레임 개수를 실측해 커널 이미지 바로 뒤 물리 공간에
`uint16_t` 배열을 bump 방식으로 예약하고(`kSubtractReservedFromList`로
usable range에서 제외), `retain(physAddr)`/`refCount(physAddr)` 두
정적 메서드로 접근한다 - COW(`SP-2AAD7C8D` §6-6)가 이미 이 API를
소비 중이다. 이 문서는 이 "refCount 하나만 있는 배열"을 "여러 필드를
가진 `PageFrame` 구조체 배열"로 일반화한다.

**[개정, 2026-09-18, 설계자 opinion]** 최초 작성은 "지금 실사용처가
있는 필드만 v1으로 최소화"하는 방향이었으나, 설계자가 "이 구조체는
한 번 정의하면 나중에 바꿀 때 파급력이 크니 v1으로 한정하지 말고
두 수 앞을 내다보고 설계하라"고 지시했고, 이어서 §5가 예비 공간으로만
남겨 뒀던 rmap/swap LRU 두 가지도 "Linux와 유사한 구조로" 실제로
설계해 반영하라고 추가 지시했다 - 이번 개정이 그 둘을 채운다.

## 1. `PageFrame` 구조체 — 고정 크기(캐시 라인) + 예비 공간

**설계 원칙**: 배열이 이미 커널 이미지 뒤에 bump 예약된 물리 공간을
차지하고 있어, 원소 하나의 크기(stride)가 바뀌면 배열 전체를 다시
자리 잡아야 한다 - 이게 설계자가 말한 "파급력"의 실체다. rmap/LRU를
실제로 채워 넣은 결과 이전 개정의 32바이트로는 부족해져, **캐시 라인
전체(64바이트)**로 키웠다 - Linux `struct page`도 정확히 이 이유로
캐시 라인 크기에 맞춘다(align 겸 향후 확장 여유).

```cpp
// 물리 프레임 하나(4KiB)당 정확히 하나 - PageFrameAllocator::init()이
// direct map 크기만큼 이 배열을 커널 이미지 바로 뒤에 bump 예약한다
// (SP-6BEAE0C1 §11-3과 동일한 배치 패턴 - 기존 uint16_t 배열이 이
// 구조체 배열로 대체된다).
struct PageFrame {
    // --- 참조/공유 (기존 §1, 변경 없음) ---
    uint16_t refCount;          // 총 참조 카운트(COW+공유 메모리 등)
    uint16_t mapCount;           // 실제 PTE가 가리키는 횟수(§6 참고)

    // --- 버디/캐시 메타데이터 (기존 §1, 변경 없음) ---
    uint8_t  order;               // 버디 오더, free 블록 head에서만 유효
    uint8_t  numaNode;
    uint16_t flags;                // §2
    uint8_t  lastCacheType;         // SP-8D206F11 §2.3 CacheType, §3
    uint8_t  cacheTypeAssigned;

    // --- [신규] 역참조(rmap) - §6 ---
    RmapEntry* rmapHead;             // 이 프레임을 매핑 중인 (프로세스,
                                     // 가상주소) 쌍들의 단일 연결 리스트.
                                     // Anonymous 백킹 VMA(스왑 대상)에만
                                     // 채워짐 - MMIO/DMA/FileBacked는
                                     // nullptr 고정(§6.3).

    // --- [신규] swap LRU 연결 - §7 ---
    PageFrame* lruPrev;
    PageFrame* lruNext;              // 전역 active/inactive 이중 연결
                                     // 리스트의 이웃 - 리스트 미소속이면
                                     // 둘 다 nullptr.

    // --- 예비 공간 ---
    // 지금은 어떤 필드도 배정하지 않는다(CLAUDE.md 규칙 4 - 설계 확정
    // 전 임의 배정 금지) - 구조체 stride를 다시 바꾸지 않고 흡수할
    // 여유로 남겨 둔다.
    uint8_t  reserved[22];
};
static_assert(sizeof(PageFrame) == 64);
```

크기 계산: refCount(2)+mapCount(2)+order(1)+numaNode(1)+flags(2)+
lastCacheType(1)+cacheTypeAssigned(1) = 10, + rmapHead(8)+lruPrev(8)+
lruNext(8) = 34, + reserved(22) = 56... 실제 컴파일러 정렬(포인터
필드가 8바이트 경계에 와야 함)에 따라 정확한 패딩은 달라질 수 있어
`reserved` 크기는 "8의 배수 경계를 맞추고 전체가 64바이트가 되도록"
조정한다는 의도로 읽는다 - 정확한 바이트 수는 구현 시점에
`static_assert`가 강제하므로 이 문서에서 1바이트 단위까지 확정하지
않는다(RM-23F4B687 §4 취지 - 여기서부터는 진짜 숫자 튜닝).

배열 인덱스는 기존과 동일하게 물리 프레임 번호(direct map 시작 기준
상대 프레임 번호)로 삼는다.

## 2. 플래그

- **`PG_RESERVED`**(비트0): 할당 대상 아님(커널 이미지, 이 배열
  자신).
- **`PG_HEAD`**(비트1): order>0 블록의 첫 프레임(페이지 병합,
  `SP-8B6B8D25` §2 항목8).
- **`PG_ACTIVE`**(비트2) — [신규, §7]: active 리스트 소속(꺼져 있으면
  inactive 리스트 소속 또는 리스트 미소속).
- **`PG_ACCESSED`**(비트3) — [신규, §7]: 최근 참조됨(x86 PTE의
  하드웨어 Accessed 비트를 폴트/스캔 시점에 여기로 반영) - LRU 승격
  판단에 사용.
- **`PG_SWAPPABLE`**(비트4) — [신규, §6/§7]: 이 프레임이 rmap/LRU
  추적 대상(Anonymous 백킹)임을 표시 - MMIO/DMA/FileBacked는 이
  비트가 꺼진 채로 유지되고, rmap 리스트에도 LRU 리스트에도 절대
  들어가지 않는다.

## 3. [열린 확인 — 설계자] 캐시 타입 일관성 검사 — `SP-8D206F11`과의 연결

(이전 개정과 동일, 변경 없음)

x86_64는 같은 물리 페이지를 서로 다른 캐시 타입(PAT 인덱스)으로
동시에 매핑하는 것을 사실상 미정의 동작으로 취급한다(Intel SDM
Vol.3A §11.5.2.1). `lastCacheType`/`cacheTypeAssigned`가 이 검사의
자리다 - `kMapPageWithCacheType`이 매핑 전에 확인해, 최초 매핑이면
기록하고 진행하되, 이미 다른 타입으로 설정된 프레임을 다시 다른
타입으로 매핑하는 경우의 처리 정책은 여전히 미정이다(`PN-81223433`).

## 4. 기존 API와의 관계

`PageFrameAllocator::retain(physAddr)`/`refCount(physAddr)`
(`SP-6BEAE0C1` §11-3, `SP-2AAD7C8D` §6-6의 COW가 이미 소비 중)
시그니처는 그대로 유지한다 - 내부 구현만 `PageFrame[].refCount`
필드 접근으로 바뀐다. 기존 호출부(COW 로직)는 코드 변경 없이 동일하게
계속 동작한다.

## 5. `RmapEntry` — rmap 엔트리 자료구조

```cpp
// PageFrame::rmapHead가 가리키는 단일 연결 리스트의 노드 하나 -
// "이 프레임이 어떤 프로세스의 어떤 가상주소에 매핑돼 있는지" 한 쌍.
struct RmapEntry {
    Process* owner;      // 이 매핑을 소유한 프로세스(SP-6BEAE0C1의
                          // Process, §2 부모/자식 포인터와 같은 급)
    uint64_t virtAddr;    // 그 프로세스 주소공간에서의 가상주소(페이지
                          // 정렬됨)
    RmapEntry* next;       // 같은 프레임을 가리키는 다음 엔트리
};
```

크기 24바이트 - `GenericSlabAllocator`(`SP-D7013B26`)의 최소 버킷인
32B 버킷에 그대로 들어간다(§2.3 "32B 미만 요청 금지" 조건을 만족하는
가장 작은 표준 버킷) - **이 자료구조를 위해 새 전용 할당자를 만들지
않는다**, 기존 범용 slab을 그대로 재사용한다(RM-23F4B687 §4 - 여기는
"과설계 방지"가 그대로 적용되는 진짜 구현 세부).

## 6. 역참조(rmap) 설계

### 6.1 왜 필요한가

swap이 실제로 프레임을 회수하려면(`SP-8B6B8D25` §2 항목6) 그 프레임을
가리키는 **모든** PTE를 먼저 무효화해야 한다 - VMA(`SP-2AAD7C8D`)는
"가상주소 → 물리주소"만 알 뿐 그 반대는 모른다. `mapCount`(§1)는
"몇 곳"인지는 세지만 "어디"인지는 모른다 - rmap이 그 "어디"에 답한다.

### 6.2 자료구조 — 프레임당 연결 리스트 (Linux `anon_vma`보다 단순화)

Linux는 fork 트리를 공유하는 여러 VMA가 하나의 `anon_vma`를 공유하고
그 안에서 구간 트리로 페이지를 찾는 정교한 구조를 쓴다 - 이 프로젝트
규모에서 그 복잡도를 그대로 들여올 근거가 없다(실사용처가 그 정도
스케일에 도달했다는 증거가 전혀 없음, `RM-23F4B687` §4). 대신 **프레임
하나마다 `(Process*, virtAddr)` 쌍의 단순 연결 리스트**(§5의
`RmapEntry`)를 둔다 - "이 프레임을 참조하는 매핑을 전부 찾는다"라는
요구를 그대로 만족하면서, 이 프로젝트에 이미 있는 자료구조(slab
할당자)만으로 구현 가능하다.

- **삽입**: 요구 페이징(`SP-2AAD7C8D` §9.5)이나 COW 폴트가 물리
  프레임을 실제로 PTE에 매핑하는 순간, `PG_SWAPPABLE`이 켜진
  프레임(즉 `VmaBacking::Anonymous`)이면 `RmapEntry`를 하나 만들어
  `rmapHead`에 push하고 `mapCount++`.
- **제거**: `munmap`/프로세스 종료/페이지 언매핑 시 해당 `(owner,
  virtAddr)` 엔트리를 리스트에서 찾아 제거하고 `mapCount--`
  (리스트가 프레임당이라 항목 수는 보통 작음 - COW로 공유된 경우에만
  둘 이상).
- **스왑아웃 시 사용**: 회수 대상 프레임의 `rmapHead`를 순회하며 각
  `(owner, virtAddr)`의 PTE를 present=0으로 바꾸고 TLB shootdown -
  전부 무효화한 뒤에야 실제 스왑아웃(`SwapBackend::writeSlot`,
  `SP-2BCE5D60` §4)으로 넘어간다.

### 6.3 적용 범위 - Anonymous + FileBacked 백킹(MMIO/DMA는 제외)

[결정, 2026-09-18, PN-9E2CC631, 설계자 지시로 지금 확정] FileBacked
페이지도 Anonymous와 **같은 rmap/LRU 메커니즘을 공유**한다 - 별도의
"page cache 전용" 회수 경로를 새로 만들지 않는다(RM-23F4B687 §4 -
아직 이 구조를 실제로 소비할 fs 서비스 실코드(`SP-2BCE5D60`,
`PN-452FF696`)가 하나도 없는 시점에 두 개의 병렬 회수 인프라를 만들
근거가 없다 - Linux도 2.6.28 이후 anon/file 페이지를 하나의
active/inactive LRU로 통합 관리한다는 선례를 그대로 따른다).

`MMIO`(`SP-8D206F11`)/`DMA`(`SP-39F18E30`) 프레임만 여전히 스왑
대상이 아니므로 rmap 리스트를 유지할 이유가 없다 - `PG_SWAPPABLE`이
꺼진 채로 두고 `rmapHead`는 항상 `nullptr`이다.

`VmaBacking::FileBacked`(`SP-2AAD7C8D` §4) 프레임은 §9.5(요구
페이징)가 실제로 PTE를 매핑하는 시점에 `PG_SWAPPABLE`을 켜고 §6.2와
정확히 같은 방식으로 `RmapEntry`를 삽입한다 - Anonymous와 삽입
지점/자료구조가 완전히 동일하다(차이는 §7.4의 회수 동작에서만
갈린다).

## 7. Swap LRU 설계 (Linux와 유사한 구조)

### 7.1 자료구조 — 전역 active/inactive 이중 연결 리스트

```cpp
// 전역 하나(NUMA 노드별 분리는 실사용 압박이 확인되면 후속 - §8).
struct PageFrameList {
    PageFrame* head;
    PageFrame* tail;
};
PageFrameList gActiveList;    // 최근 접근된 프레임들
PageFrameList gInactiveList;  // 회수 후보 - 리스트 끝(tail)에 가까울
                                // 수록 먼저 쫓겨날 후보.
```

`PageFrame::lruPrev`/`lruNext`(§1)로 스레딩한다 - 새 자료구조를 더
추가하지 않고 프레임 메타데이터 자체에 이중 연결 리스트 노드를
내장하는 Linux `struct page`의 `lru` 필드와 같은 패턴.

### 7.2 상태 전이 (Linux의 2-리스트 + accessed-bit 모델)

1. **신규 진입**: `PG_SWAPPABLE`인 프레임이 처음 매핑되면(§6.2 삽입
   시점) `gInactiveList` 뒤(head)에 넣는다 - Linux와 동일하게 "처음
   보는 페이지는 일단 의심"(바로 active에 넣지 않음).
2. **재접근 감지 — [정정, 2026-09-18, 설계자 opinion] 폴트 기반이
   아니라 스캔 기반**: 최초 작성 시점엔 "폴트 처리 경로가 재접근을
   감지해 그 시점에 `PG_ACCESSED`를 세팅한다"고 잘못 적었다 - 이미
   `PAGE_PRESENT`인 페이지를 다시 읽거나 쓰는 것은 x86_64에서
   **애초에 폴트를 일으키지 않는다**(CPU가 트랩 없이 PTE의 하드웨어
   Accessed 비트만 자동으로 세팅) - v1처럼 즉시 매핑 위주라 지연
   매핑/재폴트 경로가 거의 없다는 사실과 무관하게, 애초에 "폴트로
   재접근을 감지한다"는 전제 자체가 틀렸다. 실제로 재접근을 관찰하는
   유일한 방법은 **3단계의 스캔이 각 프레임의 PTE를 직접 walk해
   하드웨어 Accessed 비트를 읽고(있으면 `PG_ACCESSED`에 반영 후
   PTE 쪽은 클리어) 그 자리에서 곧장 승격까지 처리하는 것**이다 -
   즉 이 2단계는 별도 메커니즘이 아니라 3단계 스캔의 일부다(Linux의
   페이지 회수 스캐너가 하는 일과 동일 - refault와 무관하게 항상
   PTE 스캔으로 age를 판정). 지연 매핑/재폴트 경로가 없다는 점은
   오히려 "이 스캔이 Accessed 비트를 관찰하는 유일한 경로"라는
   뜻이라, 스캔 자체(§8-1, `PN-4859FDE9`)를 설계하지 않고는 이 상태
   전이를 아예 구현할 수 없다 - 두 항목이 사실상 하나의 결정이라는
   점을 명확히 한다.
3. **승격(inactive → active)**: 위 스캔이 걸으며 `PG_ACCESSED`가
   켜진 프레임을 만나면 active로 옮기고 비트를 지운다(second-chance)
   - **스캔을 누가 언제 돌릴지(회수 압박 시
   동기? 별도 커널 스레드? 주기?)는 이 문서 범위 밖**(§8) - 자료구조와
   전이 규칙만 확정, 스캔 트리거 정책은 `PageFrameAllocator`의 기존
   §2.4 고갈 정책(즉시 nullptr, 비블로킹 - `SP-D7013B26`과 같은 전역
   원칙)과 맞물려야 하는 더 큰 결정이라 별도로 남긴다.
4. **강등(active → inactive)**: active 리스트가 일정 비율 이상
   커지면 오래된 쪽(tail)부터 일부를 inactive로 되돌린다(aging) -
   정확한 비율/주기는 §8.
5. **회수(eviction)**: 메모리 압박 시 `gInactiveList` tail부터
   `PG_ACCESSED`가 꺼진 프레임을 골라 §6.2의 rmap 무효화 →
   `SwapBackend::writeSlot`(`SP-2BCE5D60` §4) 순서로 내보낸다.

### 7.3 지금 확정하는 것 / 미루는 것

- **확정**: 2-리스트(active/inactive) 구조, `PG_ACCESSED` 비트의
  의미, 승격/강등이 "재접근 여부"로 결정된다는 원칙, 회수는 항상
  inactive tail부터. 이 다섯 가지는 Linux의 핵심 아이디어를 그대로
  가져온 것으로 구조적으로 재검토가 거의 필요 없는 부분이다.
- **미룸**(§8): 스캔이 언제/어떤 스레드에서 도는지, active/inactive
  목표 비율, 회수 배치 크기(한 번에 몇 개나 내보낼지) - 전부 "숫자
  튜닝" 또는 "동시성 모델" 수준의 구현 세부라 swap이 실제로 착수될
  때 계측하며 정한다(`RM-23F4B687` §4).

### 7.4 회수 동작의 백킹별 차이 (Anonymous vs FileBacked)

[결정, 2026-09-18, PN-9E2CC631] §7.2 5단계(회수)의 "rmap 무효화 →
내보내기" 중 **"내보내기" 자체의 목적지가 백킹에 따라 갈린다**:

- **Anonymous**: 항상 `SwapBackend::writeSlot`(`SP-2BCE5D60` §4)로
  스왑아웃 - 원본이 이 메모리 자신뿐이라 반드시 어딘가에 내용을
  보존해야 한다(기존 §7.2 그대로).
- **FileBacked, 깨끗한(clean) 경우**(로드 이후 한 번도 안 쓰임):
  그냥 버린다(rmap 무효화 후 프레임만 반납) - 파일 자체가 이미 그
  원본이므로 다시 필요해지면 §9.5의 요구 페이징이 다시 읽어 온다.
  스왑 슬롯을 전혀 쓰지 않는다.
- **FileBacked, 더러운(dirty) 경우**(로드 이후 쓰기가 있었음): 프레임을
  반납하기 전에 그 내용을 파일로 write-back해야 한다 - **정확한
  write-back API/트리거는 아직 미정**(`fs` 서비스 실코드가 없어
  검증 불가) - `PN-452FF696`(fs 서비스) 완료 이후로 남긴다(§8 항목3
  범위 축소 - "공유 LRU를 쓸지"는 이번 결정으로 이미 해소됨, 남은
  것은 dirty 판정/write-back 경로뿐).
- **깨끗함/더러움 판정 방법**: 별도 `PG_DIRTY` 플래그를 지금 §2에
  새로 배정하지 않는다(RM-23F4B687 §4 - 아직 이걸 실제로 세팅/소비할
  코드가 없다) - `PG_ACCESSED`가 x86 하드웨어 Accessed 비트를 그대로
  반영하는 것과 같은 패턴으로, write-back 경로 착수 시 하드웨어
  Dirty 비트(PTE)를 그대로 반영하는 새 플래그를 그때 §2에 추가하기로
  하고 지금은 예비 공간(§1 `reserved`)에만 여지를 남긴다.

## 8. [열린 확인 — 설계자] 여전히 남은 것

1. **스왑 회수 트리거/스캔 스레드**(§7.3) - 언제 스캔이 도는지,
   `PageFrameAllocator`의 "즉시 nullptr" 고갈 정책과 어떻게
   맞물리는지(고갈 시점에 동기적으로 회수를 시도하는지, 아니면 별도
   백그라운드 스레드가 미리 여유분을 만들어 두는지) - swap 착수
   시점에 확정.
2. **캐시 타입 불일치 처리**(§3) - `PN-81223433`으로 계속 추적.
3. **FileBacked write-back 경로/dirty 판정**(§7.4) - "공유 LRU를
   쓸지"는 2026-09-18 결정(PN-9E2CC631)으로 이미 해소됨(§6.3/§7.4) -
   남은 것은 dirty 비트 반영 방식과 실제 write-back 호출 지점뿐,
   fs 서비스(`SP-2BCE5D60`, `PN-452FF696`) 착수 시 재검토.

## 참고

- `SP-8D206F11` §2.3 — `kMapPageWithCacheType`, §3의 대상.
- `SP-6BEAE0C1` §11 항목3 — 기존 `uint16_t` 참조 카운트 배열(이 구조체가 대체).
- `SP-2AAD7C8D` §6-6/§9.5 — COW `retain()`/요구 페이징, §6의 rmap 삽입 지점.
- `SP-2BCE5D60` §4 — `SwapBackend::writeSlot`, §7.2 회수 절차의 최종 목적지.
- `SP-D7013B26` §2.3 — `GenericSlabAllocator` 32B 버킷, §5의 `RmapEntry` 할당처.
- `SP-8B6B8D25` §2 항목6/7/8/10 — 이 구조체가 채우는 커널 책임들의 원출처.
- `PN-81223433` — §3 캐시 타입 불일치 처리 정책(미정, 낮은 우선순위).
- `minicore/kernel/page_frame_allocator.{h,cpp}` — 실제 구현 위치.

