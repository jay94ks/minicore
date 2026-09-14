# 장치 자동 인식 및 핫플러그 프레임워크(PnP) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-9DD4F3EA
  status: review
  updatedAt: 2026-09-14T15:45:39.896Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# 장치 자동 인식 및 핫플러그 프레임워크("PnP") — 설계 제안

설계자 지시(2026-09-14, 메시지) - "AHCI와 PnP 그리고 USB도 설계
제안서 작성해줘." 세 건 중 다른 둘(AHCI/USB)의 기반이 되는 프레임워크
부터 먼저 제안한다 - AHCI/USB 둘 다 "장치가 어떻게 발견되고 어떤
드라이버로 연결되는지"를 이 문서에 의존한다.

## 0. 용어 정정 - "PnP"의 의미

전통적 "Plug and Play"(ISA PnP, BIOS PnP 서비스)는 1990년대 레거시
ISA 버스 시절의 특정 프로토콜로, x86-64 + ACPI 기반의 이 프로젝트와는
무관하다 - **범위에서 명시적으로 제외**한다. 이 문서가 실제로 다루는
것은 "장치 자동 인식(enumeration) + 드라이버 매칭 + 핫플러그 알림"을
포괄하는 현대적 의미의 PnP다(Linux의 udev/hotplug, Windows의 PnP
Manager와 같은 역할) - 이름은 설계자 지시를 따라 그대로 유지하되
범위는 이렇게 재정의했다.

## 1. 배경 및 위치

SP-8B6B8D25 §2 "커널이 담당하는 범위" 4번("인터럽트를 적절한 커널
서비스들에게 라우팅")과 5번("커널 서비스들에게 요청받은 IO 권한
부여")이 이 프레임워크의 커널 쪽 전제조건이다. §2-A 원칙("커널이
모든 통제권을 쥔 유저랜드 서비스")에 따라, 이 PnP 프레임워크 자체는
**devmgr(유저랜드) 안에서 구동**되고 커널은 다음만 제공한다:

- 부팅 시 이미 확보한 하드웨어 토폴로지 정보(PCI 장치 목록 -
  `Pci::enumerate()`, 이미 구현됨) 조회 경로.
- 핫플러그를 알리는 인터럽트를 devmgr로 라우팅(§2 4번, 계획
  PN-B3DD3D19 - 아직 미구현, 이 문서의 핵심 의존성).
- devmgr이 특정 장치의 MMIO/포트 I/O 영역·인터럽트 벡터를 요청하면
  권한을 내주는 syscall(§2 5번 - 아직 미구현, 별도 계획 필요).

## 2. 커널이 이미 제공하는 것 (재사용)

- **PCI 열거**: `minicore/kernel/pci.cpp`의 `Pci::enumerate()` -
  부팅 시 버스/장치/기능별로 vendor/device ID, class/subclass/
  prog-if를 이미 스캔해 로그로 남긴다(현재는 진단 로그 전용). 이
  결과를 devmgr에 IPC로 넘기는 syscall이 필요(§3.1 참고).
- **MSI/MSI-X**: PL-2070E6EF로 이미 구현 완료 - AHCI/xHCI 둘 다
  이 인터럽트 방식을 선호(레거시 INTx보다 코어별 라우팅이 쉬움).
- **ACPI MADT/SRAT**: 이미 파싱됨(`Acpi` 클래스) - PCIe 네이티브
  핫플러그가 아닌 ACPI 기반 핫플러그(GPE)를 쓰려면 추가로 DSDT/SSDT
  AML 인터프리터가 필요(이 프로젝트에 아직 없음 - §5 참고).

## 3. 설계 제안

### 3.1 장치 열거 경로 (커널 → devmgr)

새 syscall endpoint(가칭 `kSyscallEndpointEnumerateDevices` 류,
`SyscallRegistry`에 등록 - PL-21344323 패턴 재사용) 하나를 제안한다:

```cpp
struct EnumerateDevicesArgs {
    // in: 몇 번째 장치부터 반환할지(페이지네이션 - 결과가 커널 스택/
    // 버퍼 크기를 넘을 수 있음)
    uint32_t startIndex = 0;
    // in/out: 호출부가 준비한 배열 크기 / 실제로 채운 개수
    uint32_t capacity = 0;
    DeviceDescriptor* outDevices = nullptr;
    // out: 전체 장치 수(다음 호출의 startIndex 계산용)
    uint32_t totalCount = 0;
};

struct DeviceDescriptor {
    uint32_t bus, device, function;      // PCI 위치
    uint32_t vendorId, deviceId;
    uint32_t classCode, subclass, progIf;
    uint64_t mmioBases[6];               // BAR0~5(메모리 매핑인 것만, 포트I/O BAR는 0)
    uint32_t irqVector;                  // 0이면 아직 미배정(MSI/MSI-X 협상 전)
};
```

devmgr은 부팅 후 이 syscall을 반복 호출해 전체 PCI 장치 목록을 얻는다
(v1은 PCI만 - USB 장치는 USB 서브시스템 자체가 호스트 컨트롤러
드라이버 위에서 별도로 열거, SP(USB) §3 참고).

### 3.2 드라이버 매칭

devmgr 내부에 `DeviceManager`(가칭) 컴포넌트를 둔다:

- **매칭 테이블**: 드라이버마다 (vendorId, deviceId) 정확 일치 또는
  (classCode, subclass, progIf) 클래스 매칭 규칙을 등록.
- **드라이버 형태(v1 제안)**: 정적 링크 - devmgr 실행 파일 자체에
  AHCI/xHCI 등 드라이버를 컴파일해 넣고, `DeviceManager`가 매칭되는
  장치를 발견하면 해당 드라이버의 `probe(DeviceDescriptor&)`를
  호출하는 구조. **동적 드라이버 로딩(공유 라이브러리)은 v1 범위
  밖**으로 제안한다 - `userland/tests/libuserlandtest`가 이미 shared+
  static 빌드 검증을 계획 중이므로(RM-7C249618), 그 인프라가 자리
  잡은 뒤 재검토할 수 있다.
- **IO 권한 요청**: `probe()`가 성공(이 드라이버가 이 장치를 맡기로
  확정)하면 그 장치의 MMIO BAR/인터럽트 벡터에 대한 권한을 커널에
  요청하는 syscall을 호출(§3.3, AHCI/USB SP 양쪽이 공통으로 쓸 API).

### 3.3 IO 권한 부여 (devmgr → 커널)

SP-8B6B8D25 §2 5번을 실제 syscall로 구체화하는 제안:

```cpp
struct RequestIoPermissionArgs {
    uint32_t bus, device, function;  // 이 PCI 장치를 특정
    uint64_t mmioBase;               // 요청하는 BAR(0이면 "이 장치의 모든 BAR")
    // out
    ChannelError error;              // NotFound(장치 없음)/InvalidHandle(이미 다른 프로세스가 점유)
    uint64_t mappedVirtualAddr;       // 성공 시 devmgr 프로세스 주소공간에 매핑된 가상주소
    uint32_t assignedIrqVector;       // 성공 시 배정된 인터럽트 벡터(MSI/MSI-X 우선)
};
```

커널은 이 요청을 받으면: (1) 그 BAR가 이미 다른 프로세스에 배정돼
있지 않은지 확인(장치별 "소유자" 테이블 필요 - 신규 커널 자료구조),
(2) `Paging::mapPage`로 devmgr 프로세스의 주소공간에 그 물리 MMIO
영역을 `PAGE_USER | PAGE_CACHE_DISABLE`로 매핑, (3) MSI/MSI-X를
설정해 새 인터럽트 벡터를 배정하고 §2 4번(인터럽트 라우팅)에 따라
그 벡터가 devmgr로 전달되게 등록한다.

### 3.4 핫플러그

- **PCIe 네이티브 핫플러그**: PCIe 슬롯의 Slot Capability 레지스터
  (Presence Detect Changed 등)를 통한 인터럽트 - 커널이 이 인터럽트를
  받아 §2 4번 라우팅으로 devmgr에 전달, `DeviceManager`가 다시
  `Pci::enumerate()`(또는 그 특정 슬롯만 재스캔하는 축소판)를 호출해
  새/제거된 장치를 반영한다.
- **ACPI GPE 기반 핫플러그**: DSDT/SSDT AML 인터프리터가 필요해
  이번 v1 범위 밖으로 제안(별도 계획 - AML 인터프리터 자체가 상당한
  작업량).
- **USB 핫플러그**: 호스트 컨트롤러(xHCI)의 포트 상태 변경 인터럽트로
  감지 - USB SP §4 참고, 이 프레임워크의 "핫플러그 알림 → 드라이버
  매칭 재실행" 흐름을 그대로 재사용한다.

## 4. 아직 열려 있는 설계 영역

- **드라이버 실패/충돌 정책**: 두 드라이버가 같은 장치에 매칭되면?
  v1은 "먼저 등록된 순서로 하나만 시도, 실패하면 다음"으로 제안하지만
  확정 아님.
- **드라이버 우선순위/블랙리스트**: 커널 커맨드라인으로 특정 장치
  드라이버를 비활성화하는 기능 필요 여부.
- **다중 devmgr 인스턴스 여부**: 지금은 devmgr 하나가 모든 장치를
  관리한다고 가정 - 실제로 devmgr이 여러 프로세스로 나뉠지(예: 스토리지
  전용/네트워크 전용) 아직 미정(SP-8B6B8D25는 "devmgr" 단수로만
  언급).

## 5. 선행 조건 (계획으로 등록 예정)

- 프로세스 모델(PN-16CA347D) - devmgr 자체가 실행되려면 필요.
- 커널→유저 인터럽트 라우팅(PN-B3DD3D19).
- IO 권한 부여 syscall(§3.3, 이 문서가 처음 구체화 - 신규 계획 등록
  필요).
- 프로세스간 공개 인터페이스 registry(PN-268F062B) - devmgr이 다른
  유저 프로세스에게 "이 장치를 내가 담당한다"를 알리는 데 쓰일 수
  있음(예: fs 서비스가 AHCI 드라이버를 통해 디스크 I/O를 요청하는
  경로).
