# Channel/BridgeHandle 안전한 핸들 해석 — 세부 설계 (PN-CE6A04AB)

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-CA3C3E57
  status: approved
  updatedAt: 2026-09-17T02:41:41.934Z
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

constexpr uint32_t kMaxChannelTableSlots = 65536;  // [확정, 2026-09-17,
// QU-1AF2C16B 답변] "채널의 전역 상한은 64K" - 64*1024 = 65536(인덱스
// 0..65535, uint16_t 전체 범위를 정확히 채움). SP-9CB55C5B의
// UINT16_MAX(65535)와는 1 차이지만 설계자가 이번엔 "64K"로 명시했으므로
// 그 문구 그대로 65536을 쓴다.

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

1. **[확정, 2026-09-17, QU-1AF2C16B 답변]** `kMaxChannelTableSlots`
   - 설계자 답변: "채널의 전역 상한은 64K." - 제안된
   `UINT16_MAX`(65535, §2)를 그대로 확정(≈64K, `SP-9CB55C5B`의
   `kMaxProcessTableSlots`와 같은 값 재사용 판단이 승인됨).
2. **[미확정 - 착수 세션 판단]** §3에서 "테이블 조회 자체엔 락이
   불필요하다"고 판단했으나, 이 질문 자체는 QU-1AF2C16B 답변에서
   명시적으로 다뤄지지 않았다 - 착수 세션이 §3의 논증(발급/해제만
   드문 쓰기, 조회는 generation 비교만 - 최악의 경우도 "못 찾음"으로만
   안전하게 실패)을 근거로 직접 판단해 구현한다(RM-23F4B687 §4 -
   임의로 여기서 재확정하지 않음).
3. **[확정, 2026-09-17, QU-1AF2C16B 답변]** Accept/Destroy 권한 부재
   - 설계자 답변: **"Channel은 항상 소유자가 있어야해. 만든놈은
   있는데 소유자가 없다는건 말이 되면 안돼. Bridge도 마찬가지야.
   채널의 전역 상한은 64K. 만든놈들에게 관리 책임을 넘겨."** - §1의
   \"의도된 개방성 아니면 실제 갭\" 질문에 대한 답이 명확히
   **실제 갭**으로 확정됐다. `Channel`에 `ownerProcess`(또는 동급)
   필드를 신설해 생성자 프로세스를 기록하고, `AcceptFromChannel`/
   `DestroyChannel`이 `kResolveOwnedBridge`(§1이 이미 확인한 `Bridge`
   쪽의 기존 패턴)와 대칭되는 소유자 검증을 거치도록 해야 한다 -
   "Bridge도 마찬가지"는 `Bridge`가 이미 `Process::openBridges`로
   이 패턴을 갖고 있음(§1 참고)을 재확인한 것이지 Bridge에 새로
   뭔가를 추가하라는 뜻이 아니다. **후속 작업으로 분리**: 이 문서
   본문(§2/§4/§5)은 이미 승인 대기 중인 세대 태그 슬롯 테이블 자체를
   바꾸지 않고, 오너십 필드+검증은 별도 구현 단계(착수 세션이 PN
   등록, CLAUDE.md 규칙 7)로 다룬다 - `Channel` 구조체에 필드 하나
   추가 + 두 핸들러 앞단에 소유자 비교 한 줄이라 이 설계의 세대
   태그 테이블 골격과 충돌하지 않는다.

### 6.1 소유권 필드 구현 방식 — 왜 `WeakPtr<Process>`를 바로 못 쓰는가

**[추가, 2026-09-17, minicore-88]** 후속 구현 단계로 넘기기 전에
실제 구현 방식 하나를 미리 확정해 둔다 - 처음 떠오르는 방향은
`Channel`에 `WeakPtr<Process> owner`를 추가하는 것이다(`Process::
parent`가 이미 이 패턴). 그런데 `Channel`은 `kCreateNamedChannel`
에서 `GenericSlabAllocator::alloc()` + 명시적 `init()`으로만
준비되고 **실제 C++ 생성자/소멸자를 거치지 않는다**(channel.cpp:
147-154, `DestroyChannelHandler`도 `GenericSlabAllocator::free()`만
불러 `~Channel()`은 호출되지 않는다). `WeakPtr<T>::operator=`는
대입 전에 `if (_block) _block->releaseWeak()`로 **현재 값**을 먼저
정리하는데, `init()`에서 `owner = WeakPtr<Process>()`처럼 대입하면
그 시점 `_block`이 슬랩 재사용으로 남은 쓰레기 값일 수 있어 임의
주소에 `releaseWeak()`를 호출하는 새 미정의 동작을 만든다(placement
new 없이는 안전하지 않음 - `SP-1DB13F61`이 vtable 타입에 대해 이미
지적한 것과 같은 종류의 문제, 이번엔 가상 함수가 아니라 WeakPtr의
비trivial 대입 연산자가 원인). `Channel`을 `BridgePipe`처럼 완전한
`SharedPtr`/`EnableSharedFromThis` 관리로 마이그레이션하면(placement
new로 실제 생성자를 거치게) 근본적으로 해결되지만, 이건
`kCreateNamedChannel`/`DestroyChannelHandler` 전체의 할당/해제
경로를 다시 짜는 훨씬 큰 작업이라 이 취약점 수정 PN의 범위를
넘는다.

**채택 — v1은 raw `Process*` 포인터 동일성 비교만**: `Channel::
ownerProcess`를 **raw `Process*`**로 추가하고 **절대 역참조하지
않으며 포인터 값 비교로만** 쓴다(`kProcessFromSubmitter(task).get()
== channel->ownerProcess`). 이건 이 문서 §2가 막 고친 것과 같은
종류의 위험(포인터 재사용/ABA)을 아주 좁은 범위로 다시 들여오는
것이지만 결정적 차이가 있다 - **역참조가 전혀 없으므로 최악의
경우도 "잘못된 프로세스가 권한이 있다고 오판"(권한 오판)에
그치고, §2가 막던 것(임의 주소 역참조로 커널 패닉/손상)과는 심각도가
다르다**. `Process`는 좀비로 회수 대기 중엔 구조체가 살아있고
(`wait()`로 reap되기 전까지), 그 슬랩 슬롯이 곧바로 다른 새
프로세스에 재사용될 확률도 낮아 실무적 위험은 작다고 판단한다.
완전히 닫으려면 위의 `SharedPtr` 마이그레이션이 필요 - 별도 후속
계획으로 분리한다(§7 참고).

적용 지점: `channel.h`에 `Process* ownerProcess = nullptr;` 필드 +
`init()`에서 `nullptr`로 리셋. `OpenChannelHandler::onExec`에서
`channel->ownerProcess = kProcessFromSubmitter(task).get();`.
`AcceptFromChannelHandler::onExec`/`DestroyChannelHandler::onExec`
에서 `kResolveChannelId()` 성공 직후, 실제 로직 전에 `caller.get()
!= channel->ownerProcess`면 `ChannelError::PermissionDenied`(이미
존재하는 값)로 거부. `ConnectChannel`은 소유자 검증 대상이 아니다
(§1에서 이미 확인 - 연결은 원래 남이 만든 채널에 하는 것이 정상).

## 6-A. `DontDeref<T>` - 신원 비교 전용 포인터 래퍼 (신설, 2026-09-17,
설계자 의견)

> "이걸 보다가 든 생각인데, 단순 포인터 비교용으로 들고 있을거라면
> `DontDeref<T>`를 설계하고 wrapping해서 역참조를 절대 하면 안된다고
> 못박도록 해."

§6-3이 확정한 `Channel::ownerProcess`(§2의 `ChannelTableSlot::ptr`과
성격이 다르다 - 그건 실제로 역참조해 `Channel` 멤버에 접근해야 하는
"진짜 포인터"지만, `ownerProcess`는 **"호출자의 `Process*`와 같은가"만
비교**하면 되고 그 자체를 역참조할 일이 없다, §4/§5가 이미 호출자
식별은 항상 `submitterTask.lock()`으로 별도로 얻고 있음)가 정확히
이 패턴이다 - 과거 `AcceptFromChannel`류의 raw pointer 역참조 취약점
(§0/이 문서 전체의 발단)과 `PN-C4611402`(Channel onCancel 댕글링
포인터) 둘 다 "역참조하면 안 되는/안전하지 않은 포인터를 실수로
역참조"에서 비롯된 만큼, 타입 시스템으로 원천 차단하자는 제안이다.

```cpp
// minicore/libs/libkenv/shared_ptr.h (제안 - SharedPtr/WeakPtr와 같은
// "포인터류 유틸" 파일에 추가, 새 파일을 만들 만큼 크지 않음)
template <typename T>
class DontDeref {
public:
    DontDeref() = default;
    explicit DontDeref(T* ptr) : ptr_(ptr) {}

    bool operator==(const DontDeref& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const DontDeref& other) const { return !(*this == other); }
    explicit operator bool() const { return ptr_ != nullptr; }

    // 의도적으로 없음: operator*, operator->, T*로의 암시적/명시적
    // 변환 연산자 전부 - 이 값은 신원 비교(==/!=/bool)만 허용되고
    // 역참조는 컴파일 타임에 막힌다(멤버로 raw T* 자체가 없어
    // reinterpret_cast 없이는 꺼낼 방법이 없음).

private:
    T* ptr_ = nullptr;
};
```

**적용 대상**: `Channel::ownerProcess`(`PN-18FDBFF3`, 지난 틱 등록)를
`Process*` 대신 `DontDeref<Process>`로 선언 - 소유자 검증은
`channel->ownerProcess == DontDeref<Process>(callerProcess)`(또는
동급 비교)로만 이뤄지고, `ownerProcess`를 통해 `Process`의 멤버에
접근하는 코드는 애초에 컴파일되지 않는다. **일반화 여부(미결)**:
이 제안은 지금 발견된 `ownerProcess` 하나를 위해 `DontDeref<T>`를
`shared_ptr.h`에 범용 유틸로 추가하는 안이다 - 앞으로 "신원만
비교하고 절대 역참조하면 안 되는 포인터"가 또 나오면 같은 템플릿을
재사용한다(`kMakeSharedNew`류와 동일한 확장 패턴, `SP-1DB13F61`
참고). 착수 시 `RM-32D06563`(용어 및 개념)에 등록(CLAUDE.md 규칙 12).

## 7. 요약

- **확정 제안**: §2의 `kResolveChannelId()`/`kAllocateChannelId()`/
  `kFreeChannelId()` 세대 태그 슬롯 테이블, §4/§5의 세 호출부 교체.
- **실측으로 확정**: §0 - `ChannelRead`/`Write`/`CloseBridge`는
  이미 안전(`PN-9CC66142`), 남은 취약 지점은 `ConnectChannel`/
  `AcceptFromChannel`/`DestroyChannel` 셋뿐.
- **확정, 2026-09-17(QU-1AF2C16B)**: 슬롯 상한 65536(64K, §2 갱신
  완료) 확정. Accept/Destroy 권한 부재는 실제 갭으로 확정 -
  `Channel::ownerProcess`(raw 포인터, 동일성 비교 전용 - §6.1)
  신설 + 소유자 검증을 이 PN의 구현 범위에 포함한다.
- **여전히 미확정**: 테이블 조회 락 필요 여부 - 착수 세션 판단에
  위임(§3 원안대로 진행).
- **이 문서 범위 밖으로 분리**: `Channel`을 `BridgePipe`처럼 완전한
  `SharedPtr` 관리로 마이그레이션해 §6.1의 raw 포인터 동일성 비교
  (좁은 범위의 잔여 위험)까지 없애는 작업 - 별도 계획 등록 예정.

