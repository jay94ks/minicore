# 메시징 채널 IPC — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-1FBC0EEB
  status: review
  updatedAt: 2026-09-14T06:26:49.967Z
  갱신: node scripts/export-cnw-docs.mjs
-->
## 배경

설계자 지시(2026-09-14):

> "채널을 개설하면 커널이 커널 내에서 핸들과 유효한 채널 ID를
> 반환한다. 혹은, 프로세스가 본인이 원하는 `이름`으로 채널을 개설한다.
> 채널 ID를 공유할 대상 프로세스에게 어떻게든 전달을 한다. (클라이언트가)
> connectChannel로 연결을 시도하면, (서버가) acceptFromChannel로
> 접속자를 수락한다 - 수락 실패도 고려. handshake가 완료되면 브릿지
> 파이프를 반환한다(양쪽 다). 용도가 끝나면 closeChannel로 닫는다.
> 데이터는 recv/send로 주고받는다."

관련 기존 결정:

- **SP-8B6B8D25 §2** - 커널의 책임 중 "3. IPC를 해석하여 메시징
  채널로 커널 서비스들에게 전파"와 "10. 공유 메모리 및 프로세스간
  메시징(IPC와 별도로 존재하는 채널)" - 이 두 항목이 가리키는 실체가
  바로 이 채널 IPC다(용어가 "IPC"/"메시징 채널"로 섞여 쓰였을 뿐,
  하나의 메커니즘으로 통합해 다룬다 - §"이름 정리" 참고).
- **DS-D4E5C451** - IPC 메시지 포맷은 **raw binary**로 이미 확정 -
  `send`/`recv`가 주고받는 데이터는 별도 직렬화 없이 그대로의 바이트열.
- **DS-D4E5C451** - "프로세스간 공개 인터페이스 registry"는 **아직
  열린 설계 영역**으로 명시적으로 미뤄져 있다 - 이름 있는 채널의
  네임스페이스가 그 registry의 초기/부분 구현이 될 수 있다는 점을
  §"이름 있는 채널" 절에서 짚고 넘어가되, registry 자체를 이번에
  전부 설계하지는 않는다.
- **SP-04EE2A18(Syscall 디스패치 및 비동기 처리 서브시스템, 짝을
  이루는 제안)** - 아래 모든 API(`openChannel`/`connectChannel`/
  `acceptFromChannel`/`send`/`recv`/`closeBridge`)는 전부 그 제안의
  syscall endpoint 하나씩으로 구현된다 - 이 문서는 그 syscall들
  각각의 핸들러(`AsyncTaskHandler`)가 정확히 무엇을 해야 하는지를
  정의한다. **새 블로킹/큐잉 메커니즘을 따로 만들지 않는다** - 전부
  `AsyncTask`/`AsyncReactor`/스케줄러 블로킹 위에서 동작한다.

## 이름 정리 (제안 - 확인 필요)

설계자 지시 원문은 "채널"이라는 단어를 **두 가지 다른 객체**를
가리키는 데 섞어 쓴다:

1. 서버가 `openChannel`로 만드는, 클라이언트의 연결 요청을 기다리는
   **랑데부 지점**(이하 **`Channel`**).
2. handshake가 끝난 뒤 `connectChannel`/`acceptFromChannel`이 돌려주는,
   실제 데이터를 주고받는 **연결된 양방향 통로**(원문의 "브릿지
   파이프", 이하 **`BridgePipe`**).

그런데 원문 6번("브릿지 파이프의 용도가 더이상 남아있지 않을 때
`closeChannel`로 채널을 닫는다")은 **`BridgePipe`를 닫는데 함수 이름은
`closeChannel`**이라 - 두 객체와 이름이 어긋난다. 이 제안은 아래처럼
**이름을 객체에 맞게 분리**할 것을 제안한다:

- `closeBridge(bridge)` - `BridgePipe` 하나를 닫는다(원문의
  `closeChannel`에 대응, 이름만 교정).
- `Channel` 자체(랑데부 지점)는 **명시적으로 닫는 API를 이번 제안에
  넣지 않는다** - 이유는 §"Channel 수명" 참고. 필요해지면
  `destroyChannel`을 별도로 추가할 수 있다.

이 이름 교정은 API 동작을 바꾸지 않는다 - 순전히 명명 문제라 DC로
확인만 받고(§"결정이 필요한 사항" 1번) 진행한다.

## 핵심 객체

```cpp
namespace kernel {

using ChannelId = uint64_t;      // 커널 전역에서 유일
using BridgeHandle = uint64_t;   // 프로세스 로컬 핸들(파일서술자류)

// 랑데부 지점 - 서버 프로세스가 openChannel로 만든다. 실제 데이터는
// 안 오가고, "연결 요청 대기열"만 갖는다.
struct Channel {
    ChannelId id;
    ProcessId owner;                 // 이 채널을 만든 프로세스(=accept할 자격)
    // 이름 없이 개설했으면 비어 있음 - 있으면 커널 전역 이름 테이블의 키.
    kernel::string name;
    // connectChannel이 도착시켜 두고, acceptFromChannel이 하나씩
    // 꺼내가는 대기열(FIFO) - PL-2D3184BC 4단계의 TaskQueue와 같은
    // "침습적 포인터 + Spinlock" 패턴 재사용을 제안한다.
    /* PendingConnectRequest 큐 */
};

// handshake 완료 후 양쪽이 하나씩 갖는 연결된 반쪽 - 실제 실체는
// 커널에 있는 한 쌍(서버쪽/클라이언트쪽)의 raw binary 링버퍼 둘
// (각 방향 하나씩 - 전이중) 이다.
struct BridgePipe {
    BridgeHandle localHandle;
    ProcessId owner;         // 이 반쪽을 쥔 프로세스
    /* peer BridgePipe*로의 참조, 송신 링버퍼, 수신 링버퍼 */
};

}  // namespace kernel
```

## API 시퀀스와 각 syscall의 처리

모든 함수는 **SP-04EE2A18**의 `SyscallEndpointId` 하나씩이다 -
"블로킹"이라고 적은 것은 전부 "그 syscall의 `AsyncTaskHandler
::onExec`가 조건이 안 맞으면 `AsyncTask::yield()`로 대기하고, 조건이
맞으면 그 자리에서 완료 처리한다"는 뜻이다(직접 스케줄러를 건드리지
않는다 - Syscall 서브시스템이 이미 그 계층을 담당).

### 1. `openChannel(name: optional<string>) -> (ChannelId, handle)`

- `name`이 없으면: 새 `Channel`을 Slab/전용 풀에서 만들고 새
  `ChannelId`를 발급한다 - **블로킹 없음**(즉시 완료).
- `name`이 있으면: 커널 전역 이름 테이블에서 그 이름이 이미 쓰이고
  있는지 확인 - **있으면 즉시 실패**(정책은 §"이름 있는 채널" 참고),
  없으면 등록하고 위와 동일하게 진행.
- 반환된 `handle`은 이 프로세스가 나중에 `acceptFromChannel`을 부를
  때 쓰는 로컬 참조(파일서술자류) - `ChannelId`는 **다른 프로세스에게
  전달할 값**이다(원문 2번 - 전달 방법 자체는 이번 제안 범위 밖,
  §"Channel ID 전달"참고).

### 2. `connectChannel(target: ChannelId | name) -> BridgePipe | 실패`

- 대상 `Channel`을 `ChannelId`(또는 이름 조회 후 얻은 ID)로 찾는다 -
  **없으면 즉시 실패**(블로킹 없음).
- 있으면 `PendingConnectRequest`를 그 `Channel`의 대기열에 넣고
  **블로킹**(서버가 accept하거나, 명시적으로 거부하거나, 채널이
  없어질 때까지).
- 서버가 accept하면: 새 `BridgePipe` 쌍 중 클라이언트 쪽 반쪽을
  들고 깨어난다.
- 실패 사유(예): 채널을 찾을 수 없음, 채널 소유 프로세스가 이미
  종료됨(대기 중 또는 시도 시점), `BridgePipe` 쌍 할당 실패(자원
  고갈).

### 3. `acceptFromChannel(handle) -> BridgePipe | 실패`

- 이 프로세스가 소유한 `Channel`의 대기열이 비어 있으면 **블로킹**,
  하나 있으면 즉시 꺼낸다.
- 꺼낸 요청에 대해 `BridgePipe` 쌍을 할당(Slab, 각 방향 링버퍼 포함)
  하고, 서버 쪽 반쪽은 이 호출의 반환값으로, 클라이언트 쪽 반쪽은
  대기 중이던 `connectChannel` 호출을 깨워 전달한다(완료/응답 흐름은
  SP-04EE2A18과 동일한 메커니즘).
- **수락 실패 사유**(설계자가 명시적으로 고려하라고 지시): `handle`이
  유효하지 않음(즉시 실패), 이 프로세스 소유 채널이 아님(즉시 실패),
  `Channel` 자체가 그 사이 없어짐(예: 소유 프로세스가 같은 채널에
  대해 동시에 종료 처리 중 - 경쟁 상태 방지 필요), `BridgePipe` 쌍
  할당 실패(자원 고갈 - 이 경우 대기 중이던 `connectChannel`도 실패로
  깨워야 한다).

### 4. `send(bridge, data: raw bytes) -> 전송된 바이트 수 | 실패`

- 이 `BridgePipe`의 송신 링버퍼에 공간이 있으면 즉시 쓰고 완료 -
  꽉 차 있으면 **블로킹**(공간이 생기거나 상대가 `closeBridge`할 때
  까지).
- 상대가 이미 `closeBridge`한 반쪽이면 즉시 실패("broken pipe"류).

### 5. `recv(bridge, buffer, maxLen) -> 받은 바이트 수 | 실패`

- 수신 링버퍼에 데이터가 있으면 즉시 반환 - 비어 있으면 **블로킹**
  (데이터가 오거나, 상대가 `closeBridge`해서 더 이상 올 데이터가
  없다고 확정될 때까지 - 이 경우 0바이트 반환으로 "EOF"를 표현할지,
  실패로 표현할지는 §"결정이 필요한 사항" 3번).

### 6. `closeBridge(bridge)`

- 이 반쪽을 무효화하고, 상대 반쪽에 "피어가 닫혔음"을 통지한다(상대의
  대기 중인 `recv`/`send`를 깨워서 그 상태를 알게 함). 양쪽이 다
  닫히면 `BridgePipe` 쌍 자체(링버퍼 포함)를 회수한다.

## Channel ID 전달 (범위 밖 - 교차 참조만)

"채널 ID를 공유할 대상 프로세스에게 어떻게든 전달"(원문 2번)은 이
제안이 풀지 않는다 - 후보 경로(프로세스 생성 시 인자로 전달, 이미
연결된 다른 `BridgePipe`로 전달, 향후 "공개 인터페이스 registry"를
통한 발견)는 전부 **다른 설계 영역**(프로세스 생성/exec, registry -
둘 다 DS-D4E5C451이 이미 "아직 열린 설계"로 명시)에 속한다. **이름
있는 채널**(§"이름 있는 채널")이 그 중 가장 실용적인 즉시 사용
가능한 경로를 제공한다 - 이름을 미리 약속해 두면 `ChannelId`를
따로 전달할 필요가 없다.

## 이름 있는 채널

- 이름 테이블은 **커널 전역**(프로세스/사용자별 네임스페이스 분리
  없음 - 필요해지면 이름에 프리픽스 규약을 두는 정도로 유저스페이스
  관례에 맡기는 것을 제안, §"결정이 필요한 사항" 2번).
- 이름 충돌(이미 쓰이는 이름으로 `openChannel`)은 **즉시 실패** -
  "먼저 연 쪽이 이긴다"는 단순 규칙.
- 이 이름 테이블은 사실상 DS-D4E5C451이 미뤄 둔 "공개 인터페이스
  registry"의 **최소 부분집합**(이름→핸들 조회만, 인터페이스
  버전/타입 정보 등은 없음)이다 - registry가 나중에 실제로 설계되면
  이 이름 테이블을 흡수하거나 그 위에 얹는 방향을 제안한다(지금
  당장 registry 전체를 설계하지 않는다).

## Channel 수명

`Channel`(랑데부 지점)은 소유 프로세스가 살아있는 동안 유효하고,
**소유 프로세스가 종료되면 커널이 자동으로 정리**한다(그 시점에
대기 중이던 `connectChannel` 호출들은 전부 실패로 깨운다) - 그래서
명시적인 `closeChannel`/`destroyChannel` API를 필수로 두지 않았다.
다만 프로세스가 오래 사는 동안 이름 있는 채널을 명시적으로 반납하고
싶은 경우(예: 이름을 재사용하고 싶을 때)를 위해 `destroyChannel`을
선택적으로 추가할지는 §"결정이 필요한 사항" 4번.

## 취소/실패 처리와 Syscall 제안의 연동

`connectChannel`/`acceptFromChannel`/`send`/`recv`로 블로킹 중인
스레드가 죽으면(SP-04EE2A18의 "종료 시 취소 처리" 그대로) 각각의
`AsyncTaskHandler::onCancel`(같은 제안에서 신설 제안)이 불려 다음을
정리해야 한다:

- `connectChannel` 취소: 그 `Channel`의 대기열에서 자신의
  `PendingConnectRequest`를 제거.
- `acceptFromChannel` 취소: 별도 정리 없음(대기열은 그대로 - 다음
  `acceptFromChannel` 호출이 여전히 유효).
- `send`/`recv` 취소: 이미 만들어진 `BridgePipe`는 그대로 유지(다른
  스레드가 같은 핸들로 계속 쓸 수 있음 - 단일 스레드만 쓰는 경우가
  일반적이겠지만 금지할 이유는 없음).

## 결정이 필요한 사항 (DC로 등록)

1. §"이름 정리"의 `Channel`/`BridgePipe`/`closeBridge` 명명 제안
   승인 여부(원문 그대로 `closeChannel`을 유지하되 대상만 명확히
   문서화하는 대안도 가능).
2. 이름 있는 채널의 네임스페이스 범위(전역 단일 vs 프로세스/사용자별
   분리) 및 이름 충돌 정책("먼저 연 쪽이 이김" 외 대안 필요 여부).
3. `recv`가 피어의 `closeBridge` 이후 호출됐을 때 "0바이트 반환
   (EOF)"과 "실패 코드 반환" 중 어느 쪽을 쓸지, 그리고 `send`가
   완전히 채워진 링버퍼에서 블로킹하는 대신 부분 전송(가능한 만큼만
   쓰고 즉시 반환)을 허용할지.
4. `BridgePipe` 링버퍼 크기(고정 vs `openChannel`/`connectChannel`
   시 지정 가능) 및 `destroyChannel`(명시적 Channel 반납 API) 신설
   여부.

## 참고

- SP-8B6B8D25 §2(3번/10번 항목) - 이 설계가 구체화하는 커널 책임.
- DS-D4E5C451 - raw binary IPC 포맷, "공개 인터페이스 registry는
  아직 열린 설계" 결정.
- **SP-04EE2A18(Syscall 디스패치 및 비동기 처리 서브시스템)** - 이
  문서의 모든 API가 구현되는 하부 계층(짝을 이루는 제안).
