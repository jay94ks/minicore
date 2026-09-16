# 장치 자동 인식 및 핫플러그 프레임워크(PnP) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-9DD4F3EA
  status: approved
  updatedAt: 2026-09-16T15:00:00.989Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
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
- **드라이버 형태(v1 제안)**: 코드는 정적 링크(동적 드라이버 로딩/공유
  라이브러리는 v1 범위 밖 - `userland/tests/libuserlandtest`가 이미
  shared+static 빌드 검증을 계획 중이므로, RM-7C249618, 그 인프라가
  자리 잡은 뒤 재검토 가능). **[갱신, 2026-09-15, §4a-3 답변 반영]
  단 실행은 devmgr 프로세스 자신 안이 아니라 devmgr이 스폰하는 별도
  자식 프로세스에서 이뤄진다** - `DeviceManager`가 매칭되는 장치를
  발견하면 해당 드라이버 진입점을 자식 프로세스로 스폰하고
  `probe(DeviceDescriptor&)`를 그 자식 프로세스 안에서 실행한다(코드
  자체는 devmgr 실행 파일에 정적 링크돼 있지만, 실제 구동 시점에
  독립 프로세스로 분리 실행 - "정적 링크"와 "별도 프로세스"는
  모순이 아니다, 하나의 바이너리가 여러 진입점/모드로 재실행되는
  패턴은 SP-68182FBD의 `init`/일반 유저 프로그램 구분과 유사).
  **이유**: 장치 격리/장애 전파 방지(드라이버 하나의 크래시가 devmgr
  전체나 다른 드라이버를 끌고 내려가지 않음, PN-645CF608의 Resurrect가
  이 자식 프로세스 단위에도 적용 가능) - §4a-3(다중 devmgr 인스턴스
  질문)에 대한 설계자 답변이 이 구조를 명시적으로 확정했다("devmgr
  인스턴스는 무조건 1개고 그 밑에 자식 프로세스들이 달려야 하는
  구조로만 설계") - §3.3a가 이미 전제하고 있던 "devmgr이 probe() 성공
  후 띄우는 드라이버 자식 프로세스"(SP-EAB162FC §4/QU-3AAAB5E9 답변
  참고)와도 이제 정합성이 맞다(이전엔 이 §3.2만 "정적 링크=같은
  프로세스"로 잘못 읽힐 수 있는 서술이었다).
- **IO 권한 요청**: `probe()`가 성공(이 드라이버가 이 장치를 맡기로
  확정)하면 그 장치의 MMIO BAR/인터럽트 벡터에 대한 권한을 커널에
  요청하는 syscall을 호출(§3.3, AHCI/USB SP 양쪽이 공통으로 쓸 API) -
  **이 syscall의 실제 호출자는 devmgr 자신이 아니라 위 드라이버 자식
  프로세스**다(§3.3a와 일치).

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

**[확정, SP-EAB162FC §4/QU-3AAAB5E9 답변, 2026-09-15]** 이 syscall은
호출자의 `ProcessRole`을 검증하지 **않는다** - `DeviceOwnerTable`
확인(§3.3a, "이 BAR를 이미 누가 점유했는가")만으로 충분하다고
확정됐다. devmgr이 `probe()` 성공 후 띄우는 드라이버 자식
프로세스(AHCI/USB 등, §3.2)가 이 syscall의 실제 호출자인 경우가
많은데, 이들을 `ProcessRole::Normal`로 두고 여기서 role 검증까지
추가하면 정상 흐름 자체가 막히기 때문이다.

**신뢰 근거(role 검증 없이도 안전한 이유)**: 설계자 답변대로,
devmgr이 역할별로 분기해 띄우는 드라이버 프로세스들은 전부 유저랜드
에서 구동되는 **"커널 프로세스"**로 취급한다 - 임의의 서드파티
유저 프로세스가 아니라, OS/드라이버 설치 시점에 사용자에게 이미
고지된 신뢰 컴포넌트라는 전제다(SP-EAB162FC §2.2 개정 참고 - PnP
드라이버 스폰 경로 자체가 devmgr이라는 KernelService만 도달 가능한
고정 경로이므로, 그 경로로 태어난 프로세스는 role 필드 없이도
이미 신뢰 경계 안에 있다). 그래서 `RequestIoPermission`이 이
syscall 자체에서 role을 되짚어 확인할 필요가 없다 - 신뢰는 스폰
경로 자체가 이미 보장한다.

커널은 이 요청을 받으면: (1) 그 BAR가 이미 다른 프로세스에 배정돼
있지 않은지 `DeviceOwnerTable`(신규 커널 자료구조 - §3.3a)로 확인,
(2) `ProcessAddressSpaceManager::findGap(len, align)`으로 devmgr
프로세스 주소공간에 가상주소를 할당한 뒤 `VmaBacking::FixedPhysical
{ paddr, flags: (Read | Write | Uncacheable) }`로 `store()`해
Maple Tree에 정식 등록(3) MSI/MSI-X를 설정해 새 인터럽트 벡터를
배정하고 §2 4번(인터럽트 라우팅)에 따라 그 벡터가 devmgr로 전달되게
등록한다.

### 3.3a 자원 소유권 및 정리 (설계자 답변, QU-4D8D1F59, 2026-09-15)

**갱신 배경**: 이 §3.3은 원래 `Paging::mapPage`를 devmgr 프로세스
주소공간에 직접 호출하는 설계였다 - 이 문서 작성 당시엔 맞았지만,
지금은 `ProcessAddressSpaceManager`(SP-2AAD7C8D §2, PN-012E8C1A로
구현 완료)가 존재한다. 직접 `Paging::mapPage`를 호출하면 그 매핑이
Maple Tree에 기록되지 않아, devmgr 프로세스가 죽어도
`ProcessAddressSpaceManager::unmapAll()`(PN-71C3D483 항목 3, 프로세스
자원 회수) 경로에 걸리지 않고 영구히 새는 자원이 된다 - 위 (2)단계를
`findGap`+`store(VmaBacking::FixedPhysical)` 경로로 갱신한 이유다.
SP-39F18E30 §3.2(DMA 버퍼 관리자)가 이미 같은 패턴으로 갱신된 선례다.

**`DeviceOwnerTable`** - 배정된 MMIO 영역/인터럽트 벡터/BAR마다 소유
프로세스를 기록하는 신규 커널 자료구조(위 (1)단계가 조회하는 대상).
**프로세스 종료 시퀀스(PN-71C3D483의 Process Teardown Hook)가 이
테이블도 반드시 함께 정리해야 하는 공식 요구사항**:

- 할당된 IRQ 벡터 반환 및 IO-APIC/MSI 라우팅 마스킹.
- BAR 레지스터 소유권 해제(`DeviceOwnerTable`에서 제거).

이 정리가 빠지면 devmgr crash-restart 시나리오에서 §3.3의
`InvalidHandle`(이미 다른 프로세스가 점유) 에러로 장치가 영구히
막힐 수 있다.

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
  확정 아님. **[갱신, 2026-09-15] §4a로 구체화된 제안 참고.**
- **드라이버 우선순위/블랙리스트**: 커널 커맨드라인으로 특정 장치
  드라이버를 비활성화하는 기능 필요 여부. **[갱신, 2026-09-15] §4a로
  구체화된 제안 참고.**
- **다중 devmgr 인스턴스 여부**: 지금은 devmgr 하나가 모든 장치를
  관리한다고 가정 - 실제로 devmgr이 여러 프로세스로 나뉠지(예: 스토리지
  전용/네트워크 전용) 아직 미정(SP-8B6B8D25는 "devmgr" 단수로만
  언급). **[갱신, 2026-09-15] §4a로 구체화된 제안 참고.**

### 4a. 위 세 항목에 대한 설계 제안 (2026-09-15, 설계자 확인 요청)

**1. 드라이버 매칭 우선순위/충돌 정책** - `DeviceManager`의 매칭
테이블이 (vendorId, deviceId) **정확 일치**와 (classCode, subclass,
progIf) **클래스 매칭** 두 방식을 섞어 등록한다는 §3.2 전제를 그대로
받아, 구체적 우선순위 규칙을 제안한다:

1. 한 장치에 매칭되는 후보를 **특이도(specificity) 순으로 정렬** -
   정확 일치(vendorId+deviceId) 드라이버가 항상 클래스 매칭 드라이버
   보다 먼저 시도된다(예: 특정 벤더의 AHCI 컨트롤러 전용 드라이버가
   있으면 범용 AHCI 클래스 드라이버보다 우선).
2. 같은 특이도 안에서는 **등록 순서**(devmgr 빌드 시점에 정적으로
   링크된 순서, 코드 리스트 순)로 하나씩 `probe()`를 시도한다.
3. `probe()`가 실패(장치가 이 드라이버가 기대하는 세부 조건을 충족
   못 함 - 매칭 테이블 자체는 맞았지만 실제로는 못 다루는 경우, 예:
   특정 리비전만 지원)하면 다음 후보로 넘어간다 - **성공하는 첫
   드라이버가 그 장치를 갖는다**(v1 원안 그대로, "충돌"은 애초에
   동시 소유가 아니라 순차 시도 실패 전파로 처리).
4. 모든 후보가 실패하면 그 장치는 "드라이버 없음"으로 로그만 남기고
   건너뛴다(패닉하지 않음, RM-23F4B687 §4 원칙 - 장치 하나의 결여가
   부팅을 막으면 안 됨).

**2. 드라이버 블랙리스트** - **[갱신, 2026-09-15, 설계자 답변 반영]
부트 커맨드라인 대신 설정 파일 경로로 확정**: 커맨드라인 토큰은
"너무 길어질 수 있다"는 이유로 기각되고, 대신 fs 접근이 가능해진
시점에 `/sys/etc/pnp.cnf`(RM-C65F7760 VFS 구조의 `/sys/etc` 아래)를
읽어 **블랙리스트 + 우선순위 조정**을 함께 로드하는 방식으로
확정됐다:

- **경로**: `/sys/etc/pnp.cnf`(SP-8B6B8D25 §4의 `/sys/etc` 아래,
  일반 텍스트 설정 - 정확한 파싱 형식은 착수 시점에 다른 `/sys/etc/*`
  설정 파일과 일관되게 확정, 아직 이 프로젝트에 `/sys/etc/*` 설정
  파일 선례가 없다면 이 문서가 첫 사례가 될 수 있음).
- **부팅 초기 가용성 문제**: `fs` 서비스(SP-7CC5693A)가 아직 마운트를
  끝내기 전(devmgr이 가장 먼저 뜨는 커널 서비스 중 하나일 가능성이
  높음)에는 `/sys/etc`가 아직 없을 수 있다 - 이 경우 devmgr은
  initrd(CPIO) 안에 같은 이름/내용으로 동봉된 `pnp.cnf`를 폴백으로
  읽는다(CPIO 파싱 인프라는 `minicore/libs/libcpio`로 이미 존재,
  PL-FC38956C 참고) - **실제 fs가 준비되면(§2.5의
  `WaitForUserlandReady`류 신호, SP-7CC5693A 참고) `/sys/etc/pnp.cnf`
  로 다시 읽어 갱신**(설정 우선순위: 실제 fs 버전 > initrd 동봉
  버전).
- **내용**: 블랙리스트(vendorId:deviceId 목록 - probe() 자체를 건너뜀)
  와 우선순위 조정(특정 드라이버를 매칭 순서보다 앞당기거나 뒤로
  미루는 명시적 지정)을 함께 담는다 - §4a-1의 기본 우선순위 규칙
  (특이도 → 등록 순서)은 이 설정 파일의 명시적 지정이 없을 때의
  기본값으로 유지된다.
- 부트 커맨드라인 기반 안(`pnp.blacklist=...` 토큰)은 폐기.

**3. 다중 devmgr 인스턴스** - **v1은 SP-8B6B8D25가 이미 가정한 대로
devmgr 단수 유지**를 제안한다. 근거: (a) 이 문서 §3.2가 이미 devmgr
내부에 정적 링크된 여러 드라이버가 공존하는 구조를 전제하므로 devmgr
프로세스 자체를 나눌 필연적 이유가 아직 없다, (b) 장치 격리/장애
전파 방지가 목적이라면 "devmgr 하나가 죽으면 모든 드라이버가 같이
죽는다"는 위험이 있지만, 이는 PN-645CF608의 Resurrect(크래시 루프
방지 포함) 메커니즘이 이미 KernelService 프로세스 전반에 적용되는
완화책이라 당장 분할이 필수는 아니다, (c) 분할은 devmgr 간 통신
(어느 devmgr이 어느 장치를 맡는지 조율)이라는 새 설계 축을 열어
지금 확정하기엔 이르다. **분할이 필요해지는 신호**(예: 특정
드라이버 카테고리의 반복적인 불안정성, 격리 요구사항)가 실측으로
나타나면 별도 아키텍처 결정으로 재검토 - 이번 답변은 "영구히
안 한다"가 아니라 "v1 범위 밖"이다.

**[갱신, 2026-09-15] 설계자 답변으로 확정** - "devmgr 인스턴스는
무조건 1개고 그 밑에 자식 프로세스들이 달려야 하는 구조로만 설계해야
해(권한 관리와 누수 문제 유발 방지)." devmgr 단수 유지는 그대로
확정됐고, 추가로 "각 드라이버가 devmgr의 자식 프로세스로 격리 실행"
구조 자체가 **v1의 고정 아키텍처**로 못박혔다(위 §3.2 갱신 참고 -
이전 §3.2의 "정적 링크=같은 프로세스" 서술을 "정적 링크+자식 프로세스
스폰"으로 정정한 근거가 이 답변이다).

DC 없이 임의로 확정하지 않고 질의로 등록해 둔다(RM-23F4B687 §4
원칙, CLAUDE.md 규칙 4).

## 5. 선행 조건 (계획으로 등록 예정)

- 프로세스 모델(PN-16CA347D) - **[갱신, 2026-09-15] 완료됨** - devmgr
  자체가 실행되려면 필요했던 이 전제는 충족됐다. 실제 devmgr 프로세스
  스폰 체계(PN-D3C05C0B)도 **[재갱신, 2026-09-16] 완료됨**(commit
  6fae6c1) - 다만 devmgr 자신의 실행 파일 내용(minicore/devmgr 실코드,
  PN-BD9AAE2F)이 아직 없어 "스폰 메커니즘이 준비됐다"와 "devmgr이
  실제로 뜬다"는 여전히 별개다(현재는 initrd에 devmgr 이름의
  바이너리가 없어 "not found, skip"으로 처리됨).
- 커널→유저 인터럽트 라우팅(PN-B3DD3D19, `scheduled` - 설계 확정,
  실제 배선은 미착수).
- IO 권한 부여 syscall(§3.3, 이 문서가 처음 구체화 - 신규 계획 등록
  필요).
- 프로세스간 공개 인터페이스 registry(PN-268F062B) - **[갱신,
  2026-09-16] 설계 완료(SP-B071E628, 설계자 확인 대기, §6 참고)**
  - devmgr이 다른
  유저 프로세스에게 "이 장치를 내가 담당한다"를 알리는 데 쓰일 수
  있음(예: fs 서비스가 AHCI 드라이버를 통해 디스크 I/O를 요청하는
  경로 - SP-B071E628 §4가 이 구체 흐름을 다룬다).

## 6. devmgr 메인 서비스 시퀀스 (설계자 지시, 2026-09-16, "FS 서비스
및 devmgr 서비스 설계 착수하라")

위 §§가 각자 다룬 조각(장치 열거/드라이버 매칭/IO 권한/핫플러그/
blacklist)을 devmgr 프로세스 하나의 실제 시작 시퀀스로 엮는다 - 새로운
설계 결정은 없고 기존 설계를 순서대로 나열하는 것이 이 절의 목적이다:

1. **스폰**: `kmain.cpp`의 부팅 매니페스트(SP-EAB162FC §2.2 - initrd
   안의 `devmgr`이라는 이름과 정확히 일치하는 실행 파일을 커널이
   직접 스폰, `ProcessRole::KernelService` 부여)로 `init`과 별개로
   기동된다 - **PN-D3C05C0B가 "착수 조건"으로 요구하던 "부팅
   매니페스트/서비스 기동 체계" 설계가 바로 이 §2.2다**(정정 내용은
   PN-D3C05C0B 본문 참고).
2. **PCI 열거**: `EnumerateDevices`(§3.1)를 반복 호출해 전체 PCI
   장치 목록을 로컬에 캐시.
3. **설정 로드**: `/sys/etc/pnp.cnf` 시도 - `fs` 서비스가 아직
   마운트를 끝내지 않았을 가능성이 높으므로(devmgr이 가장 먼저 뜨는
   서비스 중 하나) 실패하면 initrd 동봉 `pnp.cnf`로 폴백(§4a-2).
   fs가 나중에 준비되면(SP-7CC5693A §2.5류 신호) 실제 파일로 다시
   읽어 갱신.
4. **드라이버 매칭**: 캐시된 장치 목록 각각에 대해 §4a-1의 우선순위
   규칙(특이도 → 등록 순서, blacklist 제외)으로 `probe()` 순차 시도.
5. **드라이버 자식 스폰**: `probe()` 성공 시 해당 드라이버를 자식
   프로세스로 스폰(§3.2, `ProcessRole::KernelService` 상속 -
   SP-EAB162FC §2.2 개정).
6. **자식 쪽**: `RequestIoPermission`(§3.3)으로 BAR/IRQ 확보 →
   자체 데이터 Channel `openChannel()`. **[정정, 2026-09-16,
   SP-B071E628 §5-A/§5-B 재확인]** 한때(2026-09-15~16 사이) 이
   지점에서 `pubreg`에 `register`하는 것으로 갱신했었으나, 그 직후
   SP-B071E628 §5-A(설계자 지시 "커널 서비스들이 pubreg에 뭔가를
   등록하지 않아")가 이 전제 자체를 뒤집었다 - **커널 서비스(devmgr
   과 그 드라이버 자식 포함)는 pubreg에 등록하지 않는다**, fs가
   AHCI 등 블록 장치를 찾는 경로는 pubreg를 거치지 않고 기존 PnP
   "장치 열거 → IO 권한 요청" 패턴(바로 이 §3.1/§6, `EnumerateDevices`/
   `RequestIoPermission`)을 그대로 쓴다(SP-B071E628 §5-B가 명시).
   이 문서의 이 절이 그 반전을 반영하지 못한 채 남아 있었던 자리다 -
   `PublishInterface`(옛 커널 syscall)는 여전히 폐기 상태 그대로지만,
   "pubreg register로 대체"도 마찬가지로 폐기됐다(대체할 것 자체가
   없어짐). devmgr/드라이버 자식은 여기서 pubreg를 아예 모른다 -
   PN-BD9AAE2F(devmgr 구현 계획)가 이미 이 최종 상태로 반영돼
   있었다(이 문서만 갱신이 빠져 있었음).
7. **핫플러그 대기**: PCIe Slot Capability/USB 포트 상태 변경
   인터럽트(§3.4)를 PN-B3DD3D19 라우팅으로 수신 대기 - 도착하면
   2번부터 그 슬롯/포트만 재실행.

이 시퀀스 자체의 실제 구현은 devmgr 프로세스의 최초 코드(현재
존재하지 않음)가 필요하므로, 착수 시 이 절을 그대로 구현 체크리스트
삼아 새 PN 계획으로 등록한다(CLAUDE.md 규칙 7) - 이 문서는 설계까지만
다룬다.

**[해소, 2026-09-16, DC-6E2500A6 답변]** 2/4/7단계(`EnumerateDevices`/
`RequestIoPermission`/핫플러그 통지)가 "커널↔커널서비스 고속 채널"
결정에 걸려 재설계될 수 있다고 우려했던 것은 **재설계 불필요**로
확정됐다 - SP-00CA7175 §3 전수 검토 결과 전부 소용량 제어 트래픽이라
Channel IPC에 `exclusivePreemptive` 플래그만 얹으면 충분(Tier B).
Tier A(전용 공유메모리 링버퍼)는 현재 실사용처 없음. 이 시퀀스는
그대로 유효하다.

