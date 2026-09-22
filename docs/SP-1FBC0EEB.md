# 메시징 채널 IPC — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-1FBC0EEB
  status: approved
  updatedAt: 2026-09-21T20:00:11.552Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
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

**[2026-09-14, DC-B538A218/QU-B9FFBD37 답변 반영 - 아래 전면 개정]**:

1. 원문/직전 초안의 API 이름(`Channel`/`BridgePipe`/`closeBridge`
   등)은 **형태 예시일 뿐 고정이 아니다** - "원문 이름을 그대로
   유지할 필요는 없고, 형태만 제안한 것이지 고정된 구조로 받아들이지
   마라"는 답변대로, 이 문서가 쓰는 이름은 전부 계속 바뀔 수 있는
   제안으로 취급한다.
2. 이름 있는 오브젝트는 **`/sys/live/named/` 아래의 가상 파일**로
   통합된다 - §"이름 있는 오브젝트 = `/sys/live/named/` 가상 파일"
   전면 신설.
3. `recv`/`send` → **`read`/`write`로 통일**, 닫힌 파이프 RW는
   무조건 `-1`+에러코드("broken pipe"), **기본은 비동기, 블로킹
   옵션 설정 시에만 블로킹**(호출자 관점) - §"데이터 전송" 개정.
4. 링버퍼 크기는 **커널 설정값**(페이지 단위)이 상한이고 호출부가
   직접 지정 못함 - huge page 사용 여부만 플래그로 제출, 미지원 시
   실패 코드 - §"링버퍼 크기 정책" 신설.
5. **`destroyChannel` 포함 확정**(필수 API로 승격).

**[커널 내부 구현 완료, 2026-09-14, 계획 PN-A5C06B9B]** 아래 설계
전부가 PL-C8648D4D로 구현·검증됐다(ring3 트랩 진입 제외 - 계획
PN-124C105B).

관련 기존 결정:

- **SP-8B6B8D25 §2/§4** - 커널의 책임 "3. IPC를 해석하여 메시징
  채널로 커널 서비스들에게 전파"/"10. 공유 메모리 및 프로세스간
  메시징" - 이 채널 IPC가 그 실체다. §4의 파일시스템 구조에서
  `/sys/live/`는 "이 시스템을 지탱하는 커널과 커널 서비스들이
  담당"하는 트리로 이미 정의돼 있다 - `/sys/live/named/`는 그 아래
  새로 추가되는 하위 트리다.
- **DS-D4E5C451** - IPC 메시지 포맷은 **raw binary**로 이미 확정 -
  `read`/`write`가 주고받는 데이터는 별도 직렬화 없이 그대로의
  바이트열. "프로세스간 공개 인터페이스 registry"는 작성 당시
  아직 열린 설계 영역이었으나 **[갱신, 2026-09-16]** 이후 5번째
  커널 서비스 `pubreg`(유저랜드, MCP 스타일 tool 선언, SP-B071E628)
  로 완전히 확정됐다(계획 PN-268F062B, 완료 처리됨 - 실제 구현은
  PN-185406F6) - `/sys/live/named/`는 그와 별개로, 이 문서(Channel
  IPC)가 다루는 채널/향후 큐/공유메모리 등 커널 자체 오브젝트의
  네이밍 계층이다(§"이름 있는 오브젝트" 참고, pubreg의 tool 레지스트리와는
  다른 네임스페이스).
- **SP-04EE2A18(Syscall 디스패치 및 비동기 처리 서브시스템, 짝을
  이루는 제안)** - 아래 모든 API는 전부 그 제안의 syscall endpoint
  하나씩으로 구현된다 - "제출(submit)은 즉시 반환, 실제 대기는
  `waitForSyscall`" 모델을 `read`/`write`의 기본 비동기 동작이 그대로
  물려받는다(§"데이터 전송" 참고).
- **PL-57CF86EF(4K→2M 페이지 병합/분할)** - huge page 옵션과 관련은
  있지만 **같은 메커니즘은 아니다**(§"링버퍼 크기 정책"의 구분 참고,
  Channel 쪽 huge page 지원은 계획 PN-34B34DB4).

## 이름 있는 오브젝트 = `/sys/live/named/` 가상 파일

**설계자 지시(2026-09-14)**: "어떤 객체냐에 관계없이 `이름`들을
담는 공간은 결국 `/sys/live/named/` 하에 비치되는 `가상 파일`들이다.
이름이 충돌나면 개설을 실패시켜야 하고, `개설`을 시도할 때 그
이름이 `메시징 채널`인지 `큐`인지 `공유 메모리`인지는 파악할 수
없어야 한다(보안정책)."

이 지시는 채널 하나만의 이름 공간이 아니라 **커널이 제공하는 모든
"이름 붙은 IPC성 오브젝트"(메시징 채널, 향후 큐, 향후 공유 메모리
등)가 공유하는 단일 네임스페이스**를 요구한다:

- 이름 하나(`/sys/live/named/<name>`)당 정확히 하나의 오브젝트만
  존재할 수 있다 - **오브젝트 종류와 무관하게 같은 충돌 검사를
  거친다**. 이미 쓰이는 이름으로 무엇을 개설하려 하든 즉시 실패한다.
- **보안 정책 - 종류 은닉**: 이름 충돌(또는 그 이름을 대상으로 한
  뒤이은 조작)이 실패할 때, 그 실패가 "이미 있는 오브젝트가 어떤
  종류인지"를 유출해서는 안 된다 - 예를 들어 채널로 열려던 이름이
  이미 공유 메모리로 쓰이고 있어도, 그냥 "이름 사용 불가"라는
  **하나의 일반 실패 코드**만 돌려준다("이 이름은 공유 메모리라
  채널로 못 엽니다" 같은 종류별 상세 사유를 노출하지 않음) - 이름
  공간을 훑어 어떤 이름에 어떤 종류의 오브젝트가 있는지 추측하는
  공격을 막기 위함이다.
- **범위 확정 방법(제안)**: 실제 VFS/fs 서비스(작성 당시 유저랜드
  전제였음 - `fs` 자체는 2026-09-21부로 커널 `KernelThread`,
  SP-8B6B8D25 §2-A 정정 각주 참고, 단 이 절이 가리키는 "실제 파일
  드라이버 로직은 아직 없다"는 핵심 논지는 그대로 유효)는 아직
  없으므로, 지금 단계에서는 커널이 **자체 내부 이름
  테이블**을 두고 그 경로 표현(`/sys/live/named/<name>`)만 미리
  맞춰 둔다 - procfs(SP-8B6B8D25 §2 13번)와 같은 성격의, fs 서비스에
  의존하지 않는 커널 자체 가상 트리다. 실제 범용 VFS 계층이 생기면
  그 마운트 지점 중 하나로 이 이름 테이블을 노출하는 방향을 제안한다
  (지금 당장 완전한 VFS 의미론을 구현하지 않는다).

이 이름 테이블은 `openChannel`(아래) 하나만의 것이 아니라, 앞으로
추가될 다른 이름 붙은 오브젝트(큐, 공유 메모리 등)도 **같은
`open<Kind>(name)`류 API가 같은 충돌 검사 경로를 공유**하도록
설계한다 - 이번 제안에서는 채널의 경우만 구체화한다.

## 핵심 객체 (이름은 예시 - §"배경" 1번 참고)

```cpp
namespace kernel {

using ChannelId = uint64_t;      // 커널 전역에서 유일
using BridgeHandle = uint64_t;   // 프로세스 로컬 핸들(파일서술자류)

// 랑데부 지점 - 서버 프로세스가 openChannel로 만든다. 실제 데이터는
// 안 오가고, "연결 요청 대기열"만 갖는다.
struct Channel {
    ChannelId id;
    ProcessId owner;                 // 이 채널을 만든 프로세스(=accept할 자격)
    // 이름 없이 개설했으면 비어 있음 - 있으면 /sys/live/named/ 이름
    // 테이블의 키(위 절 참고).
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
    // 기본 false(비동기) - 설정되면 이 반쪽의 read/write가 완료까지
    // 호출자를 블로킹한다(§"데이터 전송" 참고).
    bool blocking = false;
    /* peer BridgePipe*로의 참조, 송신 링버퍼, 수신 링버퍼 */
};

}  // namespace kernel
```

## API 시퀀스와 각 syscall의 처리

모든 함수는 **SP-04EE2A18**의 `SyscallEndpointId` 하나씩이다.

### 1. `openChannel(name: optional<string>) -> (ChannelId, handle)`

- `name`이 없으면: 새 `Channel`을 Slab/전용 풀에서 만들고 새
  `ChannelId`를 발급한다 - **블로킹 없음**(즉시 완료).
- `name`이 있으면: `/sys/live/named/` 이름 테이블에서 그 이름이
  이미 쓰이고 있는지 확인 - **있으면 즉시 실패**(종류 불문, 위
  "보안 정책 - 종류 은닉" 참고), 없으면 등록하고 위와 동일하게 진행.
- 반환된 `handle`은 이 프로세스가 나중에 `acceptFromChannel`을 부를
  때 쓰는 로컬 참조(파일서술자류) - `ChannelId`는 **다른 프로세스에게
  전달할 값**이다(전달 방법 자체는 이번 제안 범위 밖, §"Channel ID
  전달" 참고).

### 2. `connectChannel(target: ChannelId | name) -> BridgePipe | 실패`

- 대상 `Channel`을 `ChannelId`(또는 이름 조회 후 얻은 ID)로 찾는다 -
  **없으면 즉시 실패**(블로킹 없음). 이름으로 찾았는데 그 이름의
  오브젝트가 채널이 아니면(다른 종류) - 역시 종류를 밝히지 않는
  일반 실패.
- 있으면 `PendingConnectRequest`를 그 `Channel`의 대기열에 넣고
  **블로킹**(서버가 accept하거나, 명시적으로 거부하거나, 채널이
  없어질 때까지) - 정확히는 SP-04EE2A18의 submit/wait 모델을 따른다
  (아래 "데이터 전송"과 동일한 기본 동작).
- 서버가 accept하면: 새 `BridgePipe` 쌍 중 클라이언트 쪽 반쪽을
  들고 깨어난다.
- 실패 사유(예): 채널을 찾을 수 없음, 채널 소유 프로세스가 이미
  종료됨(대기 중 또는 시도 시점), `BridgePipe` 쌍 할당 실패(자원
  고갈).

### 3. `acceptFromChannel(handle) -> BridgePipe | 실패`

- 이 프로세스가 소유한 `Channel`의 대기열이 비어 있으면 **블로킹**,
  하나 있으면 즉시 꺼낸다.
- 꺼낸 요청에 대해 `BridgePipe` 쌍을 할당(Slab, 각 방향 링버퍼 포함
  - 크기 정책은 §"링버퍼 크기 정책" 참고)하고, 서버 쪽 반쪽은 이
  호출의 반환값으로, 클라이언트 쪽 반쪽은 대기 중이던 `connectChannel`
  호출을 깨워 전달한다.
- **수락 실패 사유**(설계자가 명시적으로 고려하라고 지시): `handle`이
  유효하지 않음(즉시 실패), 이 프로세스 소유 채널이 아님(즉시 실패),
  `Channel` 자체가 그 사이 없어짐(예: 소유 프로세스가 같은 채널에
  대해 동시에 종료 처리 중 - 경쟁 상태 방지 필요), `BridgePipe` 쌍
  할당 실패(자원 고갈 - 이 경우 대기 중이던 `connectChannel`도 실패로
  깨워야 한다).

### 4. 데이터 전송 - `write(bridge, data)` / `read(bridge, buffer, maxLen)`

**설계자 지시(2026-09-14)로 `send`/`recv`를 `write`/`read`로
통일한다** - 나머지 syscall과 이름 결도 맞고, 파일류 오브젝트로
다루겠다는 `/sys/live/named/` 방향과도 일관된다.

- **기본 동작 = 비동기(호출자 관점)**: 이 두 syscall도 SP-04EE2A18의
  일반 submit/wait 모델을 그대로 따른다 - 호출하면 즉시 추적 토큰을
  반환하고, 실제 완료 여부/결과는 `waitForSyscall(token)`으로
  따로 확인한다. 이게 "기본적으로 비동기"의 의미다 - 커널 차원에서
  다른 syscall과 다르게 취급하지 않는다.
- **`BridgePipe.blocking` 옵션**(설계자 지시 - "블로킹 옵션이
  설정되면 블로킹 동작을 한다(caller 관점에서)"): 이 반쪽에
  블로킹이 켜져 있으면, `write`/`read`의 유저랜드 스텁이 syscall
  제출 직후 자동으로 `waitForSyscall`까지 호출해 호출자에게는 평범한
  블로킹 I/O처럼 보이게 만든다 - 커널 쪽 메커니즘은 바뀌지 않고
  유저랜드 스텁의 동작만 달라진다. 옵션은 `connectChannel`/
  `acceptFromChannel` 시점에 반쪽별로 설정한다고 제안한다(정확한
  변경 API는 구현 시 확정).
- **닫힌 파이프 처리(확정)**: 상대가 이미 닫았거나 이 반쪽이 이미
  닫힌 상태에서 `read`/`write`를 시도하면, 데이터 유무와 무관하게
  **항상 `-1`과 에러코드로 "broken pipe"를 나타낸다** - 0바이트
  반환으로 EOF를 표현하는 별도 경로는 두지 않는다(POSIX 파이프의
  "0=EOF" 관례를 따르지 않기로 명시적으로 확정됨).
- 송신 링버퍼가 꽉 찼거나 수신 링버퍼가 비어 있는 "아직 처리할 수
  없음" 상태는 실패가 아니라 **미완료**로 취급한다 - submit/wait
  모델 그대로, `waitForSyscall`이 채워지거나 비워질 때까지 대기(또는
  즉시 완료)를 처리한다.

### 5. `closeBridge(bridge)`

- 이 반쪽을 무효화하고, 상대 반쪽에 "피어가 닫혔음"을 통지한다(상대의
  대기 중인 `read`/`write`를 깨워 위 "닫힌 파이프 처리"대로 실패
  하게 만든다). 양쪽이 다 닫히면 `BridgePipe` 쌍 자체(링버퍼 포함)를
  회수한다.

### 6. `destroyChannel(handle)` (신설, 필수)

**설계자 지시로 필수 API에 포함한다.** `Channel`(랑데부 지점)을
명시적으로 반납한다 - 소유 프로세스만 호출 가능. 대기 중이던
`connectChannel` 호출들은 전부 실패로 깨운다. 이름 있는 채널이면
`/sys/live/named/`에서 그 이름도 함께 제거해 재사용 가능하게 만든다.
(프로세스가 종료되면 소유한 모든 `Channel`이 자동으로 이렇게 정리
된다는 점은 그대로 유지 - 이 API는 프로세스가 살아있는 동안의
명시적 조기 반납 경로다.)

## 링버퍼 크기 정책

**설계자 지시(2026-09-14)**: "브릿지 파이프의 링 버퍼는 커널이
설정값을 보관하고, 그 값으로 제한한다(최소 메모리 페이지 단위, 예:
4K). 열고 닫을 때 크기를 직접 지정할 수 없고 huge page(예: 2M)을
사용할지 아닐지만 플래그로 제출한다. 지원하지 않는 경우(커널이
설정에 의해 금지한 경우)엔 실패 코드를 반환하라."

- `connectChannel`/`acceptFromChannel`은 정확한 바이트 크기를 받지
  않는다 - **`useHugePage: bool` 플래그 하나만** 받는다.
- 실제 크기(일반 페이지 몇 개, 또는 huge page 몇 개)는 **커널이
  들고 있는 설정값**(부팅 시 고정 또는 커널 설정 파일류에서 읽음 -
  정확한 소스는 구현 시 확정)이 상한이다.
- `useHugePage=true`인데 커널 설정이 huge page를 금지했으면(예:
  해당 하드웨어/구성에서 비활성화) **그 자리에서 실패 코드를 반환**
  한다 - 조용히 일반 페이지로 폴백하지 않는다. **[구현 완료,
  2026-09-16, PN-34B34DB4]** huge page 실제 매핑 지원이 완료돼
  더 이상 "v1은 항상 실패"가 아니다 - `PageFrameAllocator::
  allocOrder(9)` + `kPhysToVirt()`로 2MiB 블록을 확보/사용한다(위
  "PL-57CF86EF와는 다른 메커니즘" 절 참고). 할당 자체가 실패하는
  경우(메모리 고갈 등)만 실패 코드(`ResourceExhausted`)를 반환한다.
- **PL-57CF86EF와는 다른 메커니즘이다**: PL-57CF86EF는 *이미 4KiB
  단위로 흩어져 매핑된* 페이지들을 나중에 2MiB로 병합/재분할하는
  기능이고, 여기서 필요한 건 *처음부터* 물리적으로 연속인 2MiB
  블록을 확보해 페이지테이블에 2M PDE 하나로 매핑하는 것이다.
  `PageFrameAllocator::allocOrder(9)`(2^9×4KiB=2MiB, 버디 할당자가
  이미 지원하는 order)로 물리 블록 확보 자체는 가능하다.
  [갱신, 2026-09-15] PN-D28DD9F3(PL-57CF86EF) 구현 완료로
  `Paging::mapRange`가 2M 정렬+물리 연속+빈 슬롯 조건을 만족하면
  실제로 2M PS 하나로 매핑하는 경로를 이미 제공한다 - "이 기능이
  `Paging`에 아직 없다"는 더 이상 사실이 아니다. **[구현 완료,
  2026-09-16, PN-34B34DB4, commit 2e183a7]** Channel 링버퍼 huge
  page 지원 자체도 이제 실제로 연결됐다 - `useHugePage=true`는 더
  이상 즉시 실패하지 않는다. 실제로는 `Paging::mapRange`조차 필요
  없었다(이 링버퍼가 유저 주소공간에 매핑되지 않는 순수 커널 내부
  메모리라, direct map 덕분에 `PageFrameAllocator::allocOrder(9)`가
  돌려준 물리주소를 `kPhysToVirt()`로 바로 커널 가상주소로 쓸 수
  있었다 - PN-34B34DB4 상세 참고). 아래 "링버퍼 크기 정책" 절의
  "v1은 huge page 요청을 항상 실패시킨다"는 서술은 이제 과거형(v1
  범위였던 것)으로 읽는다.

## Channel ID 전달 (계획 PN-6D497EB0, [완료, 2026-09-17])

"채널 ID를 공유할 대상 프로세스에게 어떻게든 전달"하는 세 후보
경로 중 이름 기반 발견(`/sys/live/named/`)은 이미 구현됐고
(PL-C8648D4D), "이미 연결된 다른 `BridgePipe`로 전달"은 그 Bridge의
기존 `write()`로 ChannelId 값을 페이로드 바이트로 보내면 되는
기존 syscall만으로 이미 가능한 경로라 별도 설계가 필요 없다(자세한
경위는 PN-6D497EB0 참고). 남은 하나 - **프로세스 생성 시 인자로
전달**은 `PN-E35294B8`(SysV argv/envp 실제 전달)과
`PN-CE6A04AB`(ChannelId를 커널이 발급하는 안전한 불투명 핸들로
교체 - channel.h 참고)이 둘 다 완료되면서 이제 착수 가능해졌고,
다음과 같은 순수 관례로 확정한다(새 syscall/자료구조 불필요):

- 부모가 자식에게 채널을 공유하려면, `SpawnProcess`의 `envp`(위치
  인자 `argv`가 아니라 **환경변수**)에 `CHANNEL_ID=<10진수 ChannelId
  값>` 형태의 항목 하나를 추가해 호출한다 - POSIX 관례상 이런
  "프로그램이 명시적으로 요청하지 않은 구현 세부 정보 전달"은
  위치 인자보다 환경변수 쪽이 자연스럽다(사용자가 보는 커맨드라인
  인자를 오염시키지 않음).
- 자식은 시작 시 자신의 envp를 순회해 `CHANNEL_ID=` 접두어를 찾아
  그 뒤의 10진수 문자열을 `ChannelId`(`uint64_t`)로 파싱한 뒤,
  그 값을 그대로 `ConnectChannel(target=값)`에 넘겨 연결을 맺는다 -
  값 자체는 이제(PN-CE6A04AB) 커널이 발급한 불투명 핸들이라 자식이
  그 값을 신뢰하고 그대로 쓰는 데 아무 문제가 없다(위조해 봐야
  `kResolveChannelId()`가 걸러낸다).
- 10진수를 택한 이유: 16진수 `0x` 접두어의 유무 등 파싱 모호성을
  피하기 위한 단순한 구현 선택 - 재검토 가치가 낮아 별도 확인
  없이 결정.

실제 유저랜드 소비자(이 관례를 실제로 쓰는 프로그램)는 아직 없다 -
이 절은 관례 자체만 확정하고, 실제 구현은 그런 소비자가 생길 때
자연히 뒤따른다.

## 취소/실패 처리와 Syscall 제안의 연동

`connectChannel`/`acceptFromChannel`/`read`/`write`로 대기 중인
스레드가 죽으면(SP-04EE2A18의 "종료 시 취소 처리", `AsyncTaskHandler
::onCancel` 반영 완료, 계획 PN-40E976F2) 각각 다음을 정리해야 한다:

- `connectChannel` 취소: 그 `Channel`의 대기열에서 자신의
  `PendingConnectRequest`를 제거.
- `acceptFromChannel` 취소: 별도 정리 없음(대기열은 그대로 - 다음
  `acceptFromChannel` 호출이 여전히 유효).
- `read`/`write` 취소: 이미 만들어진 `BridgePipe`는 그대로 유지(다른
  스레드가 같은 핸들로 계속 쓸 수 있음).

> **[정정, 2026-09-17, PN-C4611402]** 위 "acceptFromChannel 취소:
> 별도 정리 없음" 전제는 실제 자료구조와 맞지 않았다 - 대기 큐
> (`channel->pendingAccepters`)에 매달아 두는 건 다른 무언가가 아니라
> **취소되면 곧 반납될 이 AsyncTask 자기 자신**이다(코드 확인,
> channel.cpp). 정리하지 않으면 반납된 AsyncTask가 그 큐에 댕글링
> 포인터로 남아, 다음 `connectChannel`이 그걸 꺼내 깨우려 하면
> use-after-free다 - 실제로는 `connectChannel`과 동일하게 **자기
> 자신을 대기열에서 제거하는 정리가 필요하다**(구현 완료,
> `AcceptFromChannelHandler::onCancel`). 위 "`read`/`write` 취소:
> `BridgePipe`는 그대로 유지" 자체는 맞지만, 이 문서가 언급하지 않은
> 별개의 정리가 하나 더 필요했다 - 대기 중이던 이 task를 그
> `RingBuffer::pendingReaders`/`pendingWriters`에서도 제거해야 한다
> (`BridgePipe` 객체의 수명과는 무관한, `pendingAccepters`와 같은
> 모양의 댕글링 포인터 문제 - 구현 완료, `ChannelReadHandler`/
> `ChannelWriteHandler::onCancel`). `connectChannel` 절 자체는 원문
> 그대로 정확했다.

## 참고

- SP-8B6B8D25 §2(3번/10번 항목)/§4(`/sys/live/` 구조) - 이 설계가
  구체화하는 커널 책임과 그 위치.
- DS-D4E5C451 - raw binary IPC 포맷, "공개 인터페이스 registry는
  아직 열린 설계" 결정.
- PL-57CF86EF - huge page 매핑과 구분되는 기존 4K/2M 병합 설계.
- **SP-04EE2A18(Syscall 디스패치 및 비동기 처리 서브시스템)** - 이
  문서의 모든 API가 구현되는 하부 계층, submit/wait 모델의 출처.
- DC-B538A218 - 이 개정이 반영한 답변의 원본 질의.

