# Minicore 설계공백 검수

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: RM-F2DAFF66
  status: review
  updatedAt: 2026-09-17T03:43:11.994Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# Minicore 설계공백 검수

설계자 지시(2026-09-17, 메시지) - "NUMA 관련 문서 찾아보고 반영된게
있는지 없는지 파악해" → "응 갭을 다 매꿔야해" → "계속 더 넓혀서
봐야하고, 전부 모아서 이 문서로 만들어놔. 루프에도 명시해서 다
찾아낼 수 있도록해." RM-7C249618/RM-C65F7760/RM-48E1E610/RM-B5764185/
RM-28225668와 같은 성격의 **현황판 문서** - 다만 저 문서들이 "무엇이
구현됐는지"를 추적한다면, 이 문서는 **"설계 문서가 확정했다고 적어
둔 것 중 실제 코드에 반영되지 않은 것"**을 추적한다.

## 왜 이 문서가 필요한가 - 발견 경위

`PL-2D3184BC`(스케줄러 설계)가 `Task` PCB가 가져야 할 필드 목록에
"소속 NUMA 노드"를 명시했는데, 실제 `task.h`에는 그 필드가 없었다
(minicore-f8 세션이 설계자 지시로 NUMA 문서를 찾다가 발견,
2026-09-17). 더 나쁜 건 그 설계의 출처 문서(`DC-8EA1E7F6`)가 "이
결정은 PL-2D3184BC에 전부 반영돼 실제 구현·검증까지 끝났다"고
**틀린 단언**을 archived 상태로 남기고 있었다는 것 - 이 프로젝트의
표준 관례(RM-* 현황판, CLAUDE.md 규칙 7-13)가 "무엇을 했는지"는
꼼꼼히 추적하지만, "설계 문서가 확정한 것 중 빠진 게 없는지"를
역방향으로 감사하는 절차는 없었다.

## 방법론 - 어디를 봐야 하는가

**결론(2026-09-17 기준 조사 경험)**: 이 프로젝트의 RM-* 현황판
(라이브러리/VFS/Syscall/Signal/인터럽트 벡터 목록)은 두 세션이
매 틱 갱신하며 실측과 맞춰 왔기 때문에 신뢰도가 높다 - 표 형태로
"번호/이름/상태"가 명시적으로 박혀 있어 빠뜨리기 어렵다. **위험
지대는 오히려 큰 SP/DC 문서 안의 산문(prose) 형태 "확정된 설계"
절이다** - 특히 여러 항목을 한 번에 나열하는 목록(예: "PCB가 가질
필드 후보: A, B, C, ..., Z") 안에서 마지막 몇 개 항목이 실제
구현 단계에서 조용히 누락되는 패턴이 두 번 확인됐다(§1 참고).

**점검 절차(반복 가능한 방법)**:
1. approved/archived 상태인 SP/DC 문서 중 "확정된 설계"/"결정"
   절이 여러 필드·단계·API를 **목록 형태로 나열**하는 문서를 고른다.
2. 그 목록의 항목 하나하나를 실제 소스(`docs git grep`/`git read`)
   와 대조한다 - 특히 목록 뒷부분(앞부분은 먼저 구현되고 검증되는
   경향이 있어 상대적으로 안전, 뒷부분이 누락 위험이 높다).
3. 문서가 "전부 반영/완료됐다"고 자체 선언한 문장을 발견하면 **그
   선언 자체를 의심하고 재검증**한다(DC-8EA1E7F6의 사례).
4. 실제로 빠진 게 확인되면: 코드 갭이면 PN 계획을 등록(CLAUDE.md
   규칙 7)하고 원본 문서에 정정 각주(RM-23F4B687 §4 관례 - 원문은
   보존, 정정만 추가), 설계 자체가 후속 결정으로 바뀌어 원문이
   단순히 낡은 것뿐이면(코드 갭 아님) 문서 정정만 하고 이 문서에
   "문서만 정정"으로 기록.

## §1. 확정된 발견 (완료)

### 1-A. `Task::numaNode` 필드 누락 (코드 갭, 완전 해소)

- **출처**: `PL-2D3184BC`(스케줄러 실행 계획, `DC-8EA1E7F6` 확정
  설계 반영) "Task 자료구조" 절의 PCB 필드 후보 목록 - "소속 NUMA
  노드"가 마지막 항목으로 나열돼 있었으나 실제 `task.h`엔 없었음.
- **조치**: **PN-A74871F2**(필드 반영, scheduled) 등록 -
  `Acpi::cpuNumaNode()`로 생성 코어의 노드를 기록. 이 필드를
  Push/Pull 로드밸런싱이 실제로 참고해야 하는지는 `SP-9525C4C0`에
  `QU-759C9C1C`로 확인 → 설계자 답변 **(A) 참고함**(같은 노드 우선
  이관) → `SP-9525C4C0` §2.3에 설계 반영 완료, **PN-9DDFB774**
  (Push/Pull 소비 로직, PN-A74871F2에 plan_depend)로 별도 추적.
- **부수 정정**: `DC-8EA1E7F6`(archived)의 "전부 반영·검증 끝났다"
  단언에 정정 각주 추가. `RM-32D06563`에 `Task::numaNode` 용어 등록.
- **[완전 해소, 2026-09-17, minicore-88 세션]** PN-A74871F2
  완료(commit 7347c47) - `Task::numaNode` 필드 반영 및 QEMU 단일/
  2노드 토폴로지 검증 완료(단, 부팅 시점 Task가 전부 BSP에서
  생성돼 `numaNode!=0` 실제 경로는 아직 미실측 - PN-A74871F2 본문
  참고).
- **[체인 완전 완료, 2026-09-17, minicore-88 세션]** PN-9DDFB774도
  완료(commit 9057d08) - `kFindLeastLoadedCoreNumaAware`/
  `kFindMostLoadedCoreNumaAware`로 Push/Pull이 실제로 같은 NUMA
  노드를 우선하도록 배선, 단일/2노드 토폴로지 무회귀 확인. 이걸로
  NUMA 갭 체인(발견→PN-A74871F2→PN-9DDFB774) 전부 완료 -
  1-A는 이제 완전히 닫힌 항목이다.

### 1-B. `SP-1FBC0EEB` Channel IPC `onCancel` 미구현 - 댕글링 포인터
위험 (코드 갭, **[완전 해소, 2026-09-17, minicore-88 세션, commit
5336b50/0d80959] 이 문서 최초의 가장 심각한 발견이었으나 지금은 해결됨**)

- **출처**: `SP-1FBC0EEB` "취소/실패 처리와 Syscall 제안의 연동" 절이
  "`connectChannel` 취소 시 그 `Channel`의 대기열에서 자신의
  `PendingConnectRequest`를 제거"해야 한다고 명시.
- **실제**: `channel.cpp`의 7개 `AsyncTaskHandler` 전부
  `onCancel(AsyncTask*, void*) override {}` - 예외 없이 완전한
  no-op. `ConnectChannelHandler::onExec()`이 코루틴 **로컬 변수**
  `PendingConnectRequest req`를 `channel->pendingConnects`에 매달아
  둔 채 대기하는데, 대기 중 제출자가 죽으면(`PN-40E976F2`가 실제로
  구현한 취소 경로) `onExec()`을 재개하지 않고 `onCancel()`만
  호출한 뒤 그 AsyncTask(코루틴 스택 포함)를 반납한다 - 빈
  `onCancel()`이라 `&req`가 제거되지 않고 **댕글링 포인터**로
  `channel->pendingConnects`에 남는다(다음 `acceptFromChannel`이
  이를 꺼내 역참조하면 UAF). `AcceptFromChannelHandler`도 자기 자신의
  `AsyncTask*`를 `channel->pendingAccepters`에 매달아 두는 구조라
  마찬가지 위험 - 설계 문서의 "acceptFromChannel 취소는 정리
  불필요" 전제가 실제 자료구조와 안 맞는 것으로 보인다.
- **조치**: **PN-C4611402**(completed, commit 5336b50) - 4개 핸들러
  (`ConnectChannelHandler`/`AcceptFromChannelHandler`/
  `ChannelReadHandler`/`ChannelWriteHandler`)의 `onCancel`에 실제
  큐 제거 로직 구현(`AsyncTaskWaitQueue::remove()`/`Channel::
  removePendingConnect()` 신설). `OpenChannelHandler`/
  `CloseBridgeHandler`/`DestroyChannelHandler` 3개는 onExec에
  yield 지점이 없어 no-op 유지가 맞음을 코드로 확인, 근거를 주석으로
  남김. `SP-1FBC0EEB`의 "acceptFromChannel 취소는 정리 불필요"
  서술이 실제로 틀렸음이 확인돼 정정 각주 추가(commit 0d80959) -
  read/write 쪽도 같은 모양의 미서술 갭(`pendingReaders`/
  `pendingWriters`)이 추가로 발견/수정됨.
- **현재 상태(2026-09-17)**: **완전 해소.** QEMU 회귀(무-initrd
  단일코어 + devmgr 포함 SMP4, 실제 Pnp/Channel IPC 정상 경로 포함)
  무회귀 확인. **단, 실제 취소 레이스(대기 중인 제출자를 다른
  스레드가 강제 종료) 자체는 재현 못 함** - 이 코드베이스에 아직
  "임의의 다른 스레드를 강제 종료"시키는 수단이 없어(SelfTerminate는
  자기 자신만, Signal 기반 취소는 PN-71E50394 미연동) 코드 검토로
  대신 검증했다 - `PN-71E50394` 완료 후 재검증 가치 있음(§3에
  후속 항목으로 등록).

### 1-C. `SP-8B6B8D25` §2-B(유저 페이지 폴트 정책) 문서 정체 (문서만 정정 - 코드 갭 아님)

- **출처**: `SP-8B6B8D25` §2-B가 "유저 폴트 시 그 프로세스만
  블로킹시키고, 폴트 정보(주소/에러코드/명령어)를 PCB에 저장해
  나중에 '진짜 오류인지 SWAP 필요인지' 판단"하는 정책을 명시.
- **실제**: `PN-71E50394`(Signal 전달 인프라)가 `QU-04C420BF` 답변
  ("그래 이렇게 해")으로 확정한 최종 정책은 **훨씬 단순**하다 -
  SWAP 자체가 없어(`RM-7C249618` "libswapfs 예정, 미구현") 그
  판단 대상이 없으므로, 온디맨드 매핑/COW로도 못 고친 ring3 폴트는
  `raiseSignal(Segv)` 후 `kTerminateFaultingUserTask()`로 **그
  즉시 종료**(블로킹 후 유예가 아님, 폴트 정보를 PCB에 별도
  보관하지도 않음).
- **조치**: `SP-8B6B8D25` §2-B에 정정 각주 추가(원문 보존) - 이건
  실제로 "설계가 나중에 더 단순한 방향으로 바뀌었는데 원본 문서가
  그 사실을 반영 못 한" 사례로, **코드가 잘못된 게 아니라 문서가
  낡았던 것** - 코드 쪽 조치 불필요.
- **[추가 발견, 2026-09-17]** 같은 낡은 정책 설명이 **두 번째
  문서에도 그대로 복제**돼 있었다 - `SP-68182FBD`(프로세스 모델)
  §2.3이 "폴트 정보를 `pendingSyscalls` 토큰으로 모델링해 비동기로
  나중에 판단"이라는 같은 옛 정책을 자기 언어로 다시 서술해 둔 채였다
  (원본 §2-B 정정과 별개로 놓쳤던 사본). `idt.cpp` 580/645/651행
  재확인(`kTerminateFaultingUserTask`가 즉시 종료, 반환 없음)으로
  같은 정정을 이 문서에도 적용 완료 - "하나를 고쳤다고 다른 문서의
  복제본까지 자동으로 고쳐지지 않는다"는 점을 상기하는 사례로 기록.
- **현재 상태**: 완전 해소(양쪽 문서 모두).

### 1-D. `RM-28225668`(인터럽트 벡터 목록) `0xE2` 항목 - 역방향 문서
정체 (문서만 정정 - 코드 갭 아님, 오히려 코드가 문서보다 앞서 있었음)

- **출처**: `RM-28225668`(신뢰도 높다고 이 감사 문서 §"방법론"이
  스스로 전제한 표 형태 현황판) `0xE2`(`kForcedMigrationVector`)
  항목이 "설계 확정(번호 배정), 구현 대기"로 표시돼 있었음
  (`SP-ECC59BAE` §3.1 대상).
- **실제**: `scheduler.cpp`에 `requestForcedMigration()`/
  `kForcedMigrationIsr`/`ForcedMigrationRequest`가 이미 구현돼
  있고, `idt.cpp`에 `kForcedMigrationVector` 디스패치도 등록돼
  있음을 grep으로 확인 - **실제로는 구현이 완료돼 있었는데 표만
  갱신이 안 된 것**. `RM-32D06563`(용어 문서)는 같은 항목을 "설계
  확정, 구현 계획 등록 완료"로만 적어 뒀을 뿐 완료 여부를 명시하지
  않아 그쪽도 모호했다.
- **의미**: 이 감사 문서의 방법론(§"방법론")은 "표 형태 현황판은
  신뢰도가 높고, 산문형 SP/DC 문서가 위험 지대"라고 전제해 왔다 -
  이번 발견은 표 형태 현황판도 갱신이 밀릴 수 있다는 첫 반례다.
  다만 방향이 반대(코드가 문서보다 앞섬)라 실제 위험(코드 미반영)은
  아니었다.
- **조치**: `RM-28225668` `0xE2` 항목에 정정 각주 추가(원문 보존).
  코드 쪽 조치 불필요.
- **현재 상태**: 완전 해소.

## §2. 점검 완료 - 갭 없음 확인

- **`TaskClass::RealTime` 우선 스케줄링**: `task.h` 주석이 "구현
  예정"이라고 남아 있어 의심했으나, `Scheduler::pickNext()`가 실제로
  Immediate→RT→Normal 순서로 큐를 비우는 것을 `scheduler.cpp`에서
  확인 - 정상 구현됨(코드 주석만 안 지워진 사소한 흔적, 별도 조치
  불필요).
- **`SP-68182FBD`(Process/AddressSpace 설계)**: 열린 항목마다
  "해결됨" 표시와 참조 문서가 일치 - 갭 없음.
- **`RM-48E1E610`(Syscall 할당표)**: 두 세션이 매 틱 갱신하며 실측과
  맞춰 옴 - 표 형태라 신뢰도 높음. 이번 조사에서 불일치 없음.
- **`RM-B5764185`(Signal 번호표)**: v1에서 실제로 발생 가능한 4개
  신호(Kill/Term/Segv/Ill)와 `PN-71E50394` 구현이 일치 - 갭 없음.
- **`RM-C65F7760`(VFS 구조)**: "구현된 경로" 표가 이미 "아직 실측
  검증 안 됨" 항목(kernel/<name> 인증 성공 분기)을 스스로 정확히
  인지하고 있음 - 숨겨진 갭 아니라 이미 추적 중인 미검증 항목.
- **`RM-28225668`(인터럽트 벡터 목록)**: 0xE0-0xE3 전부 상태 최신,
  `PN-B3DD3D19`(인터럽트 구독)가 새 고정 IPI 벡터를 요구하지 않고
  기존 동적 벡터 위임 메커니즘을 재사용함을 확인 - 갭 없음.
- **`PN-18FDBFF3`(Channel ownerProcess → DontDeref&lt;Process&gt; 승격)**:
  minicore-88 완료 보고(commit f454faf) - `shared_ptr.h:538`의
  `DontDeref<T>`(534행 주석에 "operator*/operator->/T* 변환 전혀
  없음" 명시, g++ -fsyntax-only로 operator-> 실제 컴파일 에러까지
  확인했다고 보고) + `channel.h:293/324`의 타입 교체 전부 코드로
  직접 확인. `RM-32D06563`에도 `DontDeref<T>`/세대 태그 슬롯 테이블
  공용 패턴 둘 다 신규 등록됨(RM-F2DAFF66 방법론이 요구하는 "새
  개념은 RM-32D06563에" 규칙 8/12를 스스로 챙긴 사례).
- **[좋은 사전 포착 사례]** minicore-88이 `PN-E82744B1`(Mutex/
  Semaphore syscall) 착수 전 재검토 중 `SP-0666DB3C` §17.2가
  Channel과 똑같이 "핸들=포인터값" 관례를 그대로 물려받고 있었음을
  스스로 발견 - `PN-CE6A04AB`로 그 관례 자체가 보안 취약점이었던
  걸 이미 아는 상태였기에, 구현 착수 전에 §17.2 정정 각주 + 착수
  조건에 세대 태그 테이블 패턴 필수화를 미리 걸어 뒀다(§17.2/
  `PN-E82744B1` 코드 대조로 확인). RM-F2DAFF66이 추적하는 "한 문서의
  낡은 설명이 다른 문서에도 복제돼 있을 수 있다"(§5 방법론)는 것과
  정확히 같은 패턴을 이 세션 밖에서도 스스로 잡아낸 사례 - 별도
  조치 불필요, 기록만.
- **`PN-CE6A04AB`/`SP-CA3C3E57`(Channel 보안 취약점)**: minicore-88이
  구현 완료(commit 74f0f75) 보고, 이번 틱에 코드 독립 확인 -
  `channel.h:293`(`ownerProcess`), `channel.cpp`의 `kResolveChannelId`
  (:166)/세 호출부 교체(:403/:472/:501/:612/:835)/소유자 검증
  (:510-512, :843-845) 전부 실측 확인, `gChannelTable[65536]` 크기도
  §2 확정값과 일치. RM-F2DAFF66이 추적하던 항목 중 실제로 완전히
  닫힌 사례(PN-C4611402/NUMA와 같은 급) - 잔여 항목은 §6-A
  `DontDeref<T>` 타입 승격(`PN-18FDBFF3`)/SharedPtr 마이그레이션
  (`PN-260D7D73`) 둘 다 별도 계획으로 openly 추적 중이라 갭 아님.
- **`SP-9CB55C5B`(Kill 안전한 ProcessId 해석)**: `kResolveProcessId()`/
  `gProcessTable[]` 자체는 아직 코드에 없음(`process.h` grep 0건) -
  다만 이건 문서 §7이 스스로 "제안(확정 아님)"이라고 명시한 것과
  일치하고, `PN-88E62419`(Kill 임의 대상 구현, in_review)가
  `PN-C39882D0`/`PN-AA30E4C8`/`PN-617F4E52` 세 선행 계획으로 이미
  openly 추적 중이라 "조용히 빠진" 사례는 아니다(§4 예방조치 패턴과
  동일). **부수 발견**: `PN-C39882D0`(pid ABI 마이그레이션 승인)의
  자체 선행 조건("SP-9CB55C5B approved + QU-78E4159E 해소")이 이미
  충족돼 있었는데도 `planned` 상태로 방치돼 있었음 - 착수 전 확인
  사항(유저랜드 pid 소비자 존재 여부, `docs git grep` 결과 0건)을
  이번 틱에 완료하고 `QU-AB5247DD`로 명시적 승인 요청 등록,
  `pending_approval`로 전환(이 발견 자체는 코드 갭이 아니라 계획
  진행 누락이라 이 문서보다 일반 루프 절차 2번에 해당하지만, "설계는
  확정됐는데 후속 조치가 멈춰 있었다"는 성격이 같아 여기 기록).
- **`SP-E9B44929`(syscall 그룹+call 2단계 인코딩)**: minicore-88
  완료 보고(commit 1f9228d) 독립 검증 - `syscall.h`의
  `kMakeSyscallEndpointId`/`kSyscallGroupOf`/`kSyscallCallOf` +
  `kSyscallEndpointSelfTerminate = kMakeSyscallEndpointId(0, 0)`류
  재정의 확인, `syscall.cpp`의 `gCallSlotPool[1024]`(정적 범프 풀 -
  최초엔 `GenericSlabAllocator`로 동적 할당했다가 `SelfTerminateHandler`
  등록이 `GenericSlabAllocator::init()`보다 먼저 실행되는 부팅
  순서 때문에 페이지 폴트 패닉 - 실측으로 스스로 잡고 정적 풀로
  교체) 확인. `RM-48E1E610`도 그룹별 챕터로 실제 재구성됨(그룹 0
  Process부터 확인) - Sync 그룹(8)은 §17.2 정정 대기로 의도적으로
  번호만 예약 상태 유지, 경고 문구까지 남아 있음. 완전히 닫힌
  사례 - 갭 없음.
- **`SP-9F1DB1D8`(gCurrentTask RwSpinlock)**: `PN-D3597800`
  completed(commit 2d0da74) 주장 독립 검증 - `spinlock.h:129`에
  `RwSpinlock`/`RwSpinlockReadGuard`/`RwSpinlockWriteGuard` 확인,
  `scheduler.cpp`에 `gCurrentTaskLock[kMaxCores]` + 18곳의 가드
  적용(읽기/쓰기 전부) 실측 확인 - 구현 중 추가로 발견됐다는
  `retireCurrentTask()`/`handleFpuTrap()` 두 곳도 실제로 가드가
  걸려 있음. 갭 없음.
- **`SP-1DB13F61`(vtable 타입 placement new 예외)**: `shared_ptr.h`에
  `kMakeSharedNew<T>()`/`kDestroyCtorAndFree<T>()`(§3 제안 그대로)가
  실제로 구현돼 있고, `mutex_core.h`/`semaphore_core.h` 둘 다 예전
  raw `GenericSlabAllocator::alloc`+`memset` 패턴이 남아있지 않음을
  확인(주석이 `kMakeSharedNew<Mutex>()`/`kMakeSharedNew<Semaphore>()`
  로 만들어야 한다고 명시) - 갭 없음.
- **`SP-00CA7175`(커널 ↔ 커널 서비스 통신 채널)**: 문서 자체가 이미
  2026-09-17에 "전면 정정"/"전부 완료" 각주를 달아 두었으나(RM-F2DAFF66
  방법론 - 문서의 자체 완료 선언은 그 자체를 의심하고 재검증), 이번
  세션이 독립적으로 코드 대조: `channel.h`(281/303행)의
  `Channel::exclusivePreemptive` 필드 + `channel.cpp`(10곳)의
  `AsyncReactor::submitCompletion(..., channel->exclusivePreemptive)`
  전달로 Tier B 완전 구현 확인, `kernel_service_ring.h`(16행)에
  `KernelServiceSharedRingBuffer` 구조체(Tier A 골격) 존재 확인 -
  `PN-7AC01E6E`(completed)가 자체 기록한 "항목 7: 구조체만, 실제
  소비자 없음"과 정확히 일치. **결론**: 문서의 자체 정정이 실제로
  정확했다 - 갭 없음(남은 유일한 열린 항목인 Tier A 소비 알림
  메커니즘/버퍼 크기는 실제 소비자가 생기기 전까지 정당하게 유예된
  상태, RM-23F4B687 §4 패턴).
- **`SP-EAB162FC`(ProcessRole/Capability/Resurrect 체계)**:
  §2.1(`Process::role` 필드)/§2.3(SubscribeInterrupt exclusive
  자격 검증)/§6.1-6.4(Resurrect `essential`/`resurrect` 플래그,
  3단계 분기, 백오프 상수·공식까지 정확히 일치) 전부 `process.h`/
  `scheduler.cpp` 실제 코드와 대조 확인 - 갭 없음. §2.2(PnP 드라이버
  자식도 KernelService 부여)/§2.3의 RequestIoPermission 소비는
  그 상위 기능(PnP 드라이버 스폰, RequestIoPermission 자체)이 아직
  코드로 없어 지금은 대조 불가 - devmgr 항목4/5 착수 후 재확인 필요
  (§3에 다시 추가하지 않고 여기 각주로만 남김, 그때 가서 다시 봄).
- **`SP-2AAD7C8D`(mmap/Maple Tree) §6**: 6개 항목 중 2(TLB
  샷다운)/4(파일 백킹 mmap)/6(COW)은 명시적 ~~취소선~~/"완료"
  표시로 해소 확인됐고, 1(RCU)/3(findGap 시작 지점)/5(노드 전환
  휴리스틱)은 전부 "구현 시점에 실측하며 정한다"고 처음부터 명시적
  으로 유보된 항목(RM-23F4B687 §4 정당한 유예, 숨겨진 갭 아님) -
  전체적으로 갭 없음.
- **`SP-8B6B8D25` §2 항목9 "CPU 캐시 관리"**: 프로젝트 전체 문서
  검색(`MTRR`/`wbinvd`)에서 0건 - 이 마스터 문서가 커널 책임으로
  이름만 올려 뒀을 뿐 구체적인 하위 SP 문서 자체가 한 번도 작성된
  적이 없다(지금까지의 §1/§2 항목들과 성격이 다름 - "확정된 설계가
  코드에 빠진 것"이 아니라 "구체 설계 자체가 없는 것"). 가장 실질적인
  우려였던 MMIO 캐시 일관성은 `pnp.cpp`의 `RequestIoPermission`
  구현이 `PAGE_CACHE_DISABLE`을 이미 올바르게 설정하고 있어 실재
  버그는 아님을 확인 - **`PN-5BCA7AB9`로 낮은 우선순위 문서화
  백로그 등록, 갭 없음(급한 위험 아님)**.
- **`SP-39F18E30`(DMA 버퍼 관리자)**: `AllocDmaBuffer`/`FreeDmaBuffer`
  syscall 자체가 아직 미구현 - `RM-48E1E610`("번호만 예약")과
  `pnp.cpp` grep(구현 없음) 둘 다 일치, 표 형태 현황판이 이번엔
  정확했다. §6이 스스로 열어 뒀던 3개 항목 중 2개는 이미 해소
  기록, 나머지 하나(프로세스 종료 시 물리 프레임 반납 누수)도
  `PN-FFC2F062`로 착수 조건(DMA 버퍼 관리자 자체 착수)까지 명시해
  정확히 추적 중 - 실제 드라이버(AHCI/USB)가 아직 하나도 없어
  당장 필요하지도 않다(PN-BD9AAE2F 항목5와 같은 이유). 갭 없음.
- **`SP-29D652AA`(진짜 컴파일러 thread_local)**: §7까지 전부 확정된
  approved 설계이지만 실제 구현은 `PN-22E5E9E7`(scheduled, 미착수)
  으로 이미 정확히 등록돼 있음을 `plan_get`으로 확인 - 디스패치
  핫패스를 건드리는 위험도 때문에 의도적으로 미착수 상태(minicore-88
  세션이 이미 인지하고 보류 중) - openly 추적 중, 숨은 갭 아님.
- **`SP-C2670F69`(AHCI)**: §4 항목2("fs 서비스 설계가 아직 없음")가
  낡은 교차 참조였음을 발견 - `SP-7CC5693A`(fs/VFS)가 그 사이
  approved되며 §3.2 `FileSystemDriver::mount(BlockDevice*)`가 정확히
  `AhciBlockDevice`를 연결점으로 지목해 뒀는데 이 문서는 갱신이 안
  돼 있었다. 정정 각주 추가 - `BlockDevice` 인터페이스 세부(LBA
  read/write/TRIM 등 정확한 시그니처)는 여전히 미정이지만 코드가
  전혀 없는 순수 설계 단계 세부라 별도 PN 등록은 보류(AHCI 실착수
  시 자연히 확정될 항목, RM-23F4B687 §4 취지) - 갭 없음(정정만).
- **`SP-E35FD36C`(USB 스택)**: xHCI 위주 v1 설계 + 레거시(UHCI/OHCI/
  EHCI)/SuperSpeed 확장 초안까지 전부 명시적으로 유예 조건과 함께
  기록돼 있고, 코드가 전혀 없어(devmgr 자식 프로세스로 실행 예정,
  아직 착수 전) 코드-문서 불일치 자체가 성립하지 않는다 - 교차
  참조(SP-9DD4F3EA/SP-39F18E30)도 최신 상태와 일치. 갭 없음.
- **`SP-83A07867`(CR3 동기화 통합)**: §3.2/§8이 "모든 디스패치
  재개 지점"을 두 갈래로 완결했다고 서술하지만, 작성 시점 이후
  생긴 세 번째 재개 경로(`AsyncReactor::drainOnce()`의
  `coroHandle.resume()`, 코루틴 지원과 함께 도입됨)가 빠져 있음을
  발견 - 이미 `PN-2008220B`로 별도 추적 중인 바로 그 갭이라 새로운
  코드 갭은 아니지만("작성 시점엔 정확했던 문서가 이후 생긴 새
  경로를 못 따라간" 사례), 이 "완결됐다"고 주장하는 문서 자체에
  교차 참조가 없어 다음에 §3.2/§8을 참고하는 사람이 오도될 수
  있었다 - 교차 참조 추가(원문 미수정). §8의 하드웨어 불변조건
  체크리스트(CR3/RSP0/FPU) 자체는 명시된 세 지점 안에서는 정확히
  구현/완료돼 있음을 확인(PN-40210D5A/PN-AEA74E1B/PN-F258698E 전부
  completed) - 문서 정정만, 코드 갭 아님.
- **`SP-DE19BB1C`(커널 영역 TLB 샷다운)**: 이 문서 자체가 이미
  `PN-D132A1E9`(§5-1, 유저 영역 확장 - 요청자별 슬롯+수신자별
  Target Pending Mask) 완료를 상세히 기록해 뒀고, `tlb_shootdown.cpp`
  grep으로 `gRequests[kAcpiMaxCpus]`/`gPendingMask[kAcpiMaxCpus]`
  구조가 설계 그대로임을 재확인 - `RM-28225668`의 `0xE0` 등재도
  일치. §5-2(벡터 배정)/§5-3(최적화 기법)도 전부 명시적으로 해소/
  유예된 상태 - 갭 없음.
- **`SP-2602CAA6`(커널 이벤트 발행/구독)** §4/§6/§7/§8(커널 전용
  부분, "즉시 착수 가능"으로 명시된 범위): `event_topic.cpp`를 직접
  grep/read해 `EventTopicRegistry::registerTopic/subscribeKernel`,
  `EventPublisher::publish` 전부 스펙 그대로 구현돼 있음을 확인
  (`PN-AAE631EA`가 커버, TEMP 실측 검증까지 완료 기록). §5(유저
  노출 syscall 3종)는 §13이 명시적으로 "devmgr 실사용처 등장까지
  보류"한 것 그대로 미구현 - openly 추적 중이라 갭 아님. (참고:
  `document_get`의 `backlinks`는 문서↔문서 링크만 보여주고 plan의
  `refs`는 안 잡혀서 처음엔 "계획이 전혀 없다"고 오판할 뻔함 -
  `plan_get`으로 직접 확인해야 정확함, 다음 스윕에서 같은 실수
  주의.)
- **`SP-9DD4F3EA`(PnP 프레임워크) §3 전체**: devmgr 실코드
  (`PN-BD9AAE2F`)가 아직 착수 전이라 "코드에 이미 빠진 것"은 없음
  (§3.1-§3.4 전부 그 계획의 6단계 체크리스트로 이미 openly 추적
  중). 다만 **§3.3a("자원 소유권 및 정리")가 "공식 요구사항"으로
  명시한 `DeviceOwnerTable` ↔ `PN-71C3D483`(Process Teardown Hook)
  연동이 `PN-BD9AAE2F`의 체크리스트 항목4 문구엔 명시적으로 안
  드러나 있어 착수 시 누락될 위험**을 발견 - 착수 전에 예방적으로
  `PN-BD9AAE2F` 항목4에 그 요구사항을 명시적 하위 항목으로
  추가해뒀다(`SP-39F18E30`§3.2의 `Process::dmaBuffers` 정리 경로가
  이미 같은 패턴의 선례). "아직 안 생긴 코드가 빠짐없이 요구사항을
  반영하도록 체크리스트를 보강"한 사례 - §1(이미 발생한 갭)은 아니고
  §4(예방 조치)로 분류.
- **`SP-0666DB3C`(Mutex/Semaphore/Signal) §17/§9.6-3**: §17(유저
  syscall 노출, `MutexCreate` 등 8개는 `RM-48E1E610`에 번호만
  예약)과 §9.6-3 소비 쪽(`Task::lastCancelReason`을 실제로 읽는
  대기 프리미티브가 아직 없음)은 **"숨겨진 갭"이 아니다** -
  `PN-71E50394`/`RM-48E1E610` 자신이 이미 "아직 미구현/번호만 예약"
  이라고 공개적으로 추적 중인 항목이다. 이 문서(설계공백 검수)의
  대상은 "아무도 모르게 빠진 것"이지 "알고 있고 계획된 미착수 작업"
  이 아니므로 구분 - 이런 항목은 그냥 기존 PN 추적에 맡기고 이
  문서에는 안 옮긴다(점검은 했으나 §1 대상 아님, §2에도 안 옮기고
  여기 기록만 남김).
- **`SP-F682B889`(AsyncTask 프레임워크) §3/§7/§8/§9**: 이 문서는 이미
  스스로 대량의 "확정/정정" 각주를 누적해 온 문서라(§3.4가 리액터를
  전용 Task→인라인 idle 경로로, §7.3이 재개 드라이버를
  reactorTaskEntry→drainOnce로 뒤집는 등) 특히 위험 지대일 수 있다고
  보고 실제 `async_task.cpp`와 대조 - **최신 확정 구조
  (homeCoreIndex/allowCoreMigration, coroHandle을 drainOnce()가
  resume, 별도 reactorTaskEntry 없음)가 코드에 정확히 반영돼 있음을
  grep으로 확인**(2026-09-17). §8.6/§9.5의 "아직 열려 있는 하위
  과제"들은 전부 PL-1E247831/PN-C62F7908 구현 시점으로 명시적으로
  미뤄진 항목(RM-23F4B687 §4 정당한 유예 패턴)이라 숨은 갭 아님 -
  갭 없음.
- **`SP-677210E6`(TSS/IST) "이번 범위에 포함하지 않은 것"**: 4개
  항목(RSP0/IST5-7/NMI·MC·DB 실처리/#DF 레지스터 복구) 전부 각자
  PN 계획(PN-124C105B completed, PN-5377545F, PN-F443FE73 completed,
  PN-AD3B2D5B)으로 명시적으로 openly 추적 중 - 숨은 갭 아니다.
  PN-F443FE73을 실제로 열어 확인한 결과 NMI/#MC/#DB 커널 라우팅과
  QEMU 실측까지 전부 완료, 그 안에서 파생된 "#DB 유저 syscall
  범위" 후속 질의(QU-3DB5F85C)도 빠짐없이 SP-9A6D579F/PN-87D6B615로
  분리 등록돼 이미 이 감사 문서 §3에서 추적 중임을 재확인 - 갭 없음.

- **`SP-0666DB3C` §17(Mutex/Semaphore syscall 노출)**: §17.2 정정이
  `PN-CE6A04AB`/`SP-CA3C3E57`를 정확히 인용하며 "세대 태그 슬롯
  테이블" 패턴으로 교체 방향을 잡아 뒀고, `SP-E9B44929`(syscall
  그룹+call 인코딩)와는 다른 축(핸들 값 형식 vs 엔드포인트 번호
  형식)이라 서로 충돌 없음을 확인. `RM-48E1E610`의 Sync 그룹(8)도
  "§17.2 정정 대기"로 정확히 의도적 미등록 상태 유지 중 - 갭 없음.

## §3. 아직 점검 안 한 영역 (다음 틱 대상)

같은 방법론(§목차 나열형 "확정된 설계" 절 vs 실제 코드)을 아직
적용 안 해본 주요 SP 문서/영역 - 매 틱 1-2개씩 골라 점검하고
결과를 이 절에서 §1(발견) 또는 §2(갭 없음)로 옮긴다:

- [ ] `SP-9A6D579F`(DebugSession) - **[갱신, 2026-09-17] PN-87D6B615
  착수됨.** 항목1(자료구조)/항목2(부모-자식 권한, 일부) 구현·실왕복
  검증 완료(`debug_session.cpp`에서 `kFindDebuggableChild`/
  `submitterTask.lock()` 사용 직접 grep 재확인 - PN-5BBD4301류
  실수 없이 처음부터 올바르게 작성됨). 항목2의 KernelService 예외는
  `QU-764C5624`(Kill 스코프) 답변과 연동 대기 중임을 코드 관계도에도
  기록해 둠 - 별도 질의 중복 없이 잘 처리됨. **항목3-9(브레이크포인트/
  싱글스텝/#DB ISR/메모리 대행/이벤트 통지/멀티스레드)는 여전히
  미착수이나 계획 자신의 체크리스트로 이미 openly 추적 중** - 전부
  완료되면 그때 전체를 §목차 방법론으로 재대조.
- [ ] `SP-B1E258D8`(RCU) - `rejected`(도입 시점 보류)라 코드 갭
  대상 아님, 재개 조건(커널단 v1 완료)이 실제로 도래했는지만 주기적
  확인.
- [ ] (2026-09-17 재정정) `PN-C4611402`(§1-B, 완전 해소) 취소 로직이
  실제 취소 레이스로는 아직 검증 안 됨(코드 검토로만 확인). **직전
  갱신("PN-71E50394 완료로 착수 가능")은 틀렸다 - `PN-B5C2845A`로
  실측 확인**: `Kill`은 `Task::blockedOn`(Waitable 기반 블로킹)이나
  실행 중인 대상에만 실제로 도달하고, `acceptFromChannel`/
  `connectChannel` 등이 쓰는 `Syscall::wait()`의 순수 파킹
  (`Scheduler::parkCurrent()`, `blockedOn` 전혀 안 씀)에는 강제
  웨이크업 경로 자체가 없다 - 그 파킹을 깨우는 유일한 길은 그
  UserThread 자신이 제출한 AsyncTask가 정상 완료되는 것뿐이라,
  `Kill`로 `pendingSignals`에 기록해도 대상이 절대 깨어나지 않는다.
  **여전히 재현 불가능** - `PN-B5C2845A`(신규 등록, 해법 후보 및
  위험도 분석 포함)가 해소돼야 이 항목도 재검증 가능해진다.
- [ ] (신규, 2026-09-17) `PN-2008220B`(coroHandle.resume() CR3
  미동기화, RM-23F4B687에 원칙으로도 기록) 해소되면 그 수정이
  `SP-F682B889` §7.3(코루틴 재개 경로) 서술과 여전히 일치하는지
  재확인.

## §4. 예방 조치 (아직 코드가 없어 "갭"은 아니지만, 착수 시 누락 위험을
미리 체크리스트에 못박아 둔 것)

- **`SP-9DD4F3EA` §3.3a → `PN-BD9AAE2F` 항목4**: `DeviceOwnerTable`
  ↔ `PN-71C3D483`(Process Teardown Hook) 연동을 명시적 하위 항목으로
  추가(2026-09-17) - 자세한 내용은 §2의 해당 항목 참고.
  **[효과 확인, 2026-09-17]** minicore-88이 항목4(`RequestIoPermission`,
  commit e869786)를 구현하며 teardown hook 정식 연동을 지금 당장
  하지 않고 **`PN-7528A406`으로 명시적으로 분리 등록**했다 -
  `owner.lock()` 지연(lazy) GC로 임시 대체 중임과 "§3.3a가 공식
  요구사항으로 명시"했다는 사실까지 그 계획 본문에 그대로 인용해
  적어 뒀다. 예방 조치를 걸어 두지 않았다면 그냥 조용히 빠졌을
  가능성이 있는 항목이 **이번엔 미착수 상태로나마 openly 추적**된
  사례 - 이 문서(§4)의 목적이 실제로 작동함을 확인.

## §5. 기록 규칙

- 새로 발견한 갭은 §1에 하위 절로 추가(코드 갭이면 PN 등록 +
  plan_link, 문서만 정정이면 원본에 각주만).
- **문서 하나를 정정할 때, 같은 설명이 다른 문서에도 복제돼 있는지
  의심한다** - 이 커널의 SP 문서들은 서로 참고/인용하며 같은 설계를
  자기 말로 다시 서술하는 경우가 흔하다(예: SP-8B6B8D25 §2-B와
  SP-68182FBD §2.3이 같은 유저 페이지 폴트 정책을 각자 서술) - 하나만
  고치고 나머지를 놓치면 다음 스윕에서 같은 문제를 처음 발견한 것처럼
  또 찾게 된다. 정정할 때 `docs search`/관련 문서의 backlinks로
  같은 주제를 다루는 다른 문서가 있는지 한 번 더 확인한다. **이
  복제는 CNW 문서 사이에서만 일어나지 않는다 - 소스 코드 주석도
  같은 위험에 노출된다**(2026-09-17 사례: `chunked_list.h`의 수정
  주석이 "PN-584DB994의 근본 원인일 가능성이 높다"고 최초 가설을
  그대로 남겨 뒀는데, 정작 `PN-584DB994`/`PN-A8D235E7` 두 계획
  본문은 이후 실측 근거(8회 재현 레지스터 값 완전 동일 + 이 재현
  시나리오에선 `children`/`openBridges` insert 자체가 아예 안 불림)로
  walk-back해 "근본 원인 아닐 가능성이 높다"로 뒤집었다 - 코드
  주석만 안 따라가 같은 파일 안에서 정반대 결론이 공존했다. 문서를
  walk-back할 땐 그 근거가 된 소스 코드 주석도 같이 갱신됐는지
  확인한다.
- §2/§3 사이를 오갈 때 이 문서 자체를 `plan_set`이 아니라
  `document_patch`로 갱신(문서이지 계획이 아니므로).
- 이 문서 자체는 "완결"되는 문서가 아니다 - §3이 빌 때까지, 그리고
  그 이후로도 새 SP 문서가 승인될 때마다 계속 대상에 추가한다(루프
  표준 절차 0-3번에 편입, 별도 지시 참고).

