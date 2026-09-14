# USB 스택(호스트 컨트롤러 + 장치 열거) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-E35FD36C
  status: review
  updatedAt: 2026-09-14T15:56:43.217Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# USB 스택(호스트 컨트롤러 + 장치 열거) — 설계 제안

설계자 지시(2026-09-14, 메시지) - "AHCI와 PnP 그리고 USB도 설계
제안서 작성해줘." PnP 제안(SP-9DD4F3EA, 장치 자동 인식/핫플러그 프레임워크)이
PCI 레벨(호스트 컨트롤러 자체)의 발견을 다루고, 이 문서는 그 아래
**USB 버스 자체의 장치 열거**(호스트 컨트롤러 하나에 여러 USB
장치가 매달리는 계층)를 다룬다 - PnP 프레임워크(SP-9DD4F3EA)의 "핫플러그 알림 →
드라이버 매칭" 흐름을 호스트 컨트롤러 안에서 한 번 더 반복하는
구조다.

## 1. 배경 및 범위

- **호스트 컨트롤러 세대**: UHCI/OHCI(USB 1.1), EHCI(USB 2.0),
  xHCI(USB 3.x, 하위 호환으로 1.1/2.0 장치도 처리) 넷 중, **xHCI만
  지원**하는 것을 v1 범위로 제안한다 - 2010년대 이후 사실상 모든
  x86-64 플랫폼이 xHCI를 표준으로 채택했고, 네 세대를 전부 구현하는
  것은 이 프로젝트 초기 단계의 투자 대비 가치가 낮다고 판단했다
  (레거시 UHCI/OHCI/EHCI 지원 설계는 §6에서 별도로 다룬다).
- **PCI 클래스**: `0x0C`(Serial Bus) / `0x03`(USB) / prog-if
  `0x30`(xHCI).
- **범위 밖(v1)**: USB 클래스 드라이버(HID/Mass Storage/오디오 등)는
  이 문서가 다루지 않는다 - 이 문서는 "호스트 컨트롤러 구동 +
  장치 열거 + 클래스 드라이버에게 넘겨주는 지점"까지만 다룬다.

## 2. xHCI 하드웨어 모델 요약

- **레지스터 세트 3종**(전부 BAR0/1 MMIO 안): Capability
  Registers(읽기 전용, 컨트롤러 능력 조회) / Operational
  Registers(USBCMD/USBSTS/CRCR/DCBAAP 등) / Runtime Registers
  (인터럽터별 이벤트 링 관리) + Doorbell Registers(커맨드/전송 큐에
  새 작업이 생겼음을 컨트롤러에 알림).
- **링 기반 통신**: Command Ring(호스트→컨트롤러 명령, 예: 장치
  슬롯 활성화), Event Ring(컨트롤러→호스트 완료/이벤트 통지),
  Transfer Ring(엔드포인트별 실제 데이터 전송 요청) - 셋 다 원형
  버퍼 + TRB(Transfer Request Block) 구조.
- **장치 슬롯(Device Slot)**: 포트에 장치가 꽂히면 컨트롤러가 슬롯을
  배정 - 슬롯마다 Device Context(엔드포인트별 상태)를 가리키는
  DCBAA(Device Context Base Address Array) 엔트리가 있다.

## 3. 설계 제안

### 3.1 계층 구조 (devmgr 내부)

```
[USB 클래스 드라이버 - HID/MassStorage/...]  (v1 범위 밖, 인터페이스만 정의)
        ^
        |  UsbDevice(디스크립터, 엔드포인트 목록)
[UsbCore]         - 장치 열거, 디스크립터 파싱, 클래스 드라이버 매칭
        |
[XhciController]  - Command/Event/Transfer Ring 관리, 슬롯 배정
        |
[xHCI MMIO 레지스터]  - PnP가 매핑해 준 가상주소로 직접 접근
```

- **`XhciController::init(mmioVirtAddr)`**: 컨트롤러 리셋(USBCMD.HCRST),
  Capability 레지스터로 포트 수/슬롯 수/인터럽터 수 확인, DCBAAP/
  CRCR 설정(§3.2의 DMA 버퍼 확보로 물리 주소 필요), Event Ring 설정,
  USBCMD.RUN/STOP 비트로 가동.
- **`UsbCore::pollPorts()`**: 포트 상태 레지스터(PORTSC)의 Connect
  Status Change 비트로 새 장치 감지 → 슬롯 활성화(Enable Slot
  커맨드) → 기본 제어 엔드포인트로 Device Descriptor 읽기(GET_DESCRIPTOR
  제어 전송) → Configuration/Interface/Endpoint Descriptor 순서로
  전부 읽어 `UsbDevice` 구조체 완성 → vendor/product ID 또는
  클래스 코드로 클래스 드라이버 매칭(PnP 제안(SP-9DD4F3EA) §3.2와 같은 매칭
  테이블 재사용 제안).
- **핫플러그**: PORTSC 변경은 인터럽트(Port Status Change Event,
  Event Ring에 들어옴)로도 통지되므로, 폴링 대신 **Event Ring
  인터럽트 기반**으로 구현하는 것을 제안한다(폴링은 디버깅 초기
  단계의 폴백으로만).

### 3.2 DMA 메모리 관리

xHCI도 AHCI와 완전히 같은 문제를 가진다 - Command/Event/Transfer
Ring, Device Context 전부 컨트롤러가 DMA로 직접 접근하는 물리
연속 메모리가 필요하다. **[해결, 2026-09-14] DMA 버퍼 관리자
(SP-39F18E30)가 이 문제를 커널 syscall(`AllocDmaBuffer`/
`FreeDmaBuffer`)로 구체화했다** - AHCI와 완전히 동일한 메커니즘을
공유하며, xHCI의 모든 DMA 구조체는 64비트 물리 주소 필드를 쓰므로
`physAddrLimit=0`(제한 없음)으로 호출하면 된다(32비트 제약이 필요한
쪽은 §6의 레거시 컨트롤러).

### 3.3 제어 전송(Control Transfer)의 동기적 성격

디스크립터를 읽는 제어 전송은 "요청 → 완료까지 대기"가 자연스러운
동기적 흐름이다 - `Syscall::wait`(SP-04EE2A18) 패턴을 그대로 재사용해
"USB 제어 전송 하나 = syscall 하나"로 모델링할 것을 제안한다
(AsyncTask 프레임워크 위에서 자연스럽게 표현됨 - 이미 구현된 인프라
재사용).

## 4. 아직 열려 있는 설계 영역

1. ~~**DMA 버퍼 확보 메커니즘**~~ - **해결(SP-39F18E30, §3.2 참고)**.
2. **클래스 드라이버 로딩 방식** - PnP 제안과 동일하게 v1은 정적
   링크를 제안(HID/Mass Storage 등도 devmgr 실행 파일에 정적
   포함) - 동적 로딩은 후속.
3. ~~**레거시(UHCI/OHCI/EHCI) 지원 여부와 시점**~~ - **설계 초안
   작성됨(§6 참고)**, 실제 착수 시점(구형 하드웨어 요구 발생 여부)은
   여전히 설계자 판단 필요.
4. ~~**USB 3.x SuperSpeed 전용 기능(예: 스트림, 링크 파워 관리)**~~ -
   **설계 초안 작성됨(§7 참고)**, v1 구현 범위 포함 여부는 설계자
   판단 필요.

## 5. 선행 조건

- PnP 프레임워크(SP-9DD4F3EA)의 IO 권한 부여 syscall - xHCI
  컨트롤러 자체를 PCI 장치로 발견하는 데 필요.
- DMA 버퍼 관리자(SP-39F18E30, §3.2 참고 - AHCI 제안과 공유).
- 프로세스 모델(PN-16CA347D), Syscall 서브시스템(SP-04EE2A18, 이미
  구현 완료 - 제어 전송 동기화에 재사용).

## 6. 레거시 호스트 컨트롤러(UHCI/OHCI/EHCI) 지원 설계 (초안, 2026-09-14)

설계자 지시로 v1 이후 지원 대상인 레거시 3세대의 구조를 미리 설계해
둔다 - **착수 시점은 여전히 미정**("실제 구형 하드웨어 요구가 생기면"
이라는 조건부, §4-3 참고), 아래는 그 시점이 왔을 때 바로 쓸 수 있게
미리 그려 둔 초안이다.

### 6.1 세 컨트롤러의 근본적 차이

- **UHCI(Intel, USB 1.1)**: **포트 I/O 기반**(MMIO가 아니다!) - PCI
  BAR가 I/O 공간을 가리킨다. Frame List(1024개 32비트 포인터 배열,
  물리 주소 **32비트 전용**) + Queue Head/Transfer Descriptor 체인.
  현재 PnP의 IO 권한 부여 syscall(SP-9DD4F3EA §3.3)은 **MMIO BAR
  매핑만** 다루므로, UHCI를 실제로 지원하려면 그 syscall을 "포트
  I/O 권한 부여"도 함께 내주도록 확장해야 한다(PnP §3.3에 대한 신규
  DC 필요 - 이 문서가 그 확장 필요성을 처음 명시).
- **OHCI(비-Intel 진영, USB 1.1)**: UHCI와 경쟁하던 대안 표준 -
  **MMIO 기반**이라 PnP의 기존 IO 권한 부여 syscall을 그대로 쓸 수
  있다. HCCA(Host Controller Communication Area, 256바이트 정렬 물리
  연속 메모리 - Interrupt Table + 현재 프레임 번호 + Done Queue Head)
  하나와 Endpoint Descriptor/Transfer Descriptor 체인으로 구성 -
  물리 주소는 **32비트 전용**.
- **EHCI(USB 2.0)**: **컴패니언 컨트롤러 모델**을 쓴다 - EHCI 자신은
  고속(High-Speed, 480Mbps) 장치만 직접 다루고, 저속/풀속 장치가
  꽂힌 포트는 **컴패니언 컨트롤러(같은 PCI 슬롯 아래 있는 UHCI 또는
  OHCI 기능)에게 소유권을 넘긴다** - EHCI의 포트별 `PORTSC` 레지스터
  중 **Port Owner** 비트를 세팅하면 그 포트의 신호선이 물리적으로
  컴패니언 컨트롤러로 전환된다. PCI 열거 시 "같은 버스/슬롯, 낮은
  function 번호 = 컴패니언(UHCI/OHCI), 가장 높은 function 번호 =
  EHCI 자신"이라는 관례(PCI 사양 규약)로 컴패니언을 찾는다. Frame
  List(EHCI는 4096 엔트리까지 가능) + Queue Head/qTD 체인 - **물리
  주소는 64비트 확장 가능**(각 포인터 다음에 상위 32비트를 위한
  선택적 필드가 있음, "64비트 애드레싱 능력" CAPBASE 비트로 감지) -
  단 컴패니언 UHCI/OHCI 쪽 구조체는 여전히 32비트 제약을 받는다.

### 6.2 공통 계층 구조 제안

§3.1의 `XhciController`와 나란히 같은 인터페이스를 구현하는 형태를
제안한다 - `UsbCore`가 컨트롤러 종류를 몰라도 되게:

```cpp
// devmgr 내부, 호스트 컨트롤러 세대와 무관한 공통 인터페이스(가칭)
class HostControllerDriver {
public:
    virtual bool init(uint64_t mmioOrIoBase, bool isPortIo) = 0;
    virtual void pollPorts() = 0;   // 또는 인터럽트 콜백
    virtual UsbTransferHandle submitControlTransfer(...) = 0;
    // ...
};

class XhciController : public HostControllerDriver { /* §3 그대로 */ };
class EhciController : public HostControllerDriver { /* §6.1 */ };
class UhciController : public HostControllerDriver { /* §6.1, 포트 I/O */ };
class OhciController : public HostControllerDriver { /* §6.1 */ };
```

`DeviceManager`(PnP 제안 §3.2)의 매칭 테이블에 UHCI/OHCI/EHCI의
(classCode=0x0C, subclass=0x03, progIf) 조합을 추가로 등록하면 되고,
`UsbCore`는 `HostControllerDriver*` 하나만 알면 되므로 xHCI 전용으로
짠 열거/디스크립터 파싱 로직(§3.1의 `pollPorts`/`GET_DESCRIPTOR` 흐름)
자체는 그대로 재사용된다 - 차이는 전송 제출 방식(TRB vs TD/qTD)뿐이다.

### 6.3 EHCI 컴패니언 라우팅과 devmgr의 초기화 순서

EHCI를 지원하려면 `DeviceManager`가 "EHCI를 먼저 초기화하고, 그 다음
같은 그룹의 컴패니언 UHCI/OHCI를 초기화"하는 순서 의존성을 인식해야
한다(현재 PnP 제안 §3.2의 매칭은 장치를 독립적으로 하나씩 처리하는
전제라 이 의존성이 없음 - 신규로 필요) - PCI 버스/슬롯이 같고
`EHCI(가장 높은 function)`가 있으면 그 그룹의 나머지 USB 컨트롤러
function들을 "컴패니언"으로 표시해 초기화 순서를 뒤로 미루는 정책을
제안한다. 이 부분은 실제 착수 시점에 PnP 제안에 대한 작은 확장(DC)
으로 다시 정리하는 것을 제안한다.

### 6.4 DMA 버퍼 제약

§6.1에서 정리했듯 UHCI/OHCI, 그리고 EHCI의 컴패니언 구조체는 모두
32비트 물리 주소 전용이다 - DMA 버퍼 관리자(SP-39F18E30)의
`AllocDmaBufferArgs::physAddrLimit = 32`를 이 세 컨트롤러 전용으로
반드시 사용해야 한다(§5-A/§5-B 참고, 이 문서가 그 필드의 실제 소비자
사례).

## 7. USB 3.x SuperSpeed 전용 기능 설계 초안 (2026-09-14)

xHCI가 USB 3.x(SuperSpeed 이상)를 다룰 때만 의미가 있는 고급 기능들 -
v1 범위(§1)는 기본 열거/제어 전송까지만이므로 전부 **v1 범위 밖으로
유지**하되, 후속 작업이 바로 이어받을 수 있도록 설계만 미리 그린다.

### 7.1 스트림 (Bulk Streams)

USB 3.x는 Bulk 엔드포인트 하나에 여러 개의 독립적인 데이터 스트림을
다중화할 수 있다(대표 소비자: UAS - USB Attached SCSI, 여러 SCSI
커맨드를 동시에 진행). xHCI 하드웨어 지원: 엔드포인트 컨텍스트의
`Max Primary Streams` 필드가 0이 아니면 그 엔드포인트는 단일
Transfer Ring 대신 **Primary Stream Context Array**(Stream ID별로
각각의 Transfer Ring을 가리킴, 필요시 Secondary Stream Context Array
로 한 단계 더 확장 가능)를 쓴다.

- **v1 설계 제안**: `UsbCore`/`XhciController`의 엔드포인트 추상화는
  처음부터 "스트림 ID 0(스트림 미사용) 고정"으로 시작해도 API 형태가
  바뀌지 않도록 자리를 잡아 둔다 - 예: `submitBulkTransfer(endpoint,
  streamId=0, ...)`처럼 `streamId` 파라미터를 v1부터 받아 두되 항상
  0만 허용(0이 아니면 에러)하는 스텁으로 시작. 실제 다중 스트림 지원
  (Stream Context Array 할당 - DMA 버퍼 관리자 재사용, Set TR
  Dequeue Pointer 커맨드로 스트림별 링 전환)은 클래스 드라이버
  (UAS 등)가 실제로 필요해지는 시점의 후속 과제.

### 7.2 링크 전원 관리 (Link Power Management, LPM)

USB 3.x 링크는 U0(활성)/U1/U2(저전력 대기)/U3(서스펜드) 상태를 오가며,
호스트 포트별 `PORTPMSC`(Port Power Management Status and Control)
레지스터로 U1/U2 진입 타임아웃, Force Link PM Accept(FLA) 등을
설정한다 - 링크가 U1/U2에서 깨어나는 지연(Exit Latency)이 전송
스케줄링에 영향을 준다(대역폭 예약 계산에 이 지연을 반영해야 함).

- **v1 설계 제안**: **LPM을 비활성 상태로 유지**(U1/U2 타임아웃을
  0 또는 비활성값으로 둬 링크가 항상 U0을 유지하게 강제)한다 - 절전
  상태 전이 중 발생하는 타이밍 이슈(전송 스케줄링과 Exit Latency의
  상호작용)까지 다루는 것은 초기 구현의 안정성 우선순위와 맞지 않다.
  전력 관리 자체가 필요해지는 시점(배터리 기반 플랫폼 지원 등)의
  후속 과제로 남긴다.

### 7.3 범위 밖으로 명시하는 나머지 SuperSpeed 전용 기능

- **USB 3.x 허브의 확장 Hub Depth 처리**: 허브를 거친 장치의 U1/U2
  Exit Latency 재계산(허브 depth만큼 누적) - 클래스 드라이버/허브
  지원 자체가 v1 범위 밖이므로 자연히 범위 밖.
- **Isochronous 전송의 확장 필드**(버스트/서비스 인터벌 등, 오디오/
  비디오 스트리밍용) - v1은 제어 전송(§3.3)까지만 다루므로 범위 밖.

이 두 항목은 "USB 클래스 드라이버는 v1 범위 밖"이라는 §1의 기존
결정과 일관된 자연스러운 결과이지 새로운 판단이 필요한 부분이 아니다.
