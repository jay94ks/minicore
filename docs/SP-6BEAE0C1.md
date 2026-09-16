# 일반 프로세스 생성 syscall(fork/exec류) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-6BEAE0C1
  status: approved
  updatedAt: 2026-09-16T12:37:59.185Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# 일반 프로세스 생성 syscall(fork/exec류) — 설계 제안

PN-543C0CE9(설계 착수), PN-6D497EB0("Channel ID를 프로세스 간에
전달하는 메커니즘 설계")를 막고 있던 선행 조건. SP-EAB162FC §6.2가
이미 지적해 둔 대로, 이 프로젝트엔 지금까지 유저 프로세스가 "임의의
다른 프로그램을 새 프로세스로 띄워 달라"고 커널에 요청하는 범용
syscall이 전혀 없다 - 지금까지 `Process`가 생성되는 유일한 경로는
`kmain.cpp`의 고정 부팅 매니페스트 스캔(`kSpawnInitProcess`/
`kSpawnServiceProcesses`, initrd 안의 "init"/"devmgr"/"fs"/"net"/
"tty"라는 정확한 이름만 인식)뿐이다.

## 1. 실측 확인한 현재 상태 - 이 설계가 반드시 다뤄야 할 전제

- `Process`/`UserThread` 인스턴스는 지금까지 **전부 정적 전역
  변수**다(`kmain.cpp`의 `gInitProcess`/`gServiceProcess[4]`) - 부팅
  시점에 정확히 한 번(또는 Resurrect §6.2로 같은 정적 인스턴스를
  재사용)만 만들어진다. **동적으로(런타임에, 임의 개수만큼) `Process`
  를 만드는 경로 자체가 지금 이 코드베이스에 전혀 없다** - 이 설계가
  다루는 syscall이 그 첫 실사용처가 된다.
- `ProcessAddressSpaceManager`/`Vma`(SP-2AAD7C8D)에 Copy-on-Write
  페이지 공유가 없다(코드 전수 확인, 2026-09-16) - 고전 `fork()`
  (주소공간 전체 복제)를 그대로 구현하면 매 호출마다 실제 물리
  페이지를 전부 복사해야 해 비용이 크다.
- 표준 파일 API(SP-2AAD7C8D §9, `Open`/`Read` syscall)가 아직
  미착수다 - "실행할 파일의 경로 문자열"을 받아 그 안에서 파일을
  열어 읽는 방식은 이 syscall이 지금 바로 채택할 수 없다.

## 2. [확정, 2026-09-16, QU-52253384 답변] 모델 - `posix_spawn` + Copy-on-Write 설계, `fork()`는 예정 사항

설계자 답변: "Posix Spawn 포함하여 Copy-on-Write 설계해두고 fork는
예정 사항으로." - 애초 권장했던 "COW 없이 posix_spawn만" 안이 아니라,
**COW 자체를 지금 설계 범위에 포함**하되 `fork()` syscall 자체(COW를
실제로 필요로 하는 유일한 소비자)는 이후로 미룬다는 뜻으로 해석한다 -
즉 이 계획의 실제 구현 범위에 **`ProcessAddressSpaceManager`/`Vma`
(SP-2AAD7C8D)의 COW 페이지 공유 메커니즘**이 새로 추가된다.

**COW 설계 스케치(초안 - 아래 §11에서 더 구체화)**:
- `Vma`/물리 페이지에 참조 카운트 추가 필요(현재 SP-2AAD7C8D의
  `Vma`는 소유자가 정확히 하나라는 전제 - `PageFrameAllocator`가
  프레임 단위 참조 카운트를 갖는지 재확인 필요).
- 페이지 테이블 엔트리를 읽기 전용(`PAGE_WRITABLE` 비트 제거)으로
  복제하고, 쓰기 폴트(#PF, `error_code`의 Write=1) 시 그 페이지만
  실제로 복사한 뒤 원래 프로세스와 새 프로세스 양쪽에 각자의 새
  프레임을 매핑하는 표준 COW 폴트 핸들러가 필요(`Paging::
  handlePageFault` 확장).
- `posix_spawn`(이 문서의 실제 syscall)은 COW를 쓰지 않는다(애초에
  기존 주소공간을 복제하지 않고 바로 새 이미지로 시작하므로) - COW는
  오직 미래의 `fork()` syscall이 쓸 인프라를 **지금 미리 준비**해
  두는 것이다. 이 문서의 실제 `SpawnProcess` syscall 구현 자체는
  여전히 §3의 `posix_spawn`류 그대로다.

## [범위 확정] `fork()` syscall 자체는 이 계획 범위 밖 - 별도 계획으로 추적

COW 인프라는 이 계획에서 준비하지만, `fork()` syscall 자체(그
인프라의 실제 소비자)는 "예정 사항"이므로 별도 계획으로 분리
추적한다 - 아래 §12 참고.

## 3. syscall 인터페이스(초안)

```
SpawnProcess(imageBuffer: void* [유저 메모리], imageSize: uint64_t,
             argv: char** [유저 메모리, NULL 종단], envp: char**
             [유저 메모리, NULL 종단]) -> pid(int64_t, 실패 시 음수)
```

- `imageBuffer`/`imageSize` - **이미 메모리에 있는 ELF 이미지 원본
  바이트**를 그대로 넘긴다(경로 문자열이 아니다 - §9 표준 파일 API가
  아직 없으므로). 호출부가 미리 어떤 방법으로든(§9 이후엔 `Open`+
  `Read`로, 지금 당장은 예를 들어 initrd에서 직접 읽거나 다른
  프로세스가 Channel로 전달하는 등) 이 바이트를 유저 메모리에
  올려 뒀다는 전제.
- **[확정, 2026-09-16, QU-52253384 답변 + QU-51BA3736 답변]** "유저
  랜드에서 들어오는 포인터나 인자들은 기본적으로 untrusted. 복사를
  최소화하는 경로를 생각하면 syscall을 별도로 둬야 해." - 이
  syscall이 받는 유저 포인터(`imageBuffer`/`argv`/`envp`)는 **커널이
  그대로 역참조하면 안 된다**(기본적으로 신뢰하지 않음 - 악의적이거나
  잘못된 프로세스가 임의 커널/타 프로세스 메모리를 가리키는 포인터를
  넘길 수 있다는 전제). 최초엔 이를 위해 별도의 `PrepareSpawnRequest`
  류 2단계 제출 syscall을 제안했으나, **QU-51BA3736 답변("아니야
  그냥 일반 syscall로 해도 되겠네", 2026-09-16)으로 그 2단계 분할은
  폐기됐다** - `SpawnProcess` 단일 syscall 하나가 그대로 유저 포인터
  (`imageBuffer`/`argv`/`envp`)를 받되, **그 syscall 핸들러 내부에서**
  각 유저 주소 범위를 검증하고 필요한 만큼 커널 버퍼로 복사한 뒤에만
  역참조한다(별도 handle/prepare 단계 없음, 검증+복사는 이 syscall
  한 번의 처리 안에서 끝난다) - 이러면 이미지 바이트가 크더라도
  여러 syscall에 걸쳐 다시 복사하는 일 없이, 이 syscall 하나의 호출
  경로 안에서 "검증하며 한 번만 복사"가 이뤄진다.
- `RM-48E1E610` 번호 예약: **59 = SpawnProcess**(다음 미사용 번호,
  이 문서 승인과 함께 표에 등록).

## 4. 인자/환경변수 전달 규약(초안)

System V AMD64 ABI 관례(`argc`/`argv`/`envp`를 유저 스택 최상단에
배치, `_start`가 그 레이아웃을 그대로 기대)를 그대로 따른다 -
`Process::execImage()`가 지금은 이런 개념 없이 고정 진입점만
준비하므로, 이 스택 프레임 구성 로직을 새로 추가해야 한다. 이
레이아웃이 확정되면 PN-6D497EB0("Channel ID를 프로세스 간에
전달하는 메커니즘")의 "프로세스 생성 시 인자로 전달" 경로(예:
`argv`의 특정 위치에 정수 Channel ID를 실어 보내는 관례)도 이
규약의 특수 사례로 자연스럽게 커버된다.

## 5. 동적 Process 풀 - 반드시 새로 만들어야 하는 인프라

§1이 확인한 대로 `Process`를 런타임에 동적으로 만드는 경로가
지금 전혀 없다 - 이 syscall이 그 첫 소비자다. `GenericSlabAllocator`
로 `Process`/`UserThread` 인스턴스를 힙에 만들고, 그 수명을 누가/
언제 반납하는지(예: `AsyncTask`처럼 완료 시 자동 반납 vs 부모가
명시적으로 소비)를 확정해야 한다. **부모-자식 추적(§6)이 "없음"으로
확정되면 이 반납 시점 결정이 더 간단해진다**(자기 종료 시 스스로
반납) - 그래서 §6을 먼저 확정하는 게 순서상 자연스럽다.

## 6. [확정, 2026-09-16, QU-52253384 답변] 부모-자식 관계 - 프로세스 트리 구조 필수

설계자 답변: "부모 자식 추적을 위해서 프로세스 트리 구조로 설계해. -
fire and forget 하면 안돼. -- 좀비 프로세스가 쌓이는걸 QA 계획을
세워." - 원래 권장했던 "v1은 fire-and-forget" 안은 폐기됐다. 확정
방향:

- **프로세스 트리**: 모든 `Process`가 자신을 생성한 부모를 가리키는
  포인터/참조를 갖고, 부모는 자식 목록을 갖는다(정확한 자료구조 -
  배열/연결리스트/`ChunkedList<Process*>` 등은 §11에서 구체화). 최초
  프로세스(init, `kSpawnInitProcess`)는 부모가 없는 루트 - 이 트리의
  루트로 취급.
- **좀비 프로세스**: `wait()`류로 부모가 명시적으로 회수(reap)하기
  전까지, 종료된 자식은 "좀비" 상태로 남아 프로세스 구조체 자체는
  즉시 반납되지 않는다(exit code 등 종료 정보를 부모가 나중에 읽을 수
  있게) - 표준 POSIX 관례 그대로.
- **QA 계획 필요**: "좀비 프로세스가 쌓이는" 문제(부모가 `wait()`를
  절대 안 부르거나, 부모 자신이 좀비를 회수하기 전에 먼저 죽는 경우
  - 이른바 "고아 프로세스") - 실제 QA 시나리오는 §11에서 구체화하고
  별도 계획(PN, §12 참고)으로 등록한다. 최소 다뤄야 할 케이스: (1)
  부모가 정상적으로 `wait()`해 자식을 회수, (2) 부모가 `wait()` 없이
  먼저 죽음(고아가 된 자식들을 누가 입양하는지 - 전통적으로 init
  프로세스가 입양), (3) 자식이 여러 개 있는데 일부만 회수되고
  나머지는 계속 좀비로 남는 경우.
- 이 확정으로 §5(동적 Process 풀)의 반납 시점 질문도 함께 풀린다 -
  자기 종료 시 스스로 반납하는 대신, 부모가 `wait()`로 회수하는
  시점에 반납(§7-A의 기존 `ProcessStartFlags::essential`/Resurrect
  경로와는 별개 - 이 syscall로 만든 프로세스는 여전히 §7 그대로
  Resurrect 적용 안 함).

## 7. `ProcessRole`/`startFlags`/Resurrect와의 상호작용

- 이 syscall로 만들어진 프로세스는 항상 `ProcessRole::Normal`
  (SP-EAB162FC §2.1 원칙 그대로 - `KernelService`는 고정 스폰
  경로로만 부여, 이 경로는 그 후보가 아니다).
  `ProcessRole::KernelService`가 이 syscall로 새 프로세스를
  만드는 것(예: devmgr이 PnP 드라이버를 이 syscall로 스폰)은 이
  문서 범위 밖 - devmgr의 그 경로(SP-9DD4F3EA §3.2)는 이미 별도
  고정 경로로 설계돼 있다.
- `ProcessStartFlags::resurrect`/`essential`은 이 syscall로 만든
  프로세스에는 **적용하지 않는다**(둘 다 기본값 `resurrect=false`
  로 고정) - Resurrect(§6.2)는 "원래 스폰 파라미터를 다시 재현"
  하는 게 전제인데, 동적으로 임의의 `imageBuffer`/`argv`를 받는
  이 경로는 그 파라미터를 다시 만들어낼 방법이 없다(SP-EAB162FC
  §6.2가 이미 이 결론을 명시해 둠 - 이 설계는 그걸 그대로 따른다).

## 8. 범위 밖(v1)

- `fork()` syscall 자체(COW 인프라는 포함, §2/§12 참고).
- 경로 문자열로 실행 파일을 지정하는 것(§9 표준 파일 API 이후).
- `ProcessRole::KernelService`를 이 syscall로 부여하는 것(§7 참고).
- 멀티스레드 프로세스의 `wait()`(스레드별이 아니라 프로세스 전체
  종료 기준 - v1은 프로세스당 스레드 하나 전제와 일치).

## 9. 검증 계획(초안, §6/§11 확정 반영해 갱신 필요)

1. 손으로 만든 최소 ELF64 두 벌(서로 다른 진입점 동작)을 이미 실행
   중인 첫 프로세스(init)의 유저 메모리에 올려 두고, 그 프로세스가
   `SpawnProcess`를 호출해 각각 새 프로세스로 뜨는지 확인.
2. `argv`/`envp`가 System V 관례대로 새 프로세스의 유저 스택에
   정확히 올라가는지(새 프로세스가 그걸 읽어 값을 되돌려주는 방식으로)
   확인.
3. 동시에 여러 개(3개 이상)를 스폰해도 각자 독립된 주소공간/PID로
   정상 동작하는지(동적 Process 풀의 기본 동작 검증).
4. **[추가, §6 확정 반영]** 부모가 `wait()`로 자식을 정상 회수하는지,
   좀비 상태가 정확히 관측되는지, 고아 프로세스가 init에게 정상
   입양되는지(§6 QA 케이스 1/2/3 그대로).
5. **[추가, §3 확정 반영]** 유효하지 않은/커널 영역을 가리키는
   포인터를 `imageBuffer`/`argv`/`envp`로 넘겼을 때 커널이 안전하게
   거부하는지(untrusted 검증 경로).
6. QEMU 4개 표준 시나리오 무회귀.

## 10. 질의 이력 (해소됨)

QU-52253384로 등록했던 3가지(모델/부모-자식 추적/유저 포인터
정책)와 후속 QU-51BA3736(syscall 분할 형태) 전부 답변 완료 -
§2/§3/§6/§11-2에 반영. §11에 남은 나머지 세부(COW 참조 카운트
배치, 프로세스 트리 자료구조, wait() ABI)는 전부 순수 구현
세부로 판단해 착수하며 정한다 - 더 이상 설계자 확인이 필요한
미결 질문이 없으므로 이 문서를 `approved`로 전환 요청한다.

## 11. [신규, 2026-09-16] 구체화가 더 필요한 세부 - 착수 전 확정 필요

QU-52253384 답변이 큰 방향(§2/§3/§6)은 확정했지만, 실제 구현에
들어가려면 아래도 구체적으로 정해야 한다 - 설계 위험이 있는 항목은
별도 확인을 구하고, 순수 구현 세부(RM-23F4B687 §4 기준)는 착수하며
정한다:

1. **프로세스 트리 자료구조**(§6) - 부모의 자식 목록을 어떤 컨테이너로
   둘지(`ChunkedList<Process*>`가 이 프로젝트의 표준 패턴과 가장
   맞아 보임 - AsyncTaskGroup 등이 이미 재사용). 순수 구현 세부로
   판단, 별도 확인 불필요.
2. **[해소, 2026-09-16, QU-51BA3736 답변]** syscall 분할 형태(§3) -
   설계자 답변("아니야 그냥 일반 syscall로 해도 되겠네")으로 별도
   준비 syscall/핸들 없이 `SpawnProcess` 단일 syscall로 확정 -
   RM-48E1E610 번호도 기존 예약(59)만으로 충분, 추가 예약 불필요.
3. **[구현 완료, 2026-09-16]** COW 참조 카운트 배치(§2) -
   `page_frame_allocator.cpp` 전수 확인 결과 **프레임 단위 메타데이터
   자체가 지금 전혀 없다** - 이 buddy 할당자는 각 노드가 `freeListHeads
   [kMaxOrder+1]`(물리주소 기반 intrusive free list)만 갖고, "할당된"
   프레임에는 아무 사이드밴드 정보도 없다(free 상태일 때만 그 페이지
   자신의 메모리에 임시로 `FreeBlock::next`를 써 두는 것뿐 - 할당되면
   그 공간은 온전히 호출자 것).

   **[정정]** 최초 이 절에 "관리 범위가 1GiB 고정이라 262144 프레임
   고정 배열이면 충분하다"고 적었으나 이는 `page_frame_allocator.h`의
   **오래된(stale) 문서 주석**만 보고 판단한 오류였다 - 실제로는
   `paging.cpp`의 `Paging::init()`이 실측 물리 메모리 크기에 맞춰
   `kMinDirectMapGib(4)`~`kMaxDirectMapGib(512)` 사이에서 동적으로
   direct map 범위(`gDirectMapLimit`)를 정한다(부팅마다/머신마다
   다름). 고정 262144 배열은 4GiB 미만 머신에서도 이미 부족해 잘못된
   설계였다.

   **실제 구현**: 정적 배열 대신, `PageFrameAllocator::init()`에서
   `Paging::directMapLimit()`(이 시점엔 이미 `Paging::init()`이 확정해
   둠) 기준으로 프레임 개수를 실측해 커널 이미지 바로 뒤 물리 공간에
   `uint16_t` 배열을 bump 방식으로 예약(`kSubtractReservedFromList`로
   그 범위를 usable range에서 제외 - 기존 kernelPhysStart/End 제외와
   같은 패턴)하고, direct map이 이미 그 범위 전체를 매핑해 둔 상태라
   별도 매핑 없이 `kPhysToVirt`로 바로 접근한다. `retain(physAddr)`/
   `refCount(physAddr)` 두 정적 메서드를 추가했고, `freeOrder`는
   order 0에서만 이 배열을 확인해 카운트가 0이 아니면 감소만 하다가
   0이 되는 순간에만 실제 free 경로로 넘어간다 - `retain()`을 한 번도
   안 부른 페이지는 항상 0이라 **기존 모든 호출부(retain을 안 쓰는
   전부)는 완전히 동일하게 동작**한다(관계도 기록: 커밋 예정, 파일
   `minicore/kernel/page_frame_allocator.{h,cpp}`).

   **검증**: 클린 빌드 통과. QEMU 3개 시나리오(PVH 단일코어 무initrd/
   PVH+initrd/GRUB multiboot2 SMP4) 전부 `page frame allocator ready`
   이후 정상 부팅 확인, 회귀 없음 - synthetic 4-service initrd는 그
   전용 아티팩트가 없어 이번 변경(PageFrameAllocator 내부 전용이라
   프로세스 스폰 경로와 무관)에서는 생략.
4. **`wait()` syscall ABI** - 반환값(exit code만? 종료 사유
   포함?), 여러 자식 중 아무나 기다리는 것도 지원할지(`waitpid(-1,
   ...)` 같은 것) - 순수 구현 세부, 착수하며 정한다.

## 12. 관련/후속 계획

- **`fork()` syscall 자체**(§2/§8) - COW 인프라가 이 계획에서
  준비되면, 그 위에 얹는 실제 `fork()` syscall은 별도 계획으로
  등록 예정(아직 미등록 - 이 문서 approved 전환 시점에 함께 등록).
- **좀비/고아 프로세스 QA 계획**(§6) - 별도 계획으로 등록 예정(이
  문서 approved 전환 시점에 함께 등록).
- SP-9A6D579F(프로세스 디버깅) §3.1이 이 프로세스 트리 구조에
  의존한다(minicore-f8 세션 확인, 2026-09-16) - 자료구조가
  확정되면 그쪽 문서와 상호 참조를 맞춘다.

## 13. 질의 이력 (전체 해소됨)

QU-52253384/QU-51BA3736 모두 답변 완료 - 더 이상 열려 있는 확인
요청이 없다. §11에 남은 항목(1/3/4)은 순수 구현 세부로 착수하며
정한다.

