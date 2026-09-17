# Paging::mapPage() 중간 테이블 생성 동시성 보호 - 락 전략 결정 요청

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-FB38F86F
  status: approved
  updatedAt: 2026-09-17T05:38:21.741Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

`PN-90BD044E`(SMP4 간헐적 페이지 폴트 조사, PN-584DB994 가설 3 중
정적 코드 감사로 발견)가 확인한 실재하는 버그: `paging.cpp`의
`kGetOrCreateNextLevel()`(PML4->PDPT->PD->PT 중간 레벨 테이블을
필요시 새로 만드는 공용 헬퍼, `Paging::mapPage()`/`mapRange()`가
호출)이 완전히 무잠금(lock-free)이다.

```cpp
kernel::uint64_t* kGetOrCreateNextLevel(kernel::uint64_t* parentTable, kernel::uint32_t index, kernel::uint64_t flags) {
    if (parentTable[index] & kernel::PAGE_PRESENT) {
        parentTable[index] |= flags;
        return kAsTable(parentTable[index] & kAddrMask);
    }
    const kernel::uint64_t newTablePhys = kernel::PageFrameAllocator::allocPage();
    kernel::uint64_t* newTable = kAsTable(newTablePhys);
    kZeroTable(newTable);
    parentTable[index] = newTablePhys | kernel::PAGE_PRESENT | kernel::PAGE_WRITABLE | flags;
    return newTable;
}
```

두 코어가 같은 `parentTable[index]`(아직 present 아님)를 동시에
통과하면 각자 다른 물리 페이지를 새 테이블로 할당해 각자
`parentTable[index]`에 쓴다 - 나중에 쓴 쪽이 이기고, 먼저 쓴 코어가
그 아래 레벨에 채운 매핑은 도달 불가능한 고아 테이블에만 남는다.
호출부(`mapPage()`)는 이 실패를 감지하지 못하고 성공한 것처럼
반환한다.

**지금 당장 재현되는 증상은 아니다** - `PN-584DB994`(SMP4 init
페이지 폴트, ~4-6%)의 직접 원인일 가능성은 낮다고 판단됨(AP 기동이
순차적이고, init의 주소공간 매핑 자체는 AP가 존재하기 전 BSP 단독
구간에 끝남 - 근거는 `PN-90BD044E` 참고). 그러나 devmgr이 드라이버
자식을 여러 개 스폰하거나, 여러 유저 프로세스가 동시에
`SpawnProcess`/향후 `mmap`류 syscall을 부르는 시나리오가 생기면
실제로 재현될 수 있는 독립적인 진짜 버그다.

## 왜 설계자 확인이 필요한가

`Paging::mapPage()`/`mapRange()`/`mergeRange()`는 이 커널에서 가장
자주 불리는 경로 중 하나(모든 프로세스 스폰, 모든 페이지 폴트
처리가 결국 여길 지난다) - 락 설계를 잘못 고르면 최악의 경우
데드락(`PageFrameAllocator`의 기존 per-node `Spinlock`과 중첩되므로
락 순서 규칙이 필요) 또는 병목(전역 락 하나로 모든 프로세스의
매핑 작업이 직렬화)으로 이어질 수 있어, CLAUDE.md 규칙 4에 따라
임의로 정하지 않고 확인을 구한다.

## 후보

- **(A) 주소공간(pml4Phys)별 전용 락** - 세밀하지만, higher-half
  구간(여러 프로세스의 PML4가 같은 물리 PDPT/PD/PT를 공유하는 커널
  공용 매핑 영역)은 이 락으로 안 덮여 별도의 전역 락이 추가로 더
  필요해진다 - 두 종류의 락이 공존하게 됨.
- **(B) 전역 페이지 테이블 락 하나** - 구현이 가장 단순하고 락 순서
  문제도 하나로 통일되지만, 서로 무관한 두 프로세스의 매핑 작업까지
  전부 직렬화되어 병목 우려(다만 지금 시점엔 실제 다중 프로세스
  워크로드 자체가 아직 없어 병목이 실측되지는 않음).
- **(C) 지금은 손대지 않고 계속 보류** - devmgr/fs 등 실제로 여러
  프로세스가 동시에 매핑을 만드는 시나리오가 나타날 때까지 기다렸다가
  그 시점에 실측 기반으로 다시 결정.

## 부수 결정 필요 사항

- `PageFrameAllocator`의 기존 per-node `Spinlock`과의 락 순서(중첩
  락 - `kGetOrCreateNextLevel()`이 이 락을 쥔 채로 `allocPage()`를
  불러 그 안에서 또 다른 락을 잡게 됨)를 어떻게 규정할지.
- (A)/(B) 중 하나를 채택한다면, 이 락을 잡는 시점을 `mapPage()`
  진입 전체로 할지 `kGetOrCreateNextLevel()` 호출마다 개별로 할지.

## [확정, 2026-09-17, 설계자 답변 QU-BE3A7E0A] (A) 주소공간별 전용 락 채택

설계자가 세 후보 중 **(A) 주소공간(pml4Phys)별 전용 락**을 선택했다.
higher-half(여러 프로세스의 PML4가 공유하는 커널 공용 매핑 영역)를
덮는 **별도의 전역 락**이 함께 필요하다는 점(원안이 이미 지적한
(A)의 트레이드오프)은 답변에서 별도로 배제되지 않았으므로 그대로
유지 - 즉 두 종류의 락(주소공간별 + higher-half 전역)이 공존한다.

`PageFrameAllocator`의 기존 per-node 락과의 중첩 순서는 설계자
답변에 명시적으로 포함되지 않았다 - 착수 세션이 실제 구현 시점에
결정(RM-23F4B687 §4, 데드락 없는 순서를 코드 관계도/문서 주석에
남길 것). 실제 구현/락 범위 세부는 `PN-90BD044E`가 이어서 진행.

## 참고

`PN-90BD044E`(계획, 발견 경위/전체 근거) 참고.
