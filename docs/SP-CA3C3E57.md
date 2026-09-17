# Channel/BridgeHandle 안전한 핸들 해석 — 세부 설계 (PN-CE6A04AB)

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-CA3C3E57
  status: review
  updatedAt: 2026-09-17T02:23:18.238Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->


# Channel/BridgeHandle 안전한 핸들 해석 — 세부 설계

QU-28293B62 답변("세부 설계안을 작성해봐")에 대한 상세 설계 -
PN-CE6A04AB가 발견한 보안 취약점(`ConnectChannel`/
`AcceptFromChannel`/`DestroyChannel`이 유저 제공 `ChannelId`/
`BridgeHandle`을 검증 없이 `reinterpret_cast<Channel*>`로 역참조)의
실제 수정안이다.

## 0. 착수 전 재확인 — 이미 절반은 고쳐져 있었다

설계 작성 중 `channel.cpp`를 다시 읽다가 **`ChannelRead`/
`ChannelWrite`/`CloseBridge` 세 핸들러는 이미 같은 종류의 취약점이
고쳐져 있음을 발견**했다 - `PN-9CC66142`(2026-09-17, 이 세션이
PN-CE6A04AB를 등록한 시점 이전에 이미 완료돼 있던 작업)가
`kResolveOwnedBridge(task, handle)`를 도입해, `args->bridge`
(`BridgeHandle`)를 더 이상 직접 `reinterpret_cast`하지 않고 **호출자
자신의 `Process::openBridges`에서 실제로 그 포인터를 쥐고 있는지
찾아서만** 반환한다(channel.cpp:245-257). PN-CE6A04AB가 지목한 다섯
지점 중 이 세 핸들러는 대상이 아니었다(애초에 안전) - 실제로 아직
안 고쳐진 건 **`ConnectChannel`의 `target`**, **`AcceptFromChannel`/
`DestroyChannel`의 `channelHandle`**, 즉 `ChannelId`/`Channel*` 쪽
셋뿐이다(`BridgeHandle`/`BridgePipe*` 쪽은 이미 안전).

## 1. 왜 `kResolveOwnedBridge`의 "호출자 소유 리스트" 패턴을 그대로
못 가져오는가

`kResolveOwnedBridge`가 안전한 이유는 `BridgePipe`가 **본질적으로
연결의 한쪽 당사자에게 배타적으로 속한다** - 연결이 성사되면
`clientProcess`/`acceptorProcess` 각자의 `openBridges`에 자기 몫만
들어가고(channel.cpp:458-477), 그 이후 Read/Write/Close는 항상
"내가 가진 내 쪽 파이프"만 조작한다. 그래서 "호출자 자신의 목록에서
찾아진 것만 신뢰"가 정확히 필요한 권한 범위와 일치한다.

`Channel`(랑데부 지점 자체)은 성격이 다르다:
- **`ConnectChannel`의 `target`**: 정의상 **자신이 만들지 않은 남의
  Channel**에 연결하는 것이다(이름 기반 발견의 raw-ID 버전) - "호출자
  소유 목록"이라는 개념 자체가 성립하지 않는다. 이건 프로세스 경계를
  넘어 공유되도록 **의도된** 식별자라, 안전하게 해석하려면 여전히
  "전역에서 유효성을 검증하는" 메커니즘이 필요하다.
- **`AcceptFromChannel`/`DestroyChannel`의 `channelHandle`**: 이건
  반대로 "자기가 만든 채널에 대해서만 호출해야 정상"이라 이론상
  "호출자 소유 목록" 패턴이 **더 잘 맞는다** - 그런데 지금 `Channel`
  에는 "누가 만들었는지"를 기록하는 소유자 필드 자체가 없다
  (channel.h:267-310 `Channel` 클래스 전체에 소유자 필드 없음,
  §4에서 별도 논의).

**결론**: `target`(공유 식별자)과 `channelHandle`(사실상 자기
소유물이어야 하는데 추적이 안 되고 있는 것)은 서로 다른 문제라
같은 해법으로 묶이지 않는다 - 이 문서는 즉시 고칠 수 있는 것(§2,
공유 식별자의 안전한 해석)과 후속으로 분리할 것(§4, 소유권
추적/권한 부재)을 나눈다.

## 2. 채택 설계 — 세대 태그 슬롯 테이블 (`SP-9CB55C5B`와 동일 패턴)

`target`이 프로세스 경계를 넘어 공유되도록 의도된 식별자인 이상,
`SP-9CB55C5B`가 `ProcessId`에 이미 적용한 것과 같은 메커니즘이
그대로 필요하다 - 유저가 어떤 64비트 값을 주든 **역참조 없이** 그
값이 지금 살아있는 진짜 `Channel`을 가리키는지 판정한다.

```cpp
// channel.cpp (제안) - Channel은 GenericSlabAllocator로 직접 할당되는
// 순수 포인터 객체라(SharedPtr 아님, PN-CE6A04AB에서 이미 확인)
// SP-9CB55C5B의 WeakPtr::lock() 대신 수동 freed 플래그를 쓴다.
struct ChannelTableSlot {
    Channel* ptr = nullptr;   // nullptr = 비어있음/이미 해제됨
    uint32_t generation = 0;  // 재사용될 때마다 +1 (ABA 방지)
};

constexpr uint32_t kMaxChannelTableSlots = 65535;  // UINT16_MAX -
// SP-9CB55C5B §4가 ProcessId 테이블에 이미 확정한 상한과 동일한
// 값을 그대로 재사용(같은 종류의 "동시 생존 개체 수" 상한 개념이라
// 별도로 다른 값을 새로 확정받을 이유가 없다고 판단 - 만약 실측으로
// 부족하면 그때 재조정).

ChannelTableSlot gChannelTable[kMaxChannelTableSlots];
Spinlock gChannelTableLock;  // 등록/해제만 보호(드묾) - 조회는
                             // 락 없이 인덱스+세대만 비교 후 그
                             // 결과 포인터로 Channel::lock을 타므로
                             // 락 순서 역전 없음(§3 참고).

using ChannelId = uint64_t;  // 이제 포인터가 아니라 발급된 핸들
constexpr ChannelId kInvalidChannelId = 0;  // 기존 관례(0=없음) 유지

// 발급 - kCreateNamedChannel()/openChannel 성공 경로에서 호출.
ChannelId kAllocateChannelId(Channel* channel) {
    SpinlockGuard guard(gChannelTableLock);
    for (uint32_t i = 0; i < kMaxChannelTableSlots; ++i) {
        if (gChannelTable[i].ptr == nullptr) {
            gChannelTable[i].generation++;
            gChannelTable[i].ptr = channel;
            channel->tableIndex = i;  // 해제 시 O(1) 역참조용 - Channel에 필드 추가
            return (static_cast<uint64_t>(gChannelTable[i].generation) << 32) | i;
        }
    }
    return kInvalidChannelId;  // 슬롯 고갈 - ResourceExhausted로 매핑
}

// 안전 해석 - 전부 이 함수를 거친다, reinterpret_cast 직접 호출 금지.
Channel* kResolveChannelId(ChannelId id) {
    if (id == kInvalidChannelId) return nullptr;
    const uint32_t index = static_cast<uint32_t>(id & 0xFFFFFFFF);
    const uint32_t generation = static_cast<uint32_t>(id >> 32);
    if (index >= kMaxChannelTableSlots) return nullptr;
    ChannelTableSlot& slot = gChannelTable[index];  // 락 없이 읽음(§3)
    if (slot.generation != generation || slot.ptr == nullptr) return nullptr;
    return slot.ptr;
}

// 해제 - DestroyChannelHandler::onExec의 GenericSlabAllocator::free
// 직전에 호출(그 채널의 tableIndex로 O(1) 접근).
void kFreeChannelId(Channel* channel) {
    SpinlockGuard guard(gChannelTableLock);
    gChannelTable[channel->tableIndex].ptr = nullptr;  // generation은 그대로 - 다음 재사용 때 +1
}
```

**선형 탐색 발급 비용에 대한 메모**: `kAllocateChannelId`가 빈 슬롯을
선형 탐색하는 건 `SP-9CB55C5B`의 `kAllocateProcessId`도 동일하게
"freelist 권장"이라고만 적어 두고 세부는 착수 시로 미룬 것과 같은
수준 - Channel 생성은 스케줄러 핫패스가 아니라(연결/디스커버리 시
1회성 syscall) 빈도가 낮으므로 v1은 단순 선형 탐색으로 시작해도
무방하다고 판단한다(RM-23F4B687 §4 과설계 방지) - 실측으로 문제되면
그때 freelist로 교체.

## 3. 동시성 - 왜 조회(`kResolveChannelId`)에 락이 필요 없는가

`gChannelTableLock`은 **발급/해제**(드문 쓰기)만 보호한다. 조회는
그 순간의 `{ptr, generation}` 값을 그냥 읽는다 - 최악의 경우
발급/해제와 경합해 "방금 막 재사용된 슬롯"을 오래된 generation으로
읽어 `nullptr`을 반환할 수는 있지만(즉 "못 찾음"으로 안전하게
실패), **틀린 살아있는 Channel을 가리키는 일은 없다**(generation
불일치가 항상 먼저 걸러짐 - SP-9CB55C5B §2의 `kResolveProcessId()`와
동일한 안전 논증). `SP-9F1DB1D8`이 `gCurrentTask`에 적용한 것 같은
전면적 RW-lock까지는 필요 없다고 판단한 이유 - `gCurrentTask`는
쓰기가 극도로 예민한 디스패치 타이밍(cli 구간)과 얽혀 있어 표준
미정의 동작 자체를 없애야 했지만, 이 테이블은 쓰기가 syscall
핸들러 안(이미 인터럽트 허용 상태, 타이밍 제약 없음)에서만 일어나고
조회 실패 시의 대가가 "그냥 NotFound 반환"뿐이라 같은 수준의
엄밀함이 필요하지 않다. **[열린 확인]** 이 판단(테이블 조회는
락 불필요)이 맞는지는 §6 질문에서 확인한다.

## 4. 적용 - 세 호출부 교체

```cpp
// ConnectChannelHandler::onExec/onCancel (channel.cpp:314-315, :379-380)
if (args->target != 0) {
    channel = kResolveChannelId(args->target);
    if (!channel) {
        args->error = ChannelError::NotFound;  // 위조/이미 소멸된 핸들
        co_return;  // (onCancel은 co_return 대신 그냥 return)
    }
}
// name 경로는 기존 그대로(NamedObjectTable::resolve - 이미 안전).

// AcceptFromChannelHandler::onExec (channel.cpp:408)
auto* channel = kResolveChannelId(args->channelHandle);
if (!channel) {
    args->error = ChannelError::NotFound;
    co_return;
}
// (AcceptFromChannelHandler::onCancel도 동일 - args->channelHandle==0
//  이던 기존 널체크 분기를 kResolveChannelId 실패 분기로 대체)

// DestroyChannelHandler::onExec (channel.cpp:726)
auto* channel = kResolveChannelId(args->channelHandle);
if (!channel) {
    args->error = ChannelError::NotFound;  // 기존엔 널체크조차 없었음
    co_return;
}
```

## 5. `OpenChannelHandler`/`kCreateNamedChannel` 갱신

`args->channelId = reinterpret_cast<uint64_t>(channel)` (channel.cpp:295)
→ `args->channelId = kAllocateChannelId(channel)`로 교체. 발급
실패(슬롯 고갈)는 `ChannelError::ResourceExhausted`로 매핑하고
방금 만든 `Channel`은 그 자리에서 반납(`GenericSlabAllocator::free`
+ 이름 있었으면 `NamedObjectTable::release`)한다 - 자원 누수 방지.

`channel.h:27` 주석("v1은 ChannelId/BridgeHandle을 같은 값으로
채운다")도 이제 정확하지 않다 - `channelId`는 이 새 핸들 값,
`channelHandle`(`OpenChannelArgs`의)은 여전히 이 값과 같은 값을
채워도 되지만(둘 다 "이 Channel을 가리키는 값"이라는 의미는 유지),
`BridgeHandle`(연결 이후 `bridge` 필드가 실제로 갖는 `BridgePipe*`)
과는 이제 확실히 다른 값 공간이라는 점을 주석으로 명확히 한다
(§0에서 확인했듯 실제로도 이미 그렇게 동작하고 있었다 - 주석만
낡아 있었다).

## 6. [열린 확인 — 설계자] 이 세부 설계에 대한 질문

1. `kMaxChannelTableSlots = UINT16_MAX`(§2, ProcessId와 동일 값
   재사용)가 적절한지, 아니면 Channel은 성격이 달라(프로세스보다
   훨씬 자주 열고 닫힐 수 있음) 다른 상한이 필요한지.
2. §3에서 "테이블 조회 자체엔 락이 불필요하다"고 판단했는데, 이
   판단에 동의하는지 - 아니면 일관성을 위해 여기도 `SP-9F1DB1D8`
   스타일 `RwSpinlock`을 쓰는 게 나은지.
3. **[§1이 발견한 별개 문제, 이 설계 범위 밖으로 제안]**
   `AcceptFromChannel`/`DestroyChannel`은 이 수정 이후에도 여전히
   "그 채널을 만든 게 아닌 다른 프로세스"가 호출해도 막을 방법이
   없다(`Channel`에 소유자 필드 자체가 없음) - 즉 역참조 안전성은
   고쳐지지만 **권한(누가 Accept/Destroy할 수 있는가)은 여전히
   무제한**이다. 이걸 이번에 같이 다룰지, 별도 계획(예: `Channel`에
   `ownerProcess` 필드 추가 + `kResolveOwnedBridge`류 소유자 검증)
   으로 분리해 나중에 다룰지 확인 필요 - 현재 v1 스코프에서 의도된
   개방성(누구든 이름/ID만 알면 상호작용 가능하다는 IPC 설계 철학)
   인지, 아니면 실제 갭인지 판단이 서지 않아 미리 결정하지 않았다.

## 7. 요약

- **확정 제안**: §2의 `kResolveChannelId()`/`kAllocateChannelId()`/
  `kFreeChannelId()` 세대 태그 슬롯 테이블, §4/§5의 세 호출부 교체.
- **실측으로 확정**: §0 - `ChannelRead`/`Write`/`CloseBridge`는
  이미 안전(`PN-9CC66142`), 남은 취약 지점은 `ConnectChannel`/
  `AcceptFromChannel`/`DestroyChannel` 셋뿐.
- **확정 안 함**: §6의 세 질문(슬롯 상한, 조회 락 필요 여부,
  Accept/Destroy 권한 부재를 이번에 같이 다룰지).

