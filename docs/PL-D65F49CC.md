# x2APIC 지원

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: PL-D65F49CC
  status: approved
  updatedAt: 2026-09-13T16:23:32.587Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# 실행 계획: x2APIC 지원

**[완료, 2026-09-14]** QA-26450C3E 체크됨. 아래는 완료 기록으로 남긴다.

지금 Lapic은 xAPIC(MMIO) 모드만 구현돼 있다(DS-D4E5C451 - 설계자
지시로 고려 대상에 등록). x2APIC은 접근 방식 자체가 다르므로 Lapic을
백엔드 추상화로 바꾼다.

## 선행 조건

- 없음(SMP AP 기동 계획의 ICR 부분과 인터페이스를 맞추는 게 좋음 -
  ICR 전송 방식이 xAPIC/x2APIC에서 다름).

## 한 것

1. **CPUID 지원 확인**: `lapic.cpp`에 `kCpuidHasX2Apic()` 추가 -
   `cpuid` 인라인 어셈블리로 CPUID.01H:ECX 비트21을 직접 읽는다(별도
   CPUID 헬퍼 라이브러리 없이 이 파일 안에서 직접 처리 - 다른 곳에서
   아직 CPUID가 필요 없어서 공용화는 보류).
2. **Lapic 내부를 백엔드로 분리**: `Lapic::init()`이 `gUseX2Apic`
   플래그(부팅 후 고정, 런타임 전환 없음)를 CPUID로 한 번 정하고,
   `readRegister`/`writeRegister`/`id()`가 그 플래그에 따라 분기한다.
   공개 API 시그니처는 그대로라 `timer.cpp` 등 호출부는 전혀 안
   바뀌었다.
   - x2APIC 레지스터 = MSR `0x800 + (MMIO 오프셋 >> 4)`
   - EOI(MMIO 0xB0) → MSR 0x80B, spurious(MMIO 0xF0) → MSR 0x80F
   - ID 레지스터: xAPIC은 MMIO 0x020을 읽어 `>> 24`(상위 8비트),
     x2APIC은 MSR 0x802 하위 32비트가 ID 그 자체(시프트 없음, 8비트
     제한도 없음) - 그래서 `id()`를 모드별로 분기 구현했다.
3. **모드 전환**: x2APIC이면 `IA32_APIC_BASE` MSR에 enable 비트(11)와
   EXTD 비트(10)를 한 번에 세팅한다. 되돌리는(x2APIC→xAPIC) 경로는
   설계대로 지원하지 않는다(부팅 시 한 번만 결정, 미결 사항 아님 -
   PL 최초 설계 범위에 이미 명시됨).
4. **초기화 순서**: x2APIC 경로는 `Paging::mapPage`/
   `PageFrameAllocator::allocPage`를 아예 안 거친다(MSR 접근만 하므로
   MMIO 매핑 자체가 불필요) - 그래서 xAPIC 경로에 있던 Lapic↔
   PageFrameAllocator 닭-달걀 문제(관계도에 기록됨, `paging.cpp`/
   `page_frame_allocator.cpp`)가 x2APIC 경로에서는 애초에 발생하지
   않음을 확인했다. `isReady()`는 두 경로 공통으로 `gLapicReady`
   불린 플래그로 통일(기존엔 `gLapicVirtAddr != 0`이었는데, x2APIC은
   그 변수를 아예 안 씀).
5. **검증**: 로컬 WSL QEMU 8.2.2로 세 가지 환경 실측.
   - 기본(가속 없음/TCG, CPU 미지정=qemu64) → CPUID에 x2APIC 비트
     없음 → `mode=xapic id=0` (회귀 없음, PL-99562483까지의 기존
     경로 그대로).
   - `-cpu qemu64,+x2apic`(TCG) → QEMU가 부팅 시점에 **"TCG doesn't
     support requested feature: CPUID.01H:ECX.x2apic [bit 21]"**
     경고를 찍고 그 피처를 무시한다 - 즉 TCG(순수 소프트웨어 에뮬
     레이션)는 x2APIC 자체를 지원하지 않는다(코드 문제 아님, QEMU/
     TCG 자체의 한계). 그래서 이 조합에서도 `mode=xapic`으로 정상
     폴백함을 확인 - 폴백 로직 자체의 방어력 확인 목적으로는 유효한
     테스트였다.
   - `-accel kvm -cpu host`(하드웨어 가상화, 호스트 CPU가 실제
     x2APIC 지원) → `mode=x2apic id=0`으로 정상 전환. 이후
     spurious 벡터 설정(MSR write) → `Timer::init()`의 LAPIC 타이머
     100Hz 보정(LVT Timer/Divide/Initial Count 전부 MSR 경유) →
     `sti` → hlt 루프까지 폴트/패닉 없이 완주 확인. 즉 MSR 기반
     레지스터 접근 경로 자체가 실제로 동작함을 실기(하드웨어 가상화)
     환경에서 검증했다.
   - 세 로그 모두 확인 후 임시로 넣었던 CPUID 원시값(ecx) 디버그
     출력은 제거했다(진단용으로 유지한 건 `mode=` 로그 한 줄뿐).

## 이번엔 안 한 것 (후속 과제로 남김)

- **ICR(Interrupt Command Register) 지원**: xAPIC은 MMIO 0x300+0x310
  두 32비트 레지스터, x2APIC은 MSR 0x830 하나(64비트)로 통합돼 있어
  구성 자체가 다르다 - 지금 `Lapic`은 EOI/spurious/타이머 레지스터만
  다루고 ICR은 아직 없다. SMP AP 기동 계획(PL-65C20380 4단계)에서
  INIT-SIPI-SIPI를 구현할 때 이 차이를 반드시 반영해야 한다(관계도에
  기록 예정).
- **x2APIC 강제 비활성화 옵션**: 디버깅/호환성 목적으로 CPU가
  x2APIC을 지원해도 강제로 xAPIC을 쓰게 하는 경로 - 필요성이 아직
  안 보여 보류.

## 미결 사항

- 없음(x2APIC↔xAPIC 되돌리기 미지원은 최초 설계 범위에 이미 포함된
  결정이라 별도 질의 불필요).
