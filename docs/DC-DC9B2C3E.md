# ext4 inode 테이블 블록 read-modify-write 경쟁(고아 블록) - 락 세분화 단위 결정 요청

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-DC9B2C3E
  status: approved
  updatedAt: 2026-09-26T04:21:13.790Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

`PN-CA92C4A7`(`PN-6D2C8836` 재검증 중 발견)가 실측으로 확인한 문제:
`gBlockBitmapAllocMutex`(`SP-33FE698A`, `AsyncCoroMutex`)로 블록
비트맵 경쟁(`PN-ADA46BF4`)을 해소한 뒤에도, 서로 다른 파일의 동시
`Write`가 **같은 inode-table 블록을 공유**하면(ext4가 여러 inode를
한 블록에 packing) 그 블록을 통째로 읽어 자기 inode만 갱신 후
통째로 다시 쓰는 read-modify-write 패턴이 lost-update를 일으켜
"고아 블록"(비트맵엔 사용 중, 어떤 inode도 미참조)이 생긴다 -
단일 writer 대조군은 무결함이라 순수 동시성 문제. 재현 절차/실측
로그는 `PN-CA92C4A7` 본문 참고.

## 코드 조사 결과 - 임계구역이 매우 크다

영향받는 4개 op(`Write`/`Mkdir`/`Rmdir`/`Unlink`)의 inode-table
read→modify→write 왕복은 각 op의 **onExec 코루틴 거의 전체**를
차지한다 - 예를 들어 `Write`는 자기 inode를 읽는 시점(inode 레코드
전체를 읽어 `inodeBlockBuf`에 보관)부터 블록 할당 루프(이미
`gBlockBitmapAllocMutex`로 보호됨, 여러 `co_await` 포함)와 쿼터
갱신(`gQuotaCurspaceMutex`)을 거쳐 마지막에 그 inode를 다시 쓰는
시점까지 **~1000줄, 수십 개의 `co_await`**를 포함한다. `Mkdir`/
`Unlink`/`Rmdir`도 정도는 다르지만 비슷하게 크다.

## 결정이 필요한 이유 - 락 세분화 단위

`AsyncCoroMutex` 자체는(재시도/폴링이 없어) 임계구역 안에 `co_await`
가 몇 개든 안전하다는 게 이미 실측으로 확인됐다(`gBlockBitmapAllocMutex`
가 4-`co_await` 임계구역에서 검증됨) - 그러니 "만들 수 있는가"는
문제가 아니다. 문제는 **락 하나의 범위(scope)를 얼마나 넓게/좁게
잡을지**가 이 커널의 핵심 목표(SMP 다중 코어 동시성)에 직접
영향을 준다는 점이다:

- **(a) 전역 하나(`gInodeTableMutex`)로 Write/Mkdir/Rmdir/Unlink
  전체를 완전히 직렬화.** 구현이 가장 단순하고 락 순서 문제가
  전혀 없다(항상 이 락 하나만 최상위에서 쥔 채 그 안에서
  `gBlockBitmapAllocMutex`/`gQuotaCurspaceMutex`를 중첩 획득 -
  역방향 중첩이 코드베이스 어디에도 없으므로 데드락 불가능).
  **대가**: 서로 완전히 무관한 두 파일에 대한 동시 쓰기까지도
  이 락 하나 때문에 사실상 한 번에 하나씩만 처리된다 - "SMP
  다중 코어, 여러 프로세스가 동시에 쓰는 범용 OS"라는 이 커널의
  목표(`PN-ADA46BF4` 본문이 명시한 바로 그 목표)와 정면으로
  부딪힐 정도의 성능 희생.
- **(b) inode-table 블록 단위로 세분화**(예: 블록 번호를 키로 하는
  락 테이블/해시맵 - 서로 다른 블록을 건드리는 연산은 병렬 가능).
  이 커널의 목표에 맞지만 구현이 훨씬 복잡하다: 동적 락 테이블
  자료구조 신설(성장/축소, 참조 카운팅 또는 영구 고정 배열),
  "이 그룹의 모든 inode-table 블록마다 락 하나씩 정적 배열로
  미리 만든다"처럼 더 단순화할 여지도 있지만 그룹 수×블록 수만큼
  메모리를 미리 잡아야 한다.
- **(c) 그 외** - 예: inode 번호 자체를 락 키로 쓰되 해시로 N개
  버킷에 매핑(false sharing 있지만 구현은 단순), 또는 처음엔
  (a)로 시작해 실측으로 병목이 확인되면 그때 (b)/(c)로 전환.

## 질의

이 4개 op의 inode-table 보호를 어느 세분화 단위로 시작할까요?
`PN-CA92C4A7`이 후속 조사로 남겨 둔 항목이라 임의로 정하지 않고
여쭤봅니다.

## 참고
- `PN-CA92C4A7` - 이 결정을 요청한 계획, 재현 기록 전체.
- `PN-ADA46BF4`/`PN-6D2C8836` - 직접 선례(블록 비트맵, 전역 하나).
- `SP-33FE698A` - `AsyncCoroMutex` 설계(재사용 대상 프리미티브 자체는 이미 승인됨 - 이번 결정은 그 적용 범위만).
- `minicore/libs/libext4/ext4_driver.cpp` - Write(795)/Mkdir(2290)/Rmdir(3720)/Unlink(4535) 케이스.
