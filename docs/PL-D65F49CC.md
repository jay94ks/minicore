# x2APIC 지원

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: PL-D65F49CC
  status: review
  updatedAt: 2026-09-13T15:38:03.434Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# 실행 계획: x2APIC 지원

지금 Lapic은 xAPIC(MMIO) 모드만 구현돼 있다(DS-D4E5C451 - 설계자
지시로 고려 대상에 등록). x2APIC은 접근 방식 자체가 다르므로 Lapic을
백엔드 추상화로 바꾼다.

## 선행 조건

- 없음(SMP AP 기동 계획의 ICR 부분과 인터페이스를 맞추는 게 좋음 -
  ICR 전송 방식이 xAPIC/x2APIC에서 다름).

## 단계

1. **CPUID 지원 확인**: CPUID.01H:ECX 비트21(x2APIC) 확인 - libkenv나
   커널에 CPUID 실행 헬퍼가 아직 없으면 먼저 추가.
2. **Lapic 내부를 백엔드 인터페이스로 분리**: `readRegister`/
   `writeRegister`(현재 공개 API)의 구현을 xAPIC(MMIO)/x2APIC(MSR)
   두 가지로 나눈다 - 공개 API 시그니처는 그대로 유지해 호출부
   (timer.cpp 등)는 안 바뀌게 한다.
   - x2APIC 레지스터 번호 = 0x800 + (MMIO 오프셋 >> 4)
   - MSR 접근이라 가상주소 매핑(kLapicVirtBase) 자체가 필요 없어짐
   - ID 레지스터가 32비트 전체(APIC ID 8비트 제한 해제)
   - ICR이 64비트 MSR 하나(0x830)로 통합(xAPIC은 0x300+0x310 두
     32비트 레지스터였음) - SMP PL의 INIT-SIPI-SIPI 코드가 이 차이를
     알아야 함.
3. **모드 전환**: IA32_APIC_BASE MSR의 EXTD 비트(10)를 세팅해 x2APIC
   모드로 전환 - xAPIC으로 되돌리는 경로는 지원 안 해도 됨(부팅 시
   한 번만 결정).
4. **초기화 순서 재확인**: x2APIC은 MMIO 매핑이 필요 없어 Lapic::init
   이 Paging::mapPage/PageFrameAllocator를 안 거치게 될 수 있다 -
   PageFrameAllocator <-> Lapic 닭-달걀 문제(관계도 기록됨)가 x2APIC
   경로에서는 애초에 발생하지 않는지 재검토.
5. **검증**: QEMU를 x2APIC을 지원하는 CPU 모델로 띄워(`-cpu` 옵션
   확인 필요) EXTD 전환 후에도 LAPIC ID 조회/타이머/EOI가 xAPIC때와
   동일하게 동작하는지 확인. x2APIC 미지원 CPU(대부분의 기본 설정)
   에서는 xAPIC 경로가 그대로 동작하는지도 확인(회귀 방지).

## 미결 사항

- x2APIC이 되는데도 xAPIC을 강제할 옵션을 둘지(디버깅/호환성 목적).
