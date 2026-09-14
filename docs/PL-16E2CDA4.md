# Slab 할당자(libkmm) 구현

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: PL-16E2CDA4
  status: draft
  updatedAt: 2026-09-14T03:46:44.021Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# 실행 계획: Slab 할당자(libkmm)

**[설계 확정, 구현 대기, 2026-09-14]** SP-D7013B26(2026-09-14 확정,
QU-4F905C08)의 Slab 할당자를 구현하기 위한 순서를 남긴다. **착수
전제조건**: 스케줄러(PL-2D3184BC)의 선점 비활성화 프리미티브(8단계)
가 공개 API로 노출돼야 `PreemptionGuard`를 구현할 수 있다.

## 배경

SP-D7013B26 참고 - Bonwick-Adams 매거진/디포 모델. 원래 제안(코어당
매거진 1개)이 검토 결과 **더블 매거진(loaded/previous)** + **선점
방지 강제**로 확정됐고, 매거진 용량(16)/버킷 크기(32~2048, 7단계)/
슬랩 order(0)/고갈 정책(즉시 nullptr)이 전부 숫자로 고정됐다(추가
DC 논의 불필요, 설계자 명시).

## 구현 순서 (착수 계획)

0. **[선행조건] 스케줄러 선점 비활성화 프리미티브**: PL-2D3184BC
   8단계의 "선점 비활성화 카운터"를 공개 API로 노출 - 이 계획의
   `PreemptionGuard`가 그 위에 얹힌다. 이 계획 자체보다 먼저
   끝나야 하는 항목이라 PL-2D3184BC 쪽 진행 상황에 의존.
1. **`PreemptionGuard`**: RAII로 진입 시 선점 비활성화, 소멸 시
   복구. 코어 ID를 얻어 스코프 내내 고정됨을 보장(코어 마이그레이션
   방지) - 인터럽트 마스킹까지 필요한지는 스케줄러의 선점 비활성화
   구현 방식(타이머 인터럽트 자체를 막는지, 아니면 스케줄러 결정만
   미루는지)에 따라 갈림 - 착수 시 확인.
2. **`libkmm` 디렉터리 신설**: `minicore/libs/libkmm/CMakeLists.txt`
   (SP-8B6B8D25 §3.1에 이름만 있던 것의 첫 실제 구현) - `slab.h/.cpp`.
3. **`SlabCache`(단일 코어, 매거진 없이)**: 슬랩 페이지 확보
   (`PageFrameAllocator::allocOrder(0)`), 빈 객체 프리리스트 구성,
   기본 `alloc()`/`free()`를 **먼저 락 없는 단일 코어 가정으로**
   구현/검증(가장 단순한 버전부터 - 이후 단계에서 매거진을 얹음).
4. **더블 매거진 + `PreemptionGuard` 통합**: `loaded`/`previous`
   16개 고정 용량, alloc/free의 스왑 로직(2.2절)을 그대로 구현.
   `PreemptionGuard`로 감싸 코어별 매거진 접근을 보호.
5. **전역 디포(lock-free)**: 가득/빈 매거진 목록을 `AtomicPtr<T>`
   기반 lock-free 스택으로 구현 - 매거진 전체 단위로만 교환.
6. **`sizeToBucket`/`GenericSlabAllocator`**: 7단계 버킷(32/64/128/
   256/512/1024/2048)마다 `SlabCache` 인스턴스, `alloc(size)`/
   `free(ptr, size)`가 전부 `sizeToBucket`을 거치도록 강제. 2048B
   초과 요청은 `PageFrameAllocator`로 직접 위임(슬랩 우회).
7. **검증(QEMU)**: 단일 코어에서 alloc/free 반복(오염/누수 없음
   확인) → `-smp 2/4`로 멀티코어 alloc/free 동시 실행(매거진 스왑/
   디포 교환이 실제로 일어나는지, 코어별로 독립적인지 실측) →
   의도적으로 `PageFrameAllocator`를 고갈시켜(예: 대량 order 0
   할당 후 남겨두기) `GenericSlabAllocator::alloc`이 정확히
   `nullptr`을 반환하고 블로킹하지 않는지 확인.
