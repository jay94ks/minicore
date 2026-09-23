# 커널 Power(Shutdown/Reboot) 서브시스템 - ACPI FADT/PM1/Reset Register + 최소 DSDT _S5 스캔 - 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-0C7A4F3B
  status: approved
  updatedAt: 2026-09-23T09:07:06.986Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# 커널 Power(Shutdown/Reboot) 서브시스템 - 설계 제안

`DC-F367AD5D`/`QU-7C3AB7A2` 답변("새 syscall로 유저 프로세스가 요청 +
ACPI 전원 이벤트가 트리거, 둘 다 구현")을 구체화한 설계. `PN-0B461E6F`
가 구현 계획을 추적한다.

## §1. 범위

이 프로젝트는 범용 AML 인터프리터가 없다(그 자체로 별도의 큰
서브시스템 - ACPICA 포팅 수준의 작업이라 v1 범위 밖). 대신 실제
전원 제어에 필요한 최소한만 직접 구현한다:

1. **ACPI FADT("FACP") 확장 파싱** - `Acpi` 클래스(acpi.h/.cpp)에
   이미 있는 MADT/HPET/MCFG 파싱과 동일한 패턴(1바이트 단위로 ACPI
   스펙과 대조, 실제 QEMU로 실측 검증)으로 PM1 이벤트/제어 레지스터
   블록, SCI 인터럽트 번호, SMI_CMD/ACPI_ENABLE, ACPI 2.0+ Reset
   Register, DSDT 물리 주소를 옮긴다.
2. **최소 DSDT `\_S5` 패키지 스캔** - AML 바이트코드 전체를 해석하지
   않고, ACPI 스펙(§20.2.4 PkgLength 인코딩, §20.2.5 Data Objects
   Encoding)에 정의된 PackageOp/ComputationalData 인코딩 규칙만
   최소로 따라가 `\_S5` NameString 뒤의 Package에서 SLP_TYPa/
   SLP_TYPb 두 값만 뽑아낸다 - 이 프로젝트가 새로 고안한 인코딩이
   아니라 스펙 그대로.
3. **`Power` 클래스**(신설, power.h/.cpp) - 위 두 파싱 결과로 실제
   레지스터를 조작:
   - `shutdown()`: 필요하면 SMI_CMD에 ACPI_ENABLE을 써 ACPI 모드로
     전환한 뒤, PM1a(및 PM1b가 있으면 그것도) 제어 레지스터에
     `SLP_TYPa|SLP_EN`(bit13)을 쓴다(ACPI 스펙 §16.1.1 "Sleep Type
     and Sleeping").
   - `reboot()`: `Acpi::hasResetRegister()`가 참이면 ACPI Reset
     Register(Generic Address Structure - addressSpaceId로 I/O
     포트인지 메모리인지 판별)에 `resetValue`를 쓴다. 아니면 8042
     키보드 컨트롤러 리셋(포트 0x64에 0xFE) - 이 프로젝트가 실제로
     검증한 QEMU 기본 머신(`pc`/i440fx, SeaBIOS)은 RESET_REG_SUP
     플래그가 꺼져 있어(실측 확인, 아래 §3) 이 폴백이 실제 사용
     경로다.
4. **새 syscall 2종** - `Shutdown`/`Reboot`(그룹 10 "Power" 신규
   배정, `RM-48E1E610`). 유저 프로세스(초기엔 init 정도만)가 호출.
5. **ACPI 전원 버튼(SCI) 트리거** - PM1_EN의 PWRBTN_EN 비트를 세팅,
   SCI_INT(GSI)를 기존 인터럽트 서브시스템(IOAPIC 리다이렉션 +
   `interrupt_subscription.cpp`와 같은 급의 커널 내부 핸들러, 유저
   구독 아님)에 등록 - ISR은 PM1_STS의 PWRBTN_STS 비트만 확인/클리어
   하고 실제 종료는 아래 §6 훅 경로로 위임(ISR에서 직접 마운트
   순회/블로킹 I/O 금지 - 이 프로젝트의 기존 인터럽트 컨텍스트 규율과
   동일).
6. **`kernel::FileSystemDriver::onUnmount()` 훅**(기본 no-op) +
   `MountTable::unmountAll()` - Shutdown/Reboot/전원 버튼 트리거
   공통 경로가 실제 정지/재시작 전에 먼저 호출해 마운트된 볼륨마다
   dirty 비트 등을 정리할 기회를 준다(`PN-547EF839`가 이어받을 부분).

## §2. FADT 구조체 (실측 검증 완료, 2026-09-23)

`Fadt` 구조체(acpi.cpp, SdtHeader 뒤 offset 0부터) - ACPI 스펙 §5.2.9
그대로, X_PM1b_CNT_BLK(오프셋 196)까지만 옮기고 그 뒤(HYPERVISOR_VENDOR
등)는 이번 범위 밖이라 절단(SuperblockCore/InodeCore와 동일한
관례). 실제 QEMU(`pc`/i440fx, SeaBIOS)로 부팅해 TEMP 로그로 확인한
값:

```
sci=9 smi_cmd=0xb2 acpi_enable=0xf1 acpi_disable=0xf0
pm1a_evt_blk=0x600 pm1b_evt_blk=0 pm1_evt_len=4
pm1a_cnt_blk=0x604 pm1b_cnt_blk=0 pm1_cnt_len=2
reset_reg_sup=false (이 머신은 ACPI Reset Register 없음 - 8042 폴백 필수)
dsdt=0x7fe0040 dsdt_len=0x1ac6(6854)
```

이 값들은 OSDev 커뮤니티가 널리 문서화한 QEMU/SeaBIOS 기본값과
정확히 일치 - 실제 하드웨어/다른 BIOS는 다를 수 있으므로 항상
`Acpi::hasFadt()`/`hasResetRegister()`를 먼저 확인하고 그 값을
그대로 쓴다(하드코딩 안 함).

## §3. `\_S5` 스캔 알고리즘

ACPI 스펙 §20.2.4(PkgLength)/§20.2.5(ComputationalData) 인코딩을
그대로 구현:

1. DSDT 바이트 전체에서 ASCII `"_S5_"` 4바이트를 찾는다(AML
   NameString이 항상 4글자 고정 폭이라 이 검색만으로 충분 - 스펙
   §20.2.2).
2. 그 뒤 바이트가 `0x12`(PackageOp)가 아니면 오검출로 보고 계속
   찾는다.
3. PackageOp 뒤 PkgLength를 스펙대로 디코드(첫 바이트 bit7-6=0이면
   그 바이트의 하위 6비트가 길이, 아니면 하위 4비트+이어지는
   1~3바이트가 상위 비트) - 실제 길이 값 자체는 쓰지 않고 그
   인코딩이 차지하는 바이트 수만 건너뛰는 데 쓴다.
4. NumElements(1바이트) 건너뛴다.
5. 이어지는 두 ComputationalData(SLP_TYPa/SLP_TYPb)를 디코드 -
   `ZeroOp`(0x00)/`OneOp`(0x01)/`OnesOp`(0xFF)는 그 자체가 값,
   `BytePrefix`(0x0A)/`WordPrefix`(0x0B)/`DWordPrefix`(0x0C)는 그
   뒤 1/2/4바이트가 값.

일반 AML 인터프리터가 아니므로 `\_S5`가 `Scope`/`If` 등으로 감싸여
있거나 참조(다른 이름에 대한 별칭)로만 존재하는 드문 DSDT는 못
찾을 수 있다 - 그런 경우 `Power::hasS5()`가 false를 반환하고
`shutdown()`은 안전하게 실패(호출자에게 에러 반환, 하드웨어 조작
안 함)한다.

## §4. 권한

이 프로젝트의 사용자/권한 체계(`SP-30FCC8AE`)는 아직 개별 syscall에
uid 검사를 배선하는 단계가 아니다(`PN-24A2B6F5`/`PN-B6DB692C` 진행
중, 다른 어떤 기존 syscall도 아직 uid 게이팅이 없음 - 코드베이스
전수 확인). 이 v1도 같은 기준으로 호출자 uid를 검사하지 않는다 -
권한 체계가 실제로 배선되면(위 두 계획 완료 후) Shutdown/Reboot을
"root만" 정책 후보 1순위로 재검토할 것을 여기 남겨 둔다.

## §5. 참고
- `DC-F367AD5D`/`QU-7C3AB7A2` - 이 설계를 촉발한 결정.
- `PN-0B461E6F` - 구현 계획(진행 상황 추적).
- `PN-547EF839` - `onUnmount()` 훅의 첫 실사용처(libvfat dirty 비트).
- `RM-48E1E610` - 그룹 10 "Power" 배정.

