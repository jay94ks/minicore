# USB 스택(호스트 컨트롤러 + 장치 열거) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-E35FD36C
  status: review
  updatedAt: 2026-09-14T15:45:42.851Z
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
  (레거시 UHCI/OHCI/EHCI 지원은 실제 구형 하드웨어 요구가 생기면
  별도 계획으로 확장).
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
연속 메모리가 필요하다. **AHCI 제안 §3.2/§4-1과 동일한 해법(커널이
DMA 가능 버퍼를 확보해 devmgr에 물리주소와 함께 내주는 syscall)을
공유**할 것을 제안한다 - 두 드라이버가 각자 다른 메커니즘을 만들
이유가 없다.

### 3.3 제어 전송(Control Transfer)의 동기적 성격

디스크립터를 읽는 제어 전송은 "요청 → 완료까지 대기"가 자연스러운
동기적 흐름이다 - `Syscall::wait`(SP-04EE2A18) 패턴을 그대로 재사용해
"USB 제어 전송 하나 = syscall 하나"로 모델링할 것을 제안한다
(AsyncTask 프레임워크 위에서 자연스럽게 표현됨 - 이미 구현된 인프라
재사용).

## 4. 아직 열려 있는 설계 영역

1. **DMA 버퍼 확보 메커니즘** - AHCI 제안과 완전히 동일한 미결
   사항(공유 설계 필요, 두 SP 중 어느 한쪽에서 먼저 구체화하고
   서로 참조하는 형태를 제안).
2. **클래스 드라이버 로딩 방식** - PnP 제안과 동일하게 v1은 정적
   링크를 제안(HID/Mass Storage 등도 devmgr 실행 파일에 정적
   포함) - 동적 로딩은 후속.
3. **레거시(UHCI/OHCI/EHCI) 지원 여부와 시점** - 실제 구형 하드웨어
   요구가 생기기 전까지는 미룬다는 것이 이 문서의 제안이나, 최종
   확정은 설계자 판단 필요.
4. **USB 3.x SuperSpeed 전용 기능(예: 스트림, 링크 파워 관리)** -
   v1은 기본 열거/전송까지만, 고급 기능은 범위 밖으로 제안.

## 5. 선행 조건

- PnP 프레임워크(SP-9DD4F3EA)의 IO 권한 부여 syscall - xHCI
  컨트롤러 자체를 PCI 장치로 발견하는 데 필요.
- DMA 버퍼 확보 메커니즘(§4-1, AHCI 제안과 공유 설계).
- 프로세스 모델(PN-16CA347D), Syscall 서브시스템(SP-04EE2A18, 이미
  구현 완료 - 제어 전송 동기화에 재사용).
