# PCIe 버스 스펙

**관련 결정**: ADR-006, ADR-007, ADR-038, ADR-039, ADR-040, ADR-041, ADR-056
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`servers/devmgr`)

이 문서는 devmgr가 PCIe 버스를 열거하고, 드라이버와 장치를 매칭하며,
핫플러그에 대응하는 방식을 정의한다. 커널에는 PCIe 전용 개념을
추가하지 않는다 — 모두 기존 MMIO 캐패빌리티·notification 메커니즘의
재사용이다 (ADR-039/040).

## 1. 설정 공간 접근 (ADR-038)

1. devmgr는 `BootInfo.arch_data_addr`(ACPI RSDP, spec/boot.md §3)를
   통해 ACPI 테이블을 자체 파싱(ADR-006)하여 **MCFG** 테이블을 찾는다.
2. MCFG가 있으면 그 안의 `{base_address, segment, start_bus, end_bus}`
   항목마다 ECAM 영역(버스당 1MB: 32 device × 8 function × 4KB)을
   자신의 주소공간에 매핑할 MMIO 캐패빌리티를 요청한다.
3. MCFG가 없거나 파싱 실패 시, x86_64에서는 레거시 I/O 포트
   (`CONFIG_ADDRESS=0xCF8`, `CONFIG_DATA=0xCFC`)로 대체한다 — 이 경우
   devmgr는 별도의 **I/O 포트 범위 캐패빌리티**(x86 전용, 신규)를
   필요로 한다.
4. ECAM 설정 공간 오프셋 계산: `addr = base + ((bus - start_bus) << 20 | device << 15 | function << 12) + offset`.

## 2. 버스 열거 (ADR-039)

devmgr는 부팅 후 다음을 수행한다:

1. bus 0부터 재귀적으로 각 device/function의 Vendor ID(오프셋 0x00)를
   읽어 `0xFFFF`(장치 없음)가 아니면 존재하는 것으로 판단.
2. Header Type(오프셋 0x0E)으로 PCI-to-PCI 브리지(0x01) 여부를 판별해
   하위 버스를 재귀 열거.
3. 각 장치의 Vendor ID/Device ID/Class Code/BAR들을 읽어 내부 장치
   테이블에 기록한다. 이 테이블이 §4 매칭의 대상이 된다.

## 3. 핫플러그 (ADR-040)

1. devmgr는 PCIe 핫플러그/PME 인터럽트에 대응하는 커널 notification
   핸들을 부팅 시 받는다(§ 커널 측 설정은 initrun 기동 매니페스트에서
   위임 — OPEN-29).
2. notification이 오면 devmgr는 §2의 열거를 다시 수행하고, 이전
   장치 테이블과 diff하여 추가/제거된 장치를 판별한다.
3. 추가된 장치는 §4 매칭을 다시 시도하고, 제거된 장치를 담당하던
   드라이버가 있으면 그 사실을 IPC로 통지한다(프로토콜은 §4).

## 4. 드라이버 등록/매칭 (ADR-041)

```cpp
// devmgr가 노출하는 등록용 엔드포인트에 드라이버가 보내는 메시지 (label 예시)
enum class devmgr_op : uint32_t {
    register_driver = 1,   // regs[0]=vendor_id, regs[1]=device_id (또는 class_code, regs[2]=mask)
    device_claimed  = 2,   // devmgr → 드라이버: 이 장치를 맡아라 (bus/dev/func, BAR 정보는 페이지로)
    device_removed  = 3,   // devmgr → 드라이버: 이 장치가 사라졌다
};
```

- 드라이버는 기동 후 `register_driver`로 자신이 처리할 ID(또는 클래스
  코드 범위)를 알린다.
- devmgr는 등록된 ID와 §2에서 열거한 장치를 매칭해 `device_claimed`로
  BAR·IRQ 정보를 넘긴다(대용량이므로 ipc.md §4의 페이지 전달 사용).
- 여러 드라이버가 같은 장치에 등록을 시도하는 경우, 기본은 **선착순**
  (먼저 등록한 드라이버가 소유, 이후 시도는 거부)이다. 다만 root
  권한 프로세스가 `/sys/etc`(vfs-layout.md)의 설정으로 특정
  장치-드라이버 바인딩을 고정해두면, devmgr는 그 설정을 선착순보다
  **우선 적용**한다 — 관리자가 지정한 드라이버가 기존 선착순
  소유자를 대체할 수 있다(ADR-056). 대체된 기존 드라이버는
  `device_removed`를 받는다.

## 아직 정하지 않은 것

- 커널이 어떤 인터럽트 벡터를 PCIe 핫플러그/PME용 notification으로
  연결할지의 구체적 설정 경로 (initrun 기동 매니페스트 설계와 함께, OPEN-29).
- I/O 포트 범위 캐패빌리티(ADR-038 레거시 경로)의 정확한 형태 —
  ADR-011 핸들 모델의 새 객체 종류로 추가할지, 별도 메커니즘으로
  둘지는 실제 레거시 경로 구현 시점에 결정.
