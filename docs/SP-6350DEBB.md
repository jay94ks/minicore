# epoll류 유저 영역 비동기 이벤트 다중화 — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-6350DEBB
  status: approved
  updatedAt: 2026-09-26T15:09:17.262Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# epoll류 유저 영역 비동기 이벤트 다중화 — 설계 제안

설계자 Opinion(`SP-94CD958D` 대상, 2026-09-26): "epoll류 유저 영역
비동기 지원을 구현할 계획과 설계안." - systemd(`sd-event`)를 포함해
대부분의 POSIX 이벤트 루프 기반 소프트웨어가 요구하는 `epoll_create`/
`epoll_ctl`/`epoll_wait`류 fd 다중화 API에 대한 설계 제안이다.

## 1. 실측 확인한 현재 상태 - 커널 내부엔 이미 유사한 것이 있다

- **`Syscall::waitForAnyOf()`**(`SP-04EE2A18`, `PN-BCE6CFF3` completed) -
  "여러 제출된 비동기 작업 중 하나가 끝날 때까지 대기"를 이미
  제공한다. 다만 이건 **한 번의 syscall 호출에 제출된 고정 집합**을
  기다리는 모델이다 - epoll처럼 "감시 집합을 동적으로 추가/제거하며
  여러 번 반복 대기"하는 **영속적인 인스턴스** 개념이 없다.
- **`AsyncReactor::submitCompletion()`**(`SP-F682B889`) - 어떤
  비동기 작업이 끝나면 그 제출자를 깨우는 커널 내부 메커니즘. 이미
  Channel accept/read, VFS Read/Write, 디버그 syscall 등 이 커널의
  거의 모든 블로킹 syscall이 이 경로로 완료를 통지받는다 - **"fd가
  준비됐다"는 이벤트의 실제 발생원은 이미 전부 이 경로를 탄다.**
- **`SP-2602CAA6`(커널 이벤트 발행/구독, approved)** - 토픽 기반
  멀티캐스트 pub/sub. `WaitEvent(topic)`는 **단일 토픽**만 기다린다 -
  이것도 "여러 fd를 동시에" 감시하는 모델이 아니다. 다만 이 문서가
  제안하는 "fd별 준비(ready) 신호를 퍼블리시한다"는 아이디어는
  이 프레임워크의 발행/구독 모델과 구조적으로 잘 맞는다(§3).
- **fd 테이블**(`SP-2AAD7C8D` §9.2, `Process::FileDescriptor`) -
  이미 존재 - epoll이 감시할 대상(정수 fd)의 네임스페이스.

**결론**: 새 이벤트 발생 메커니즘을 만들 필요는 없다 - 이미 있는
"완료 통지" 배관(`AsyncReactor::submitCompletion`)에 **영속적인 감시
집합(epoll 인스턴스)**과 **레벨 트리거 준비 상태 조회**라는 두 계층만
얹으면 된다.

## 2. [정정, 2026-09-26, 설계자 Opinion] edge 트리거도 v1 범위 - 최초안의 보류 결정 철회

**최초안은 edge 트리거(`EPOLLET`)를 v1 범위 밖으로 미루자고
제안했으나, 설계자가 "edge 트리거를 지금 단계에서 고려해야해"로
명시적으로 철회했다.** 이 문서는 이제 레벨 트리거와 edge 트리거
둘 다 v1에서 설계한다 - §4/§5를 그에 맞춰 갱신했다(아래).

**왜 최초안이 틀렸었나(자기 반성, 참고용)**: edge 트리거가
"매 상태 전이를 놓치지 않고 추적해야 하는 훨씬 엄격한 구현"이라는
평가 자체는 맞지만, 이 커널의 완료 통지 배관(§1의
`AsyncReactor::submitCompletion`)이 이미 **본질적으로 전이(edge)
사건**이다 - "새 데이터가 도착했다"/"큐가 비어 있다가 채워졌다"는
통지 자체가 레벨이 아니라 전이를 알리는 이벤트다. 즉 edge 트리거는
새로 만들어야 하는 무거운 기능이 아니라, **이미 있는 전이 이벤트에
"직전에 이미 보고했는지" 상태만 하나 더 붙이면 되는 문제**였다 -
과대평가였다.

## 2-A. 레벨 vs edge 트리거 - 공통 배관 위의 두 소비 방식

```cpp
enum class EpollTriggerMode : uint32_t {
    Level = 0,  // 기본값, POSIX select/poll과 동일
    Edge  = 1,  // EPOLLET 대응
};

struct EpollWatch {
    int64_t targetFd = -1;
    uint32_t interestMask = 0;
    uint64_t userData = 0;
    EpollTriggerMode trigger = EpollTriggerMode::Level;
    // edge 모드 전용 - "이 워치가 마지막으로 보고한 방향별 상태".
    // level 모드는 이 필드를 안 씀(매번 §4로 직접 재조회).
    bool lastReportedReadable = false;
    bool lastReportedWritable = false;
};
```

- **레벨 모드**(§2가 원래 설계한 그대로): `EpollWait`가 매번 §4로
  현재 상태를 직접 재조회 - "이미 보고했더라도 여전히 준비 상태면
  또 보고한다."
- **edge 모드**: `lastReportedReadable`/`Writable`이 `false`인
  상태에서 상태 변화 콜백(§3-A, 신규)이 "지금 준비됨"을 확인하면
  **그 순간에만** epoll 인스턴스의 준비 목록에 추가하고
  `lastReported*`를 `true`로 세운다 - 이미 `true`인 상태에서 같은
  방향의 준비 신호가 또 와도 **다시 보고하지 않는다**(POSIX
  edge 트리거의 핵심 계약). `lastReported*`는 그 fd가 실제로
  "준비 안 됨" 상태로 되돌아갔다고 판단되는 시점(예: 소켓 큐를
  끝까지 읽어 비게 됨, §4의 즉시 판정 함수가 다시 `false`를 반환하는
  걸 그 fd의 다음 작업 완료 콜백이 확인)에 `false`로 리셋된다.

## 2-B. 왜 "완료 콜백에서 즉시 판정"이 필요한가 (레벨 모드와의 핵심 차이)

레벨 모드는 `EpollWait`가 깨어난 시점에만 §4를 조회해도 충분하다
(POSIX 계약상 "여전히 준비돼 있으면 또 알려줘도 됨"이므로 조회
시점이 늦어도 손해가 없다). **edge 모드는 그렇지 않다** - "준비→
안 준비→준비"가 `EpollWait` 호출 사이에 여러 번 일어날 수 있고,
그 각각의 상승 엣지를 놓치면 안 된다(POSIX edge 트리거 계약 -
`EpollWait`을 늦게 불러도 그 사이의 전이 자체는 유실되면 안 됨).
따라서 **edge 모드로 감시 중인 워치는, 그 대상 fd의 완료 통지
콜백(§1의 `submitCompletion` 소비 지점, 예: `Channel`의
`AsyncTaskWaitQueue`가 새 데이터를 큐에 넣는 지점) 자체에서 즉시
§4 재판정 + `lastReported*` 비교를 수행**해야 한다 - `EpollWait`
호출 여부와 무관하게, 그 전이가 일어나는 즉시 epoll 인스턴스의
준비 목록에 반영된다(`EpollWait`은 나중에 그 목록을 꺼내 갈 뿐).
이는 §3(발행 로직)의 "완료 시점에 구독자 목록을 순회해 통지"
모델과 정확히 같은 지점에 훅을 거는 것이라 새 통지 배관이 필요
없다 - **구독자(epoll watch)가 그 통지를 받는 시점에 레벨 재계산 +
전이 판정을 끼워 넣는 것**뿐이다.

## 2-C. 남은 실질적 난이도 - 정확한 "전이 판정 지점"은 fd 종류마다 다르다

edge 모드가 실제로 어려운 지점은 "새 데이터 도착"(상승 엣지, 위
2-B가 다루는 것)이 아니라 **"준비 안 됨으로 돌아가는 시점"(하강
엣지, `lastReported*`를 다시 `false`로 되돌리는 시점)을 정확히
포착하는 것이다 - 이건 fd 종류별 §4 판정 함수 근처에 위치한
소비 지점(예: 소켓 `Read` 핸들러가 큐를 마지막 항목까지 비웠을 때)
에서 판정해야 한다. **이 세부는 순수 구현 세부로 남긴다**(각 fd
종류의 §4 판정 함수를 만드는 착수 세션이, 그 종류의 소비 지점에서
"방금 큐가 비었다"를 알 수 있는 지점을 찾아 `lastReported*` 리셋을
끼워 넣는다 - RM-23F4B687 §4, 종류마다 다른 자료구조 세부까지
이 설계 문서가 미리 확정할 필요는 없음).

## 2-D. [갱신, 2026-09-26, 설계자 Opinion] EPOLLONESHOT - v1 범위로 편입

**최초안은 "구체적 소비자 없음"을 이유로 범위 밖으로 뒀으나,
설계자가 "구체적 소비자가 없더라도 포함해"로 명시적으로 지시했다** -
이 문서는 이제 `OneShot`을 v1에서 설계한다.

의미: 한 번 이벤트가 보고되면(레벨/edge 어느 모드든) 그 워치는
`EpollWatch::armed = false`가 되어 **다시는 보고되지 않는다** - 다시
보고받으려면 호출자가 명시적으로 `EpollCtl(Mod, ...)`를 불러 그
워치를 재무장(armed=true)해야 한다. 구현은 §2-A/§5의 기존 보고
경로(레벨 스캔 또는 edge 전이 판정) 바로 앞에 `if (!watch.armed)
continue;` 검사 하나, 보고 직후 `OneShot` 비트가 서 있으면
`watch.armed = false` 대입 하나만 추가하면 된다 - 새 자료구조
불필요, 기존 배관에 조건 분기만 얹는 문제였다(edge 트리거와 같은
성격의 과대평가였음, §2 자기반성 참고).

## 2-E. [갱신, 2026-09-26, 설계자 Opinion] EPOLLEXCLUSIVE - v1 범위로 편입

설계자 지시로 v1 범위에 포함한다. Linux의 실제 의미: 같은 대상
fd가 **여러 개의 서로 다른 epoll 인스턴스**(또는 같은 인스턴스를
공유하는 여러 대기자)에 각각 `Exclusive` 비트로 등록돼 있을 때,
그 fd에 이벤트가 발생하면 **등록된 모든 워치가 아니라 그중 정확히
하나만** 깨운다(thundering herd 방지 - 여러 워커 프로세스가 같은
리슨 소켓을 나눠 갖는 멀티프로세스 서버 패턴이 전형적 소비자).

- **자료구조**: 대상 fd(예: 소켓)마다 "그 fd를 `Exclusive`로 감시
  중인 워치들의 목록"을 별도로 관리해야 한다 - 완료 통지 배관(§1)이
  구독자 목록을 순회해 통지할 때, **`Exclusive`가 아닌 워치는
  전부** 통지하고, **`Exclusive`인 워치들 중에서는 정확히 하나만**
  (라운드로빈 포인터 또는 목록의 첫 항목 - 착수 세션이 정함, 순수
  구현 세부) 선택해 통지한다.
- **`Exclusive`가 아닌 일반 워치와 공존 가능** - Linux 문서의 제약
  (`EPOLLEXCLUSIVE`는 `EPOLLONESHOT`/레벨 트리거와 함께 쓸 때 주의가
  필요하다는 것)을 그대로 따른다: `Exclusive`+레벨 트리거 조합은
  "하나만 깨움 + 그 하나가 다 처리 못 하면 레벨이 계속 유지돼 다음
  스캔에 다시 걸릴 수 있다"는 동작이 되므로, 진짜 워커 분산 목적이면
  `Exclusive`+`EdgeTriggered`(또는 +`OneShot`) 조합을 쓰는 게
  일반적이다 - v1은 이 조합 제약을 강제로 검사하지 않고 POSIX와
  동일하게 호출자 책임으로 둔다.

## 3. 핵심 모델

```cpp
// minicore/kernel/epoll.h (신규 파일 제안)
namespace kernel {

enum class EpollEventMask : uint32_t {
    Readable = 1u << 0,   // EPOLLIN 대응
    Writable = 1u << 1,   // EPOLLOUT 대응
    Error    = 1u << 2,   // EPOLLERR 대응(항상 암묵적으로 감시됨, POSIX와 동일)
    EdgeTriggered = 1u << 3,  // EPOLLET 대응 - §2-A/§2-B/§2-C
    OneShot  = 1u << 4,   // EPOLLONESHOT 대응 - §2-D
    Exclusive = 1u << 5,  // EPOLLEXCLUSIVE 대응 - §2-E
};

struct EpollWatch {
    int64_t targetFd = -1;
    uint32_t interestMask = 0;   // EpollEventMask 비트합
    uint64_t userData = 0;       // epoll_event.data 대응 - 그대로 되돌려줌
    // edge 모드 전용 상태(§2-A) - level 모드는 안 씀.
    bool lastReportedReadable = false;
    bool lastReportedWritable = false;
    // OneShot 전용 상태(§2-D) - 이벤트 보고 후 false, EpollCtl(Mod)로만 true 복귀.
    bool armed = true;
};

class EpollInstance {
public:
    Spinlock lock;
    ChunkedList<EpollWatch, 16> watches;
    // 레벨 트리거 판정 결과를 캐시하지 않는다(§4) - EpollWait이 매번
    // 각 watch의 현재 준비 상태를 직접 조회한다(폴링처럼 보이지만
    // 실제로는 "블로킹 대기 + 깨어난 뒤 한 번 스캔"이라 바쁜 대기가
    // 아니다 - 아래 §5 대기 메커니즘 참고).
};

}  // namespace kernel
```

- **`EpollInstance`도 fd 테이블에 들어간다** - `epoll_create()`가
  반환하는 것도 평범한 fd다(POSIX와 동일 - epoll fd 자신도 다른
  epoll에 감시될 수 있다).

## 3-A. [갱신, 2026-09-26, 설계자 Opinion] 중첩 epoll - v1 범위로 편입, 실제 구현+검증 대상

최초안은 "구조상 가능해 보이나 검증 안 함"으로 미뤘으나, 설계자가
명시적으로 구현+검증 범위에 포함하라고 지시했다.

**설계**: `EpollInstance` 자신이 §4의 fd 종류별 판정 함수 목록에
새 종류로 추가된다:

```cpp
bool kIsFdReadable(const FileDescriptor& fd) {
    switch (fd.kind) {
        // ...기존 분기...
        case FileDescriptor::Kind::Epoll:
            // 이 epoll 인스턴스의 준비 목록(레벨/edge 스캔 결과)이
            // 비어 있지 않으면 "readable" - epoll fd 자신은
            // Readable만 의미 있다(Writable/Error는 항상 false).
            return !static_cast<EpollInstance*>(fd.payload)->readyWatches.empty();
    }
}
```

- **완료 통지 전파(중첩의 핵심)**: epoll-인스턴스-B가 epoll-fd-A를
  `EpollCtl(Add)`로 감시하면, §5-1의 "완료 통지 지점에 구독자로
  등록" 원칙이 그대로 적용된다 - **A의 준비 목록에 새 항목이
  추가되는 지점 자체가 A의 "완료 통지 지점"이 된다**(§1의 일반
  원칙 - 이 fd 종류의 상태가 바뀌는 지점에서 구독자에게 알린다).
  즉 A에 감시 중인 어떤 fd가 준비되면, A의 준비 목록에 그 항목이
  들어가는 순간 B도 함께 통지받는다 - **새 전파 메커니즘이 아니라
  같은 배관을 한 단계 더 얹어 재귀적으로 쓰는 것**(§1/§2의 "이미
  있는 배관 재사용" 원칙과 완전히 일치).
- **깊이 제한**: 순환 참조(A가 B를 감시하는데 B도 A를 감시)는
  무한 통지 루프를 만들 수 있다 - `EpollCtl(Add)` 시점에 감시
  대상이 epoll fd이면 그 대상의 감시 목록을 재귀적으로 따라가
  자기 자신이 나오는지 확인해 거부한다(`InvalidArgument`,
  POSIX epoll도 자기 자신을 직접 감시하는 것은 막는다 - 순환
  전체를 따라가는 검사는 이 프로젝트가 조금 더 보강한 것).

## 4. "준비됨(ready)"의 판정 - 대상 fd 종류별 조회 함수

모든 fd 종류가 이미 "완료를 통지받는" 경로(§1)를 갖고 있으므로,
"지금 이 순간 준비됐는가"를 **동기적으로 즉시 판정하는** 함수 하나만
fd 종류마다 추가하면 된다(새 이벤트 배관 불필요):

```cpp
// 기존 FileDescriptor::Kind 분기(Open/Read/Write 핸들러가 이미 쓰는
// 것과 동일한 분기 테이블)에 조회 함수를 추가하는 형태로 구현.
bool kIsFdReadable(const FileDescriptor& fd);  // Readable 판정
bool kIsFdWritable(const FileDescriptor& fd);  // Writable 판정
```

- **소켓(§SP-231493CB)**: Stream 리슨 소켓은 accept 대기 큐가
  비었는지, 연결된 소켓은 Channel의 수신 큐가 비었는지로 판정 -
  `Channel`이 이미 갖고 있는 큐 길이 조회(`approxLength()`류, 기존
  `AsyncTaskQueue`/Push-Pull 로드밸런싱이 이미 쓰는 패턴,
  `PN-2CD26587` 참고)를 그대로 재사용.
- **일반 파일(ext4/FAT32 등 `MountKind::KernelDriver`)**: POSIX
  관례 그대로 **항상 Readable+Writable**(디스크 파일은 블로킹
  개념이 없음 - `select`/`poll`/`epoll` 전부 이렇게 취급).
- **Channel(소켓이 아닌 원 IPC 채널)**: 소켓과 동일한 판정(사실
  소켓 자체가 Channel 파사드이므로 §SP-231493CB가 완성되면 코드
  경로가 겹친다).

## 5. syscall API 및 대기 메커니즘

새 그룹 대신 **그룹 6(Event, `SP-2602CAA6`)의 다음 미사용 call
번호(3)부터** 이어 붙인다 - "무언가 준비될 때까지 대기한다"는 이
그룹의 기존 취지(`WaitEvent`)와 본질적으로 같은 문제라 새 그룹을
만들 근거가 약하다(RM-23F4B687 §4 - 과도한 그룹 분리 방지).

```
EpollCreate() -> fd(int64_t)
EpollCtl(epfd, op: EpollCtlOp[Add|Mod|Del], targetFd, mask, userData) -> error
EpollWait(epfd, outEvents: EpollEvent* [유저 메모리], maxEvents, timeoutMs) -> count(int64_t)
```

**대기 구현**: `EpollWait`이 그냥 `while(!ready) yield()`로 바쁜
대기하면 이 커널이 이미 여러 차례 겪은 "무조건적 busy-poll이
스케줄러를 굶긴다"는 계열의 함정(`DC-5F0AC0D3`/`PN-F2594E93`류)을
그대로 반복하게 된다 - **그 대신 §1의 기존 완료 통지 배관에 올라탄다**:

1. `EpollCtl(Add)` 시점에, 감시 대상 fd의 종류별 "완료 통지 지점"
   (예: Channel의 `AsyncTaskWaitQueue`, VFS의 `AsyncReactor::
   submitCompletion` 호출 지점)에 **이 EpollInstance를 추가
   구독자로 등록**한다 - 기존 소비자(원래 그 fd를 직접
   Read/Accept로 기다리던 제출자)를 방해하지 않고 "누군가 더
   깨어나야 한다"는 신호만 추가로 받는다(멀티캐스트 - 이미
   `SP-2602CAA6`이 정립한 패턴 그대로).
2. `EpollWait`은 먼저 §4의 즉시 판정 함수로 감시 목록 전체를
   한 번 스캔한다 - 이미 준비된 게 있으면 즉시 반환(POSIX와 동일,
   레벨 트리거는 "이미 준비된 상태"도 매번 다시 알려야 함).
3. 아무것도 준비 안 됐으면, 위 1번에서 구독해 둔 알림 중 **아무거나
   하나**가 올 때까지 블로킹한다 - 이는 정확히 `Syscall::
   waitForAnyOf()`(§1)가 이미 푼 문제이므로 그 구현을 그대로
   재사용한다(감시 중인 fd 개수만큼의 `AsyncTask` 핸들을
   `waitForAnyOf`에 동적으로 구성해 넘기는 방식 - 정확한 자료구조는
   착수 세션이 구체화, 순수 구현 세부).
4. 깨어나면 다시 §4 스캔을 반복(허위 각성 가능 - 그 알림을 유발한
   fd가 다른 대기자에게 먼저 소비돼 이제 준비 안 된 상태일 수 있음,
   POSIX epoll도 허위 각성을 배제하지 않음).
5. `timeoutMs`는 `SP-F15B4A63`(지연 실행/타이머 인프라)의 기존
   타이머를 `waitForAnyOf`의 대기 집합에 함께 제출해(기존
   "타임아웃 있는 대기" 패턴, `SP-04EE2A18`가 이미 확립) 구현한다.

## 6. 정리

`EpollCtl(Del)` 또는 `Close(epfd)` 시 §5-1의 구독을 해제한다 -
`SP-2602CAA6` §8의 "명시적 해제 + 프로세스 종료 시 자동 정리"
관례를 그대로 따른다. 감시 대상 fd 자신이 먼저 닫히면(다른 경로로)
그 watch를 조용히 무효화(다음 `EpollWait`에서 `EPOLLHUP`류 상태로
보고 - 정확한 에러 코드는 착수 세션이 POSIX 관례 대조 후 확정).

## 7. [갱신, 2026-09-26, 설계자 Opinion] 범위 밖 (v1) - 세 항목 모두 v1로 편입, 새 범위 밖은 하나

**[정정]** 이 절이 원래 범위 밖으로 뒀던 세 항목
(`EPOLLONESHOT`/`EPOLLEXCLUSIVE`/중첩 epoll)은 전부 설계자 지시로
v1 범위에 편입됐다 - §2-D/§2-E/§3-A 참고. 남는 범위 밖 항목:

- `signalfd`/`timerfd`를 epoll이 감시하는 것 - **설계 자체는 이제
  범위 안이지만 별도 문서로 분리됐다**(설계자 지시, "timerfd류 fd도
  별도 설계안으로 작성해") - `SP-A7479F83`("timerfd/signalfd - epoll
  호환 타이머/시그널 fd — 설계 제안")가 그 설계를 다룬다. 이 문서
  (`SP-6350DEBB`)는 §4의 fd 종류별 판정 함수 목록에 그 문서가 정의할
  `Kind::Timerfd`/`Kind::Signalfd`를 추가하는 지점만 열어 둔다 -
  `SP-A7479F83`가 승인되면 그 두 종류를 §4 목록에 실제로 추가한다.

## 8. 검증 계획(초안)

1. `EpollCreate`+`EpollCtl(Add, 소켓fd, Readable)` 후 다른 프로세스가
   그 소켓에 데이터를 쓰면 `EpollWait`이 즉시 깨어나 올바른
   `userData`/이벤트 마스크를 반환하는지 확인.
2. 이미 준비된 상태에서 `EpollWait` 호출 - 블로킹 없이 즉시 반환
   (레벨 트리거 §2-A/§5-2).
2-A. **[신규] edge 트리거**: `EdgeTriggered` 비트로 워치 등록 후
   데이터 도착 → 첫 `EpollWait`은 보고, 데이터를 다 안 읽은 채로
   (여전히 레벨상 준비 상태) 다시 `EpollWait` 호출 - **보고 안 함**
   (레벨 모드와 정확히 반대로 동작함을 확인, §2-A의 핵심 계약).
   이후 데이터를 마저 읽어 큐가 비었다가 새 데이터가 다시 도착하면
   그제서야 다시 보고되는지(하강→상승 엣지 재검출, §2-C) 확인.
3. 감시 대상이 여러 개일 때 그중 하나만 준비돼도 정확히 그것만
   보고하는지(다른 준비 안 된 fd는 결과에 안 섞임).
4. `timeoutMs` 만료 시 0개 반환(에러 아님, POSIX와 동일).
5. `EpollCtl(Del)`/`Close` 후 그 fd의 이벤트가 더 이상 이 인스턴스에
   영향 안 주는지.
6. **[신규] OneShot(§2-D)**: `OneShot` 비트로 등록 후 첫 이벤트는
   보고, 상태가 계속 준비돼 있어도 재무장(`EpollCtl(Mod)`) 전까지
   다시는 보고 안 하는지 확인.
7. **[신규] Exclusive(§2-E)**: 같은 fd를 `Exclusive`로 감시하는
   epoll 인스턴스 2개 이상을 만들고, 그 fd에 이벤트 발생 시 정확히
   하나의 인스턴스만 깨어나는지(둘 다 깨어나면 실패) 확인.
8. **[신규] 중첩 epoll(§3-A)**: epoll-B가 epoll-A를 감시, A가 감시
   중인 소켓에 데이터가 도착하면 B의 `EpollWait`도 깨어나 A의 fd를
   Readable로 보고하는지. 자기 자신을 감시하려는 시도가
   `InvalidArgument`로 거부되는지도 확인.
9. 표준 5시나리오 무회귀.

## 참고
- `SP-94CD958D` - 이 설계를 요청한 systemd 포팅 사전조사.
- `SP-231493CB` - 소켓 계층 설계 - epoll의 첫 실사용 대상(§4).
- `SP-04EE2A18`/`PN-BCE6CFF3` - `waitForAnyOf()`, 이 설계의 대기
  메커니즘(§5)이 재사용하는 기존 구현.
- `SP-2602CAA6` - 이벤트 발행/구독 - 그룹6 call 번호 재사용(§5),
  구독/정리 관례(§6)의 선례.
- `SP-F682B889` - `AsyncReactor`/`AsyncTask`, 완료 통지 배관 원 설계.
- `SP-2AAD7C8D` §9 - fd 테이블, epoll fd 자신도 이 테이블에 통합(§3).

