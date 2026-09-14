# Slab 할당자(libkmm) 구현

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: PL-16E2CDA4
  status: approved
  updatedAt: 2026-09-14T11:53:38.212Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# 실행 계획: Slab 할당자(libkmm)

**[구현 완료, 2026-09-14 - 계획 PN-DC75D601]** SP-D7013B26(2026-09-14 확정, QU-4F905C08)의
Slab 할당자를 구현했다. **착수 전제조건**이었던 스케줄러(PL-2D3184BC)의
선점 비활성화 프리미티브(`Scheduler::disablePreemption()`/
`enablePreemption()`/`PreemptionGuard`)가 먼저 완료되어 이 계획에
그대로 재사용했다.

## 배경

SP-D7013B26 참고 - Bonwick-Adams 매거진/디포 모델. 원래 제안(코어당
매거진 1개)이 검토 결과 **더블 매거진(loaded/previous)** + **선점
방지 강제**로 확정됐고, 매거진 용량(16)/버킷 크기(32~2048, 7단계)/
슬랩 order(0)/고갈 정책(즉시 nullptr)이 전부 숫자로 고정됐다(추가
DC 논의 불필요, 설계자 명시).

## 구현 (완료)

0. **[완료] `PreemptionGuard`**: PL-2D3184BC에서 이미 `scheduler.h`에
   구현됨(`Scheduler::disablePreemption()`/`enablePreemption()` 코어별
   카운터 위의 RAII) - 이 계획은 그걸 그대로 재사용, 별도 구현 불필요.
1. **`libkmm` 디렉터리 신설**: `minicore/libs/libkmm/CMakeLists.txt` +
   `slab.h/.cpp`(SP-8B6B8D25 §3.1에 이름만 있던 것의 첫 실제 구현).
   **설계 판단 하나 기록**: libkmm은 "재사용 가능한 코드만 모은
   라이브러리"로 소개돼 있지만, SP-D7013B26의 코드 스케치 자체가
   이미 `PageFrameAllocator`/`PreemptionGuard`(둘 다 커널 전용)를
   추상화 계층 없이 직접 호출하는 형태라, 이번 구현도 그대로 따라
   `minicore/kernel` 헤더를 직접 include했다(다른 `libs` 모듈처럼
   아키텍처/freestanding 독립적으로 만들지 않음) - 미래에 유저랜드
   재사용이 실제로 필요해지는 시점에 추상화를 걷어내는 편이, 지금
   쓰이지도 않을 이식성을 미리 설계하는 것보다 낫다고 판단했다(구현
   세부 판단, 새 DC 불필요 수준).
2. **`SlabCache`**: `alloc()`/`free()` 진입 시 `PreemptionGuard`로
   이 코어의 선점을 비활성화한 채 `Scheduler::currentCoreIndex()`로
   자기 코어의 매거진에 접근한다. `loaded`/`previous` 더블 매거진
   (각 16개 고정) - 매거진 자체를 디포 lock-free 스택의 **노드 겸용**
   (`MagazineNode{ next, count, items[16] }`)으로 설계해, 코어<->디포
   교환이 **값 복사 없이 포인터만 옮기는** 형태가 되게 했다(SP-D7013B26
   §2.2의 도식 그대로, 다만 "매거진 자체 = 디포 노드"로 구현을 단순화
   - 총 `MagazineNode` 개수는 `2 * kMaxCores`로 고정, 런타임 중 새로
   만들거나 없애지 않고 코어 소유 <-> 디포 소속 사이만 이동한다).
   `alloc()`/`free()` 둘 다 표준 Bonwick 알고리즘(둘 다 가득/둘 다
   빔일 때만 디포와 1:1 교환) 그대로 구현.
3. **전역 디포(lock-free)**: `AtomicPtr<MagazineNode>` 기반 Treiber
   스택 두 개(가득/빈 목록) - `pushDepot`/`popDepot`이 CAS 루프로
   구현(`libkenv/spinlock.h`의 `AtomicPtr::compareExchange` 재사용,
   스케줄러 큐와 같은 원자 프리미티브). **콜드 스타트 폴백**: 매거진
   두 개가 다 가득 찼는데 디포의 빈 매거진 예비분마저 없는 극단적
   경우(단일 코어 반복 테스트에서 실제로 관찰됨 - 디포는 코어 간
   교환이 있어야 채워지므로 코어 하나만 계속 alloc/free를 반복하면
   디포를 아예 안 거치고 매거진<->원시 슬랩 계층만 오간다), 매거진
   계층을 건너뛰고 원시 슬랩 페이지 free-list에 바로 반납/할당하는
   폴백 경로를 뒀다 - 항상 안전하고, 디포가 정상적으로 도는 경우
   (여러 코어가 실제로 교차 할당/해제할 때)엔 자연스럽게 우회된다.
4. **원시 슬랩 페이지 계층**: `PageFrameAllocator::allocPage()`
   (Order 0 고정, §2.2)로 4KiB 페이지를 받아 객체 크기로 등분하고,
   빈 객체의 첫 8바이트에 다음 포인터를 겹쳐 쓰는 침습적 free-list로
   관리한다(그래서 최소 버킷 32B가 필요 - §2.3). 이 계층은 **여러
   코어가 동시에 건드릴 수 있어** `Spinlock`으로 보호했다(설계
   문서가 이 계층의 동시성 메커니즘을 명시하지 않아 - 코어별
   매거진과 lock-free 디포는 명시돼 있었음 - 기존 `TaskQueue`와
   같은 패턴의 Spinlock 폴백으로 구현, 새 DC 불필요 수준의 구현
   세부로 판단).
5. **`sizeToBucket`/`GenericSlabAllocator`**: 7단계 버킷(32/64/128/
   256/512/1024/2048)마다 `SlabCache` 인스턴스. `alloc(size)`/
   `free(ptr, size)`가 전부 `sizeToBucket`을 거치도록 강제(호출부가
   잘못된/오래된 크기를 넘겨도 항상 같은 버킷으로 되돌아감). 32B
   미만 요청은 32로 취급(금지가 아니라 안전하게 올림 처리 - 설계
   문서의 "32B 미만 요청 금지"는 "그 밑으로 버킷을 만들지 않는다"는
   뜻으로 해석, 호출부가 실수로 작은 값을 넘겨도 패닉 대신 최소
   버킷으로 처리). **2048B 초과 요청은 슬랩을 거치지 않고
   `PageFrameAllocator::allocOrder`/`freeOrder`로 직접 위임**한다 -
   이때 필요한 가상<->물리 역변환용으로 `paging.h`에 `kVirtToPhys`
   (`kPhysToVirt`의 대칭 역함수)를 신설했다.
6. **부팅 연결**: `kMain`에서 `PageFrameAllocator::init()` 직후,
   `Lapic::init()` 이전에 `GenericSlabAllocator::init()`을 호출한다
   (`init()` 자체는 정적 구조만 채우므로 `Scheduler::
   currentCoreIndex()`가 필요해지는 실제 `alloc()`/`free()` 호출
   전이면 아무 때나 무방함).

## 검증(QEMU, 완료)

임시 디버그 테스트(검증 후 제거)로 다음을 확인했다 - 단일 코어
(PVH)와 4코어 SMP(GRUB+q35 `-smp 4`) 둘 다 회귀 없이 통과:

- **버킷 재사용/유일성**: 64B 버킷 40개(매거진 2개 용량 32개를
  초과 - 원시 슬랩 계층까지 자연히 타도록) 할당 시 전부 서로 다른
  포인터(`PASS`), 전부 해제 후 다시 40개 할당해도 전부 성공(`PASS`,
  재사용 확인).
- **2048B 초과 직행 경로**: 5000B 요청이 성공적으로 `PageFrameAllocator`
  경로를 타고 정상 해제됨(`PASS`).
- **32B 미만 올림 처리**: 4B 요청이 안전하게 성공(`PASS`).
- **고갈 시 비블로킹 nullptr**: 1MiB 단위로 최대 256회(256MiB) 반복
  할당 - 이 환경의 실제 usable 메모리(약 124MiB)를 초과하는 시점에
  블로킹 없이 정확히 `nullptr`을 반환함을 확인(`PASS`, 124MiB
  할당 후 고갈), 이후 전부 해제해 메모리를 완전히 복구하고 뒤이은
  SMP AP 스택 할당(3개 AP 모두 정상 기동)에 영향 없음을 확인.

검증 후 임시 테스트 코드는 제거 - 영구 코드는 `libkmm/slab.h/.cpp`,
`CMakeLists.txt` 변경분, `paging.h`의 `kVirtToPhys`, `kmain.cpp`의
`GenericSlabAllocator::init()` 호출 한 줄만 남았다.

## 이번 구현에서 내린 구현 세부 판단 (새 DC 불필요 수준)

- libkmm이 커널 헤더를 직접 include(추상화 없음) - 위 1번 항목.
- 원시 슬랩 페이지 free-list의 동시성은 Spinlock(디포는 설계대로
  lock-free 유지) - 위 4번 항목.
- 매거진을 디포 노드와 통합해 값 복사 없이 포인터만 이동 - 위 2번
  항목(SP-D7013B26의 도식이 보여준 개념은 그대로 유지, 구현 형태만
  단순화).
- 32B 미만 요청은 실패시키지 않고 최소 버킷(32)으로 올림 - 위 5번
  항목.

## 남은 것

- **512~2048 버킷의 Order 1 슬랩 적용 여부** (계획 PN-DE8C27B1): SP-D7013B26 §5가
  "실측 후 별도 검토"로 열어 둔 항목 - 실제 워크로드로 낭비율을
  계측해 판단한다.

## 다음 소비자

**AsyncTask 프레임워크(SP-F682B889, PL-1E247831)**가 이 Slab
할당자의 첫 실제 소비자다 - `args`/`AsyncTask` 전용 스택 확보에
`GenericSlabAllocator::alloc/free`를 재사용할 예정.
