# Minicore 설계공백 검수

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: RM-F2DAFF66
  status: review
  updatedAt: 2026-09-24T06:40:24.275Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

﻿﻿# Minicore 설계공백 검수

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

### 1-W. `SP-0C7A4F3B`(Power 서브시스템) §1 항목5 - "ACPI 전원 버튼(SCI) 트리거"가 확정된 설계이나 실제 부팅 경로에서는 비활성 (코드 갭 아님 - 알려진 비활성화, 완전 추적됨)

`SP-0C7A4F3B`가 approved로 전환된 뒤(2026-09-23) 처음 이 방법론을
적용해 §1(범위) 6개 항목을 실제 코드와 대조했다:

1. FADT 확장 파싱 - `Acpi` 클래스에 구현됨, §2가 명시한 실측값
   (`sci=9 smi_cmd=0xb2 pm1a_evt_blk=0x600 ...`)까지 코드와 정확히
   일치. **갭 없음.**
2. 최소 `\_S5` DSDT 스캔 - `power.cpp`의 `kFindS5SleepType()`으로
   구현됨, §3 알고리즘(PkgLength/ComputationalData 인코딩)과 일치.
   **갭 없음.**
3. `Power` 클래스 `shutdown()`/`reboot()` - 구현됨, §1 항목3이 서술한
   SMI_CMD→ACPI_ENABLE→PM1 제어 레지스터 순서 및 8042 폴백 그대로.
   **갭 없음.**
4. 새 syscall 2종(Shutdown/Reboot, 그룹10) - `PowerService::
   registerSyscallEndpoints()`로 구현됨, `RM-48E1E610`에도 반영됨.
   **갭 없음.**
5. **ACPI 전원 버튼(SCI) 트리거** - `kPowerKernelMain`/
   `kSubscribePowerButtonInterrupt` 등 구현 자체는 전부 존재하고
   SMP1에서 실제 `system_powerdown` 모니터 명령으로 E2E 검증까지
   끝났으나, **GRUB SMP4에서 매우 높은 빈도로 재현되는 PANIC**이
   발견돼 `kmain.cpp`의 스폰 호출 한 줄이 현재 주석 처리돼 있다 -
   즉 이 approved 설계 항목이 **지금 부팅 경로에서는 비활성**이다.
   `Task::numaNode` 사례와 표면적으로 같은 모양(확정된 설계가 실행
   경로에 안 살아있음)이지만, 이번엔 **미착수로 조용히 빠진 게
   아니라 의도적으로 비활성화하고 그 사실 자체를 코드 주석
   (kmain.cpp)+계획(`PN-0B461E6F`)에 상세히 기록해 둔 경우**라 새
   PN을 또 등록하지 않는다 - 기존 `PN-0B461E6F`가 이미 이 갭을
   정확히 추적 중이므로 여기서는 상호 링크만 남긴다.
6. `FileSystemDriver::onUnmount()` + 마운트 전체 순회 - 구현됨(다만
   함수명이 설계 문서의 `MountTable::unmountAll()`이 아니라
   `MountTable::unmountAllForShutdown()`으로 지어졌다 - 기능은
   `Power::shutdown()`/`reboot()` 양쪽 모두 레지스터 조작 직전에
   호출하는 것으로 코드 확인, 완전히 일치. 이름 차이는 순수 표기
   문제라 별도 갭으로 등록하지 않음). **갭 없음(기능 기준).**

**결론**: 항목5 하나만 "확정된 설계가 실행 경로에 없음" 상태 -
이미 `PN-0B461E6F`가 원인 조사 중이므로 추가 조치 불필요, 이
문서에는 감사 결과만 기록. 부수적으로 `kmain.cpp`의 관련 주석이
"100% 재현"이라는 이제는 정정된 서술을 그대로 갖고 있던 것도 발견해
같은 틱에서 갱신(commit fd93b252, §5 규칙 - 문서 walk-back 시 근거
소스 주석도 함께 갱신).

## 참고
- `SP-0C7A4F3B` - 이번에 대조한 설계 문서.
- `PN-0B461E6F` - 항목5 비활성화 상태를 이미 추적 중인 계획.

### 1-V. `SP-2AAD7C8D` §9.3 - `OpenFlags` enum이 확정만 되고 실제 코드엔 정의조차 없었음 (코드 갭, 완전 해소, commit 75b511c)

- **출처**: `SP-2AAD7C8D` §9.3("Syscall API")이 `OpenFlags`(ReadOnly/
  WriteOnly/ReadWrite/Create/Truncate/Append/Directory)를 코드
  스니펫으로 명시적으로 확정해 뒀고, `OpenArgs::flags`/
  `KernelFsOpenArgs::flags`(mount_table.h)도 처음부터 이 값을
  담을 목적으로 존재했다(주석에 "§9.3 OpenFlags" 명시).
- **실제**: `OpenFlags` enum 자체가 코드 어디에도 정의돼 있지 않았다
  - `flags` 필드는 그냥 raw `uint32_t`로 값 없이 지나가기만 했다.
  1차 증분(읽기 전용)에서는 이 필드를 아무도 검사하지 않아 값이
  있으나 없으나 차이가 없어 지금까지 드러나지 않았다.
- **왜 지금 드러났나**: `PN-740005DF`(libvfat 쓰기 경로 잔여) 항목1
  "truncate를 어느 syscall로 노출할지"를 조사하다, 이미 §9.3이
  `OpenFlags::Truncate`(open() 플래그 하나, POSIX O_TRUNC와 동일한
  결)로 답을 정해 뒀다는 걸 재확인 - 그런데 그 enum 정의 자체가
  없어서 실제로 쓸 수가 없었다.
- **고침**: `mount_table.h`에 `OpenFlags` enum 신설(§9.3 코드
  스니펫 그대로). `Fat32Driver::onExec()`의 Open 분기가 `Truncate`
  비트를 처음으로 실제 소비 - 성공적으로 해석된 일반 파일이고
  쓰기 가능 마운트면, 핸들 슬롯을 채우기 전에 클러스터 체인을
  전부 반납 + 디스크 디렉터리 엔트리를 0바이트로 갱신한다.
- **아직 안 채운 것**: `Create`/`Append`/`Directory` 세 비트는 여전히
  어떤 드라이버도 소비하지 않는다(§9.3이 확정한 값 자체는 맞지만,
  "파일이 없으면 새로 만든다"류 동작은 아직 어떤 FileSystemDriver
  구현체에도 없음 - Mkdir이 디렉터리 생성을 이미 지원하는 것과
  달리 파일 생성은 Open()의 Create 플래그로 노출될 예정이나 미착수).
- **검증**: 실제 mkfs.vfat 이미지의 2000바이트 파일을 Truncate
  플래그로 Open → 읽으면 0바이트, 호스트(`mdir`)로도 0바이트+
  클러스터 1개(루트 자신)만 사용 확인. `fsck.vfat` 1회
  auto-correct(기존 알려진 dirty bit/FSInfo 캐시 갭만) 후 완전히
  clean. 표준 4시나리오 무회귀.
- **참고**: `PN-740005DF` 항목1 - 이 발견의 계기, 아직 항목3(LFN
  정리)이 남아 있음.

### 1-U. `PN-CF030FC3`(Mkdir/Unlink syscall) - Rmdir syscall 배선을 빠뜨림 (코드 갭, 완전 해소, commit 1467b93)

- **출처**: `KernelFsOpCode`(mount_table.h)는 처음부터 9개 op
  (Open/Close/Read/Write/Stat/Mkdir/Rmdir/Unlink/Readdir)를 정의해
  뒀고, `PN-CF030FC3`(제목 "Mkdir/Unlink syscall 구현")가 그 중
  Mkdir/Unlink만 실제 syscall 번호(그룹3 call 12/13)+핸들러로
  노출했다.
- **실제**: Rmdir은 `vfs_syscall.h`/`vfs_syscall.cpp` 어디에도
  syscall 번호/핸들러가 없었다 - `PN-CF030FC3` 계획 본문 자체가
  제목/범위 어디에도 Rmdir을 언급하지 않아, 의도적 제외가 아니라
  순수 누락으로 보인다(Readdir처럼 "범위 밖"으로 명시적으로 적어 둔
  것과 다름).
- **왜 지금까지 관찰 가능한 버그가 아니었나**: `Fat32Driver`/
  `Ext4Driver` 모두 `Rmdir` 케이스를 그동안 `PermissionDenied` 스텁
  으로만 뒀었고(1차 증분들이 전부 읽기 전용), livefs도 항상
  `PermissionDenied`라 syscall 자체가 없어도 "아무도 실제로 부를
  일이 없는" 상태였다 - `PN-9D6FE4B6`가 `Fat32Driver::Rmdir`을 처음
  실구현하면서 "그런데 이걸 부를 syscall이 아예 없다"는 게 드러났다.
- **고침**: `RM-48E1E610` 그룹3 call 14로 예약, `RmdirArgs`/
  `kSyscallEndpointRmdir`(vfs_syscall.h) + `RmdirHandler`
  (vfs_syscall.cpp, `UnlinkHandler`와 완전히 동일한 골격) 추가 후
  `registerSyscallEndpoints()`에 등록.
- **검증**: 표준 4시나리오 QEMU 회귀 무회귀(구조가 이미 검증된
  Mkdir/Unlink 핸들러의 기계적 복제라 별도 실측 없이 컴파일+회귀로
  충분하다고 판단 - `Fat32Driver::Rmdir` 자체의 로직 검증은
  `PN-9D6FE4B6`에서 실제 mkfs.vfat 이미지로 이미 마쳤음).
- **참고**: `PN-9D6FE4B6`(libvfat 쓰기 경로) - 이 발견의 계기.

### 1-T. `SP-D02C4A73`(libswapfs) §2 - "swap도 SP-7CC5693A §5 5단계 우선순위 판별을 그대로 적용받는다, 새 DC 불필요"가 코드에 반영 안 돼 있었음 (코드 갭, 완전 해소, commit 5a3edeb)

- **출처**: `SP-D02C4A73` §2가 명시적으로 확정 - "스왑 영역이
  파티션인지 파일인지는 별도 결정이 필요 없다 - `SwapBackend::
  mount(BlockDevice* device)`가 이미 `FileSystemDriver::mount`와
  동일하게 ... swapfs도 ext4/FAT32와 마찬가지로 **자신만의 블록
  장치(파티션)에 마운트**된다 - `SP-7CC5693A` §5 5단계(우선순위대로
  슈퍼블록 판별)가 그대로 적용된다. 새 DC 불필요."
- **실제**: `fs.cpp`의 `kTryAutoMountBlockDevice()`(§2가 가리키는
  바로 그 우선순위 판별 함수)는 ext4→FAT32까지만 시도하고, 그
  선언부 주석이 "libswapfs는 이 순위 목록에 있지만 FileSystemDriver를
  구현하지 않는 별도 인터페이스라 일반 파일시스템 자동 마운트 대상이
  아니다"라고 명시적으로 **swap을 이 우선순위 체인에서 배제**하고
  있었다 - VFS 마운트 지점에 안 붙는다는 사실(맞음)과 "그래서 애초에
  탐지 자체를 안 해도 된다"(SP-D02C4A73 §2가 부정한 결론)를 혼동한
  것으로 보인다. `PN-4859FDE9`의 2026-09-23 이전 회차 검토(§2 아래
  기존 항목)도 §4(PTE 인코딩/폴트-인)만 대조했을 뿐 이 §2 조각은
  놓치고 있었다.
- **왜 지금까지 관찰 가능한 버그가 아니었나**: `gSwapBackend`(스왑
  탐지 결과를 담을 전역)가 아예 존재하지 않았고, 회수 스캔/페이지폴트
  스왑인 소비자 자체도 아직 미배선(`PN-4859FDE9` 진행 중)이라 이
  누락이 지금까지 어떤 실행 경로에도 영향을 주지 않았다 - 순수하게
  "다음에 필요해질 소비자를 위한 선행 배선이 통째로 빠져 있었다"는
  성격의 갭.
- **고침**: `kTryAutoMountBlockDevice()`에 `gSwapBackend.mount(&gAhciBlockDevice)`
  를 ext4/FAT32 다음 3번째 우선순위 후보로 추가(마운트 성공해도 VFS
  경로에는 연결하지 않음 - 스왑은 VFS 개념이 없음, §2 자체가 이미
  이렇게 규정). fs.cpp 밖의 향후 소비자(회수 스캔/페이지폴트 스왑인)를
  위해 `kernel::kActiveSwapBackend()` 접근자를 `swap_backend.h`에
  신설.
- **검증**: 실제 `mkswap`으로 만든 스왑 파티션 이미지로 양성(탐지됨)
  확인, 빈 스크래치 디스크로 음성(탐지 안 됨) 확인 - 둘 다 실제 QEMU
  AHCI 부팅으로 실측(TEMP 로그, 검증 후 원복). 표준 4시나리오 무회귀.
- **참고**: `PN-4859FDE9`(진행 중, 이 발견의 고침을 포함) - 회수
  스캔/페이지폴트 스왑인 배선 자체는 여전히 미착수.

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

### 1-E. `SP-71DA77B3`(인터럽트 구독) `WaitInterrupt` `onCancel` 미구현 -
댕글링 포인터 위험 (코드 갭, `PN-C4611402`/§1-B와 동일 결함 클래스,
**해소 완료**)

- **출처**: 설계자 지시("설계 공백을 찾아다녀봐")로 재개한 이번 스윕에서
  `SP-71DA77B3`를 처음으로 이 문서 방법론에 대입 - `interrupt_subscription.h`/
  `.cpp`(PN-B3DD3D19가 구현)와 대조.
- **문제**: `WaitInterruptHandler::onExec`이 이벤트가 없으면 자기 자신
  (`AsyncTask*`)을 `slot->waiters`(`InterruptWaiterQueue`, channel.h의
  `AsyncTaskWaitQueue`와 동일한 침습적 FIFO)에 매달아 두고 파킹하는데,
  `onCancel(AsyncTask*, void*) override {}`가 **완전한 no-op**이다 -
  제출 UserThread가 대기 중 죽으면(`PN-40E976F2`의 취소 경로) 그
  포인터가 제거되지 않고 댕글링으로 남아, 다음 인터럽트의 ISR이
  `waiters.popFront()`로 그걸 꺼내 `AsyncReactor::submitCompletion()`을
  호출하는 순간 UAF가 된다. **§1-B(Channel IPC)에서 이미 발견·수정한
  것과 정확히 같은 패턴**(코루틴 자신의 `AsyncTask*`를 침습적 대기
  큐에 걸어 둔 채 파킹 → onCancel이 그 큐에서 제거 안 함) - `PN-B3DD3D19`
  (2026-09-15 착수)가 §1-B의 수정(`PN-C4611402`, 2026-09-17)보다
  먼저 만들어져 그 교훈이 반영되지 못한 것으로 보인다.
- **조치**: **`PN-BD276A24`**(scheduled) 등록 - `InterruptWaiterQueue`에
  `AsyncTaskWaitQueue::remove()`와 동일한 패턴의 `remove()` 추가,
  `WaitInterruptHandler::onCancel`에서 `args->vector`로 슬롯을 찾아
  호출. `SubscribeInterrupt`/`UnsubscribeInterrupt`/`GetInterruptDump`
  세 핸들러는 onExec에 yield 지점이 없어 no-op 유지가 맞음(§1-B와
  동일 논리). `SP-71DA77B3`에 정정 각주 추가 완료.
- **의미**: 이 문서(§5 기록 규칙)가 "한 문서를 고칠 때 같은 설명이
  복제됐는지 의심하라"고 이미 적어 뒀는데, 이번 발견은 그보다 한
  단계 더 일반적인 패턴을 보여준다 - **한 서브시스템에서 잡은 결함
  클래스(onCancel 미구현)가 비슷한 시기에 독립적으로 설계/구현된
  다른 서브시스템에도 그대로 재현될 수 있다**. 앞으로 이런 "구조적
  결함 클래스"를 하나 잡을 때마다, 같은 침습적 대기 큐 패턴을 쓰는
  다른 서브시스템(예: `WaitInterruptHandler`와 구조가 같은 향후
  핸들러)이 있는지도 함께 훑는 것을 이 문서의 표준 절차에 추가할
  가치가 있다.
- **[완료, 2026-09-17, commit 58c7416]** `InterruptWaiterQueue::remove()`
  추가 + `WaitInterruptHandler::onCancel` 구현 완료 - 단 `args->vector`로
  owner를 재조회해 슬롯 하나만 찾는 대신(취소 시점엔 `submitterTask`가
  이미 비어 있을 수 있어 `ConnectChannelHandler`류가 그 경로를 피한 것과
  같은 이유), 그 벡터의 구독자 슬롯(최대 8개) 전부를 훑어 제거하는
  더 단순한 방식으로 구현(`PN-BD276A24` 참고). `Subscribe`/`Unsubscribe`/
  `GetInterruptDump`는 no-op 유지(이유 주석 추가). `InterruptWaiterQueue::
  remove()` 자체는 TEMP 단위 테스트(head/중간/tail/미존재)로 검증,
  실제 취소 레이스 왕복은 `PN-C4611402`와 동일하게 코드 감사로 검증
  (재현 수단 자체가 `PN-B5C2845A` 대기 중이라 동일한 제약).
- **현재 상태**: **완전 해소.**

### 1-J. `SP-83A07867`(CR3 동기화 통합) §3.2 - Task-to-Task 직접 전환이
"CR3 재동기화 필요 지점 정확히 세 곳" 목록에서 빠짐 (코드 갭,
완전 해소, 2026-09-18, commit `fb06753`)

- **출처**: minicore-88이 `PN-87D6B615` "남은 범위 2번" E2E 재현 중
  실측 발견(`PN-B5FD7B75`) - `SP-83A07867` §3.2가 "CR3 재동기화가
  필요한 재개 지점은 정확히 `kTaskStartTrampoline`/`yieldCurrent`
  재개/`parkCurrent` 재개 이 세 곳뿐"이라고 확정해 둔 전제가 실측으로
  깨졌다.
- **문제**: `Scheduler::onTick()`의 Task-to-Task 직접 전환
  (`kContextSwitch(&current->savedRsp, next->savedRsp)`)이
  `current->savedRsp`를 `onTick()` 함수 본문 한가운데(그
  `kContextSwitch` 호출 바로 다음 줄)에 남기는데, 이 지점이 그
  "정확히 세 곳" 목록에 없다. 나중에 이 `current`가 `runLoop()`의
  idle→Task 디스패치 경로(§3.2가 "도착 지점이 항상 그 세 곳 중
  하나이므로 CR3 재동기화 불필요"라고 명시한 바로 그 최적화)로
  재선택되면 CR3가 전혀 재동기화되지 않아, 재개된 Task가 자기 코드를
  실행하는 순간 즉시 #PF로 죽는다.
- **재현**: 단일 코어(SMP=1)에서 devmgr이 자신의 자식(dbgtarget)을
  DebugContinue로 같은 코어에서 즉시 Ready시킨 뒤, devmgr 자신의
  `submit()`/`wait()` 사이 짧은 구간에 스케줄러 틱이 끼어드는 정확한
  타이밍 경쟁 - 실측 재현율 ~3회 중 1-2회. `ring3 #PF task=devmgr
  rip=cr2=0x400052 cr3=0x106000`(devmgr 고유 pml4Phys는 0x1633000)
  으로 CR3-RIP 불일치가 시나리오와 정확히 일치함을 로그로 확인. 이전
  "코어 간 마이그레이션 가설"은 이 실측으로 기각됨(단일 코어에서도
  재현) - 진짜 원인은 같은 코어 안에서의 preemption 타이밍 경쟁.
- **왜 지금까지 안 드러났는가**: 이 버그는 "한 코어에 동시에 Ready인
  서로 다른 두 Task"가 있어야 발현되는데, 기존 스케줄러 테스트는
  대부분 멀티코어라 두 번째 Task가 대개 Push/Pull로 다른 코어로
  가버려 `onTick()`의 Task-to-Task 분기 자체가 잘 안 트리거됐다.
  단일 코어 + 같은 코어에서 즉시 Ready(디버그 세션)라는 이 세션
  전체에서 처음 만들어진 조합이라 처음 드러남.
- **조치**: `QU-29793535`(설계자 답변 대기, `PN-B5FD7B75`에 세 후보
  정리) - (A) `onTick()`의 그 `kContextSwitch` 다음 줄에
  `kSyncCr3(current)` 추가(권장 - §3.2의 "트랩 진입점에서 동기화"
  철학과 대칭), (B) `runLoop()` idle 디스패치가 항상
  `kSyncCr3(next)` 호출(§3.2 최적화 포기, 안전하지만 불필요한 호출
  추가), (C) 둘 다(과설계 가능성). 스케줄러 핫패스라 이 세션은 직접
  QEMU 검증 없이 발행하지 않는다는 원칙대로 구현 전 확인을 구한
  상태 - 아직 미수정.
- **[답변, 2026-09-18]** `QU-29793535` 설계자 답변: "스위칭이 일어나는
  순간에 CR3를 바꾸는게 맞다고 생각이 드는데." - (A)안(그 `kContextSwitch`
  다음 줄에 `kSyncCr3(current)` 추가) 채택으로 확인됨. minicore-88이
  질의 소유자로 직접 확인·resolved 처리(2026-09-18 05:39) - 이
  세션의 relay 불필요.
- **[완전 해소, 2026-09-18, commit `fb06753`]** `Scheduler::onTick()`의
  그 `kContextSwitch` 다음 줄에 `kSyncCr3(current)` 추가 - 정확히
  방향 A 그대로 구현됨. **부수 발견**: `Scheduler::onForcedMigration()`
  도 동일한 `kContextSwitch(&current->savedRsp, next->savedRsp)`
  Task-to-Task 직접 전환 패턴을 갖고 있어 같은 네 번째 재개 지점
  문제에 노출돼 있음을 구현 세션이 스스로 찾아내 같은 수정을 함께
  적용함(원 버그 리포트/설계자 답변엔 `onTick()`만 언급됐으나 같은
  결함 클래스를 능동적으로 확장 점검한 사례). devmgr+dbgtarget E2E
  하네스로 SMP1 + 동시 Ready 유저 태스크 2개 조합 재현 - 수정 전
  100% 재현(4번째 Task-to-Task 전환에서 cr3=gBootPml4Phys로 #PF),
  수정 후 8회 반복 무크래시(그중 6회는 이 취약 경로가 실제로 실행됨을
  breadcrumb으로 확인). `kSyncCr3`의 skip-if-same 최적화로 흔한
  재개 경로(CR3가 이미 맞는 경우)엔 추가 비용 없음.
- **현재 상태**: **완전 해소.**

### 1-K. `PageFrame` 구조체 - 구조체 교체 + rmap/LRU 1단계 배선 (완전 해소 - COW rmap 이동, PN-610CA401 완료 commit `a696970`)

- **출처**: `SP-6CEFBE9B`("물리 페이지 프레임 메타데이터 — PageFrame
  구조체", 2026-09-18 approved)가 rmap(§6)/swap LRU(§7)/캐시타입
  일관성(§3) 필드를 포함한 64바이트 `PageFrame` 구조체를 확정했다.
- **[완료, 2026-09-18, commit `bd43196`, PN-2FC5ED36 항목1-2]**
  `page_frame_allocator.h`/`.cpp`가 기존 `uint16_t` 배열을 실제
  `PageFrame[]`(64바이트, `static_assert` 확인)로 교체 완료 - 코드를
  직접 읽어 설계와 대조한 결과 필드/플래그/주석 전부 `SP-6CEFBE9B`
  §1/§2/§5와 정확히 일치함을 확인(설계 이탈 없음). `retain()`/
  `refCount()` API 시그니처 불변, `PG_RESERVED`/`numaNode` 실제
  세팅, 신규 `frameFor(physAddr)` 접근자까지 계획대로 구현됨. QEMU
  3개 표준 시나리오 무회귀 실측 확인.
- **[완료, 2026-09-18, commit `dea9f1c`, PN-2FC5ED36 항목3-4]**
  `insertRmap()`/`removeRmap()`(page_frame_allocator.cpp)을 코드로
  직접 확인 - `SP-6CEFBE9B` §6.2(삽입/제거 규칙)·§7.2 1단계(최초
  진입 시 inactive 리스트 push, 중복 삽입 방지)와 정확히 일치.
  `ProcessAddressSpaceManager`의 `mapRegion`/`registerFixedRegion`/
  `resizeAnonymousRegion`(Anonymous 매핑 지점 3곳 전부) → `insertRmap`,
  `kRollbackMapped`/`unmapRegion`/`unmapAll` → `removeRmap` 배선
  확인. `freeOrder()`에 설계 문서엔 없던 방어적 rmap/LRU 청소까지
  구현 세션이 스스로 추가(정상 경로의 안전망, 설계 이탈이 아니라
  타당한 보강). devmgr+fs 2-프로세스 실측(15+12회) 무크래시 확인.
- **의도적으로 남은 잔여 항목(둘 다 이 문서의 "갭"이 아니라 아직
  실사용처가 없는 것으로 판단 - 각자 명확한 미래 트리거가 있어
  `PN-2FC5ED36`은 completed로 종결하고 아래로 분리 추적)**:
  1. **[갱신, 2026-09-18 - 트리거 발생, 실제로는 미배선 확인 - `PN-610CA401`
     로 분리 추적]** `kHandleCowWriteFault`의 rmap "이동"(COW 새 프레임으로
     엔트리 이전) - `fork()`(`PN-44C91D6E`)가 2026-09-18 완료·발행됐으나
     (commit `996a424`), 예고했던 대로 함께 배선되지 않았다.
     `paging.cpp`의 `kHandleCowWriteFault`를 코드로 직접 확인 -
     `insertRmap`/`removeRmap` 호출 0건(oldPhys의 스테일 엔트리 미제거,
     newPhys의 신규 엔트리 미삽입). 아직 rmap 소비자(swap 스캔)가 없어
     지금 당장 관찰 가능한 버그는 아니지만, `PN-4859FDE9` 착수 전에
     고쳐야 하는 순수 구현 갭으로 `PN-610CA401`(scheduled) 등록 완료.
  2. LRU §7.2 2단계(`PG_ACCESSED` 세팅) - `paging.cpp` 전수 확인
     결과 참조 0건(미배선 맞음) - v1은 재폴트 경로 자체가 거의 없어
     소비처가 없다는 판단이 타당함(`SP-6CEFBE9B` §7.3 기존 합의와
     일치) - swap 착수 시(`PN-4859FDE9`) 재검토.
  3. `elf::loadIntoAddressSpace()`의 PT_LOAD 세그먼트가 `VmaBacking::
     Anonymous`로 등록되는지는 `elf.cpp`가 이 저장소 `minicore/kernel`
     밖에 있어 미확인(`PN-2FC5ED36`에 참고용으로 남김, Anonymous가
     아니면 rmap 커버리지에 조용한 공백 가능성 - 다음에 ELF 로딩
     경로를 손댈 세션이 확인 권장).
- **부수 확인**: 같은 날 `PN-9E2CC631`(FileBacked 캐시 정책)도
  완료돼 `SP-6CEFBE9B` §6.3/§7.4가 "Anonymous/FileBacked 공유 LRU"로
  갱신됐다(정책 확정, 코드는 fs 서비스 실코드 대기).
- **현재 상태**: **완전 해소.** 구조체/rmap/LRU 1단계 골격에 이어,
  위 잔여 항목1(COW rmap 이동)도 `PN-610CA401`(completed, commit
  `a696970`, 2026-09-19)로 `kHandleCowWriteFault`에
  `removeRmap(oldPhys,...)`/`insertRmap(newPhys,...)` 호출을 추가해
  마저 배선했다 - QEMU 회귀 3종(PVH no-initrd SMP1/SMP4, GRUB 실제
  initrd SMP4) 무회귀 확인. rmap 소비자(swap 스캔 `PN-4859FDE9`)는
  여전히 미착수라 이 배선의 직접적인 관찰 가능한 효과는 아직 없지만,
  설계(`SP-6CEFBE9B` §6.2)가 요구한 부기 자체는 이제 코드와 일치한다.
  캐시타입 불일치 처리는 `PN-81223433`, swap 스캔 트리거 정책은
  `PN-4859FDE9` 착수 시로 계속 별도 추적.

### 1-L. `SP-9DD4F3EA`(PnP) §6 - devmgr 메인 시퀀스 절이 "devmgr 최초
코드 없음"으로 낡아 있었음 (문서만 정정, 코드 갭 아님)

- **발견**: §6 마지막 문단이 "이 시퀀스 자체의 실제 구현은 devmgr
  프로세스의 최초 코드(현재 존재하지 않음)가 필요하므로"라고 적어
  뒀으나, `PN-BD9AAE2F`(devmgr 서비스 프로세스 최초 구현)가 이미
  완료돼 `minicore/devmgr/main.cpp`가 실존한다 - 코드로 직접 확인한
  결과 1단계(스폰)/2단계(PCI 열거)는 구현 완료, 6단계(자식 쪽 IO
  권한 확보)도 devmgr 자신의 임시 자기 검증으로 syscall 왕복까지
  확인됐다(진짜 드라이버 자식이 아직 없어 "자식이 호출"하는 형태는
  아님). 3단계(설정 로드)/4단계(드라이버 매칭)/5단계(자식 스폰)/
  7단계(핫플러그 대기, 지금은 essential 계약만 만족시키는 `pause`
  자리표시자)는 실제로 여전히 미착수 - 문서의 "전부 미착수"라는
  서술만 낡았을 뿐, 실제 남은 항목 자체는 정확했다.
- **조치**: §6 마지막 문단을 실제 구현 현황(1/2/6-부분 완료,
  3/4/5/7 미착수, `PN-A0F72A3A`가 4/5의 후속 계획)으로 정정.
  코드 쪽 조치 불필요.
- **현재 상태**: 완전 해소(문서 정정).

### 1-M. `SP-9DD4F3EA`(PnP) §5 "선행 조건" - 세 항목이 낡아 있었음
(문서만 정정, 코드 갭 아님)

- **발견**: §1-L과 같은 문서를 이어서 훑다 §5(선행 조건) 절에서 세
  건을 더 발견했다:
  1. "devmgr 실행 파일 내용이 아직 없어 initrd에서 'not found,
     skip'으로 처리됨" - §1-L과 같은 원인(`PN-BD9AAE2F` 완료 반영
     안 됨).
  2. "커널→유저 인터럽트 라우팅(`PN-B3DD3D19`, `scheduled` - 실제
     배선은 미착수)" - 실제로는 `PN-BD9AAE2F` 항목6이 이미
     완료(commit `2543c72`)로 갱신해 둔 것을 이 절만 놓쳤다.
  3. "프로세스간 공개 인터페이스 registry(`PN-268F062B`) - 설계자
     확인 대기" - 가장 중요한 정정: 이 전제 자체가 **나중에
     뒤집혔다** - `SP-B071E628` §5-A/§5-B가 "커널 서비스는 pubreg에
     등록하지 않는다"로 최종 확정(같은 문서 §6 6단계가 이미 이
     반전을 정확히 반영해 뒀는데 §5만 낡은 채 남아 있었다) - devmgr은
     pubreg를 아예 몰라도 된다.
- **조치**: §5의 해당 세 항목을 실제 현황으로 정정. 코드 쪽 조치
  불필요.
- **현재 상태**: 완전 해소(문서 정정).
- **일반 원칙**: §1-L/§1-M 둘 다 같은 문서(`SP-9DD4F3EA`) 안에서
  나왔다 - 한 문서 안에서도 절마다 갱신 시점이 다르면(어떤 절은
  최신 커밋을 반영했는데 다른 절은 안 했음) 불일치가 남을 수 있다는
  뜻 - 문서 하나를 대조할 때 "완료 표시가 있는 절 vs 없는 절"을
  전부 훑는 습관이 유효함을 재확인.

- **[발견+정정, 2026-09-20] `SP-9DD4F3EA`(PnP 프레임워크) §3.2/§3.3/§6 -
  AHCI 아키텍처 뒤집힘(QU-1FB6A7A4)이 반영 안 돼 있었음**: 이 문서의
  §3.2("devmgr이 probe() 성공 시 자식 프로세스로 스폰")/§3.3("이
  syscall의 실제 호출자는... 드라이버 자식 프로세스")/§6 5-6단계가
  AHCI를 그 구조의 예시로 계속 인용하고 있었는데, `SP-C2670F69` §3.1이
  이미 뒤집어 둔 사실(AHCI는 이제 devmgr 경유 없이 fs가 직접
  EnumerateDevices/RequestIoPermission을 부른다, `PN-F60E405A` 완료)이
  이 상위(부모) 프레임워크 문서에는 교차 반영이 안 돼 있었다 - 정정
  절 추가(원문 프레임워크 서술 자체는 향후 비-스토리지 PnP 드라이버를
  위해 그대로 유효, AHCI 하나의 소유권만 옮겨진 것).
  - **조치**: `SP-9DD4F3EA`에 AHCI 소유권 이전(§3.2/§3.3/§6)을 반영한
    정정 절 추가. 코드 쪽 조치 불필요(코드는 이미 옳음 - 문서만 낡음).
  - **현재 상태**: 완전 해소(문서 정정).
  - **일반 원칙**: §5의 "복제된 서술" 위험(부모-자식 문서 간)이 실제로
    또 발생한 사례 - `SP-C2670F69`(자식, AHCI 드라이버) 하나만 뒤집고
    그 소유권을 위임한 부모 프레임워크 문서(`SP-9DD4F3EA`)는 놓치기
    쉽다. 설계가 뒤집힐 때는 그 설계를 예시로 인용한 상위 문서까지
    거꾸로 추적한다.

- **[발견+정정, 2026-09-20] `SP-5D965B74`(procfs) §6 - PN-85FA4992/
  PN-617F4E52 완료가 교차 반영 안 돼 있었음**: §6이 "self 외 임의/자식
  pid 열람"을 여전히 `PN-617F4E52`(권한 체계) 완료 대기 중인 것처럼
  서술하고 있었는데, 실제로는 `PN-617F4E52`(completed, 2026-09-18)와
  `PN-85FA4992`(completed, 2026-09-20, `kCanViewProcessStatus()` 실제
  구현·발행까지 끝남)가 둘 다 이미 완료돼 있었다 - 이 문서(부모격)에
  그 완료가 교차 반영되지 않은 채 남아 있던 사례. §2("v1은 self만")도
  같은 이유로 낡아 있었다.
  - **조치**: `SP-5D965B74` §6에 완료 반영 정정, §2도 "selector가
    self 또는 pid로 일반화됨" 각주 추가. 코드 쪽 조치 불필요(코드는
    이미 옳음).
  - **현재 상태**: 완전 해소(문서 정정). 남은 실측 검증(`PN-012D6310`,
    planned)은 procfs 자체 문제가 아니라 "유저랜드에 실제
    `SpawnProcess` 사용 사례가 없다"는 별개의 더 큰 선행 공백이라
    이 문서의 범위 밖.
  - **일반 원칙**: §5가 이미 경고한 "완료된 후속 계획이 그 계획을 낳은
    상위 설계 문서에 교차 반영 안 되는" 패턴이 또 발생 - `SP-9DD4F3EA`
    (위 항목)와 같은 급의 반복. 계획을 completed로 전이할 때 `refs`로
    걸린 SP 문서의 "남은 것/미룸" 절도 함께 훑는 습관이 필요해 보인다
    (현재는 계획 쪽만 갱신되고 설계 문서 쪽은 누락되는 비대칭이
    반복됨).

## §1-Q. [발견 및 해소, 2026-09-21] `SP-83A07867` §9-사전의 교차
참조가 낡아 있었음 - coroHandle.resume() CR3 미동기화 갭은 이미
해소됐는데 "미해결"로 계속 표시돼 있었음

`SP-83A07867`(스케줄러 디스패치 CR3 동기화 통합, approved) §9-사전이
"AsyncReactor::drainOnce()의 coroHandle.resume() 재개 경로도 CR3
미동기화 위험에 노출 - PN-2008220B에서 미해결로 남아 있다"고 서술한
상태였다. 실제로는: `PN-2008220B`(완료) 자신의 범위는 idle 컨텍스트를
안전한 전용 스택으로 옮기는 것뿐이었고, 진짜 CR3 동기화 로직은
후속으로 분리된 `PN-387C18A5`가 담당했는데, 그 계획이 2026-09-18에
`PN-0EB2FABF` 4단계(Join syscall - 이 커널 최초의 진짜 `co_await`
소비자)로 실측 PANIC까지 재현하며 `kSyncCr3ForAsyncExecEntry`/
`kRestoreCr3AfterAsyncExecEntry` 재사용으로 완료돼 있었다(`PN-929CE93E`도
같은 커밋으로 완료). `SP-83A07867`이 갱신되지 않아 이미 닫힌 갭이
계속 "미해결"로 읽히는 상태였다 - 문서에 해소 각주 추가로 정정
완료. **현재 상태**: 코드 자체는 이미 갭 없음(§2로 이동), 문서만
낡아 있던 사례.

## §1-S. [발견 및 해소, 2026-09-23] `SP-F1987EF8`(libexfat) §3.5 - exFAT
디렉터리 엔트리 집합 체크섬 "읽기 시 검증"이 구현에서 빠져 있었음

`SP-F1987EF8`(libexfat, approved 2026-09-22)이 §3.5에서 명시한
확정된 설계: "**`setChecksum`은 읽기 시 검증, 쓰기 시 반드시
재계산**(무결성이 이 체크섬에 의존)". `PN-09970F05`(ExfatDriver
구현, 완료)가 `ExfatFileDirEntry::setChecksum` 필드는 정확히 옮겨
파싱했지만, 실제로 그 값을 계산/비교하는 코드가 어디에도 없었다 -
`kParseFileEntrySet()`이 Primary/Stream 엔트리를 읽고 파일명을
조립하는 로직은 갖췄지만 체크섬 검증 단계 자체가 빠진 채 항상
`valid=true`로 반환했다. 여러 필드/단계를 목록으로 나열한 §3.5
섹션에서 "체크섬 검증"이라는 한 단계가 조용히 누락된, 이 문서
서두가 경고한 패턴(`Task::numaNode` 사례와 동일 계열) 그대로였다.

**해소**: `kExfatEntrySetChecksum()`(Linux 커널 `fs/exfat/exfat_fs.h`
의 `exfat_calc_chksum16`과 동일 알고리즘) 추가, `kParseFileEntrySet()`
에서 계산값이 저장된 `setChecksum`과 다르면 무효 처리하도록 수정
(`PN-831A3998`, commit `863113f`). `mkfs.exfat`+`exfat-fuse`로 만든
실제 이미지로 정상/손상(setChecksum 1바이트 손상) 양쪽 다 실측
검증 - 정상은 통과, 손상은 정확히 거부됨을 확인. 표준 4시나리오
QEMU 회귀 무회귀. **쓰기 시 재계산**은 exFAT 쓰기 경로 자체가 아직
없어 이번 범위 밖(쓰기 경로 착수 시 함께 구현 예정).

## §2. 점검 완료 - 갭 없음 확인

- **[점검 완료, 2026-09-23] `SP-0C7A4F3B`(Power 서브시스템, approved
  2026-09-23) §1 6개 항목 전수 대조 - 갭 없음(항목5는 §1-W에 이미
  기록된 알려진 비활성화, 새 발견 아님)** - 항목1(`Fadt` 구조체,
  acpi.cpp) 구현 확인, 항목2(`\_S5` 스캔, power.cpp
  `kFindS5Package`류) 구현 확인, 항목3(`Power::shutdown()`/
  `reboot()`) 구현 확인, 항목4(Shutdown/Reboot syscall)/항목6
  (`onUnmount()` 훅)은 `PN-0B461E6F` 완료 기록으로 확인. **같은
  시점 approved된 `DC-F367AD5D`는 별도 감사 대상이 아님** - 결정
  요청 문서 자체(자기만의 "확정된 설계" 절이 없음)이고, 그 결정이
  이미 `SP-0C7A4F3B`로 구체화돼 위에서 함께 점검됐다.
- **[점검 완료, 2026-09-23] `SP-D02C4A73`(libswapfs, approved
  2026-09-22) §4 - 스왑 PTE 인코딩/폴트-인 경로 미구현은 "실제 갭"이
  아니라 이미 정직하게 문서화된 의도적 범위 결정** - §4.2(폴트-인
  경로, `Paging::handlePageFault()`의 `PAGE_SWAP_MARKER` 분기)와
  §4.1(스왑아웃 경로 배선)이 `PAGE_SWAP_MARKER`/`kMakeSwapPte`/
  `kSwapSlotFromPte`(paging.h)가 정의만 되고 코드 어디서도 호출되지
  않는 상태임을 확인해 처음엔 §1급 발견으로 의심했으나, 원 구현
  계획 `PN-6D9A5DAE`(completed) 본문이 "이번 범위에서 하지 않은 것"
  절에서 이미 정확히 같은 사실을 스스로 밝히고 있었다(`SP-6CEFBE9B`
  §6/§7의 rmap/reclaim 스캐너 자체가 그때 아직 없어 붙일 자리가
  없었다는 근거와 함께) - CLAUDE.md 규칙4를 지킨 정직한 스코프 컷.
  후속 의존 사슬(`PN-6D9A5DAE`→`PN-4859FDE9`§7.2 5단계→`PN-FFFE892E`)
  도 전부 정확히 추적돼 있었다. **새로 확인한 사실**: `PN-FFFE892E`
  (AHCI 인터럽트 기반 완료 전환, §7.2 5단계의 데드락 위험을 없애는
  진짜 선행 조건)가 이제 완료(commit 30cc142)돼 있어, `PN-4859FDE9`
  §7.2 5단계(실제 회수+스왑 쓰기)가 착수 가능 상태다 - `PN-4859FDE9`
  본문이 이미 "다음 틱 후보"로 스스로 기록해 둔 그대로라 새 계획
  등록은 불필요. **남은 주의점**: 쓰기(swap-out) 경로를 먼저 완성해도
  §4.2(swap-in) 없이는 스왑된 페이지가 영원히 복구 불가능하므로,
  `PN-4859FDE9` §7.2 5단계 착수 세션은 반드시 `SP-D02C4A73` §4.2
  (`Paging::handlePageFault()` 확장, `SP-0666DB3C` §7이 이미 확정해
  둔 "폴트를 pendingSyscalls 항목으로 모델링" 흐름 재사용)까지 같은
  단위로 함께 구현해야 한다 - 쓰기만 만들고 읽기를 미루면 그 자체가
  새로운 실제 갭이 된다.

  **[추가 조사, 2026-09-23, 같은 세션 다음 틱]** 착수를 실제로
  시도해보니 예상보다 한 겹 더 있었다 - `kReclaimScanCallback()`
  (page_frame_allocator.cpp, §7.2 5단계 자리가 이미 주석으로 표시돼
  있음)은 `DelayedExecutionQueue::pump()`가 `AsyncReactor::
  drainOnce()` 안에서 중첩 호출하는 평범한 함수 포인터 콜백이라
  코루틴이 아니다(`co_await` 불가) - 반면 `SwapfsBackend::writeSlot`/
  `readSlot`(libswapfs)은 순수 동기 함수(`device_->writeBlocks()`
  블로킹 래퍼)뿐이다. 즉 "이제 AHCI가 인터럽트 기반이니 그냥
  writeSlot을 부르면 된다"가 아니라, **스캔 콜백이 직접 I/O를
  못 하므로 별도의 코루틴 기반 `AsyncTaskHandler`(ext4/vfat/exfat
  드라이버의 onExec와 동일한 "평탄화 co_await" 패턴)를 새로 만들어
  스캔은 그 핸들러를 non-blocking `AsyncTask::submit()`으로 제출만
  하고, 그 핸들러의 `onExec` 코루틴 안에서
  `co_await kernel::AsyncTaskCoroAwaiter(device_->submitWriteBlocks(...))`
  로 실제 쓰기를 완료한 뒤(이 완료는 나중에 drainOnce()가 정상적으로
  재진입할 때 자연스럽게 처리됨 - 스캔 콜백 자신은 블로킹 대기 없이
  즉시 반환) 물리 프레임을 반납해야 한다**. 안전 최우선 판단으로
  이 세션에서는 구현을 시작하지 않았다(메모리 회수/페이지 폴트는
  이 세션에서 이미 여러 차례 미묘한 재진입/데드락 버그가 나온
  영역 - `QU-41F78A3E`/`PN-A0CEF82D` 등 - 서두르지 않는 편이 안전
  하다고 판단) - 다음 착수 세션은 이 3개 조각(async 쓰기 핸들러 +
  스캔의 non-blocking 제출 배선 + swap-in 폴트 핸들러)을 전부 하나의
  검증 단위로 준비하고 들어갈 것.

  **[추가 조사, 2026-09-23, 같은 세션 다음 틱] §7.2 5단계+§4.2를 실제로
  구현·실측했고, §4.2(swap-in)에 커널 전체 행(hang) 미해결 버그가
  확인돼 다시 비활성 상태로 되돌렸다 - 지금은 "설계 대비 미구현"이
  아니라 "구현은 됐으나 안전하게 켤 수 없어 의도적으로 꺼 둔" 상태다.**
  `PAGE_RECLAIM_INPROGRESS`(PTE AVL 비트, 위 §7.2 5단계 동시 접근
  레이스의 QU-D3A9BF13 답변대로) + `Paging`의 4개 신규 API +
  `SwapReclaimWriteHandler`/`SwapInReadHandler`(둘 다 코루틴 기반
  `AsyncTaskHandler`)로 §7.2 5단계와 §4.2를 같은 커밋(`1f3f824`)에
  구현했다. 스왑아웃은 실제 initrd(init/pubreg/authmgr)+mkswap 디스크로
  58개 진짜 프레임 회수까지 완전히 실측 검증됐지만, 스왑인은
  `Scheduler::parkFromISR(thread, frame)`(#DB/IST4의 `kHandleUserBreakpointHit()`
  과 정확히 같은, 이미 프로덕션 검증된 패턴)를 #PF/IST5 경로에서 부르는
  순간 커널 전체가 패닉 없이 조용히 완전히 멎는 것을 100% 재현 확인
  (`va=0x400000 slot=54`) - gdb 부착 시도는 이 프로젝트에 이미 기록된
  타이밍 왜곡 heisenbug 패턴(`PN-3DDF2797`/`PN-E4C6AF72`)과 동일하게
  실행 속도를 왜곡시켜 재현 실패, 근본 원인 미확정. 대응으로
  `kReclaimScanCallback()`의 `kReclaimScanReclaimPass()` 호출 단 한
  줄만 주석 처리해 활성화를 되돌렸다(§7.2 2/3/4단계만 유지 - 기존
  안전 검증된 baseline과 동일) - 나머지 메커니즘 전부는 컴파일된 채
  보존돼 그 한 줄만 되살리면 재활성화된다. 자세한 재현 조건/가설/
  검증 로그는 `PN-4859FDE9` 최상단 절 참고. 이 상태 자체는 §5가
  경고하는 "설계가 확정한 것이 코드에 없다"는 성격의 갭이 아니라
  (오히려 설계 그대로 구현까지 됐다) 안전을 위한 의도적 비활성화이므로
  §1(발견)로 옮기지 않고 이 자리에 이어서 기록해 둔다 - 다음 착수
  세션이 `parkFromISR`/#PF 버그를 고치고 그 한 줄을 되살리면 이
  추가 조사 자체가 자동으로 해소된다.

- **[점검 완료, 2026-09-23] `SP-AA6DF406`(libntfs, approved
  2026-09-22) §3.2 - MFT 레코드 fixup(Update Sequence Array) 검증/
  복원, 문서 자신이 "이 단계를 빠뜨리면 모든 파싱이 조용히 틀어진다"
  고 명시적으로 경고해 둔 항목이라 최우선으로 대조** - `PN-52C577F3`
  (NtfsDriver, completed)의 `kApplyFixup()`(ntfs_driver.cpp)이 정확히
  구현돼 있음을 확인: magic=="FILE" 확인, USA 배열 경계 검사, **각
  섹터 끝 2바이트가 저장된 USN과 실제로 일치하는지 검증**(불일치 시
  거부 - 맹목적 복원이 아니라 진짜 무결성 검사)까지 전부 설계
  그대로. 4개 레코드 읽기 호출부(Open/Stat/Readdir 등) 전부 이
  함수를 거치는지도 grep으로 확인. **갭 없음.**

- **[점검 완료, 2026-09-21] `SP-E9B44929`(Syscall Group+Call 2단계
  인코딩, approved)** - §7 요약 절이 §6의 세 미결 질문(슬롯 저장
  구조/breaking ABI 승인/그룹 번호 배정 방식)을 여전히 "확정 안 함"
  으로 적어 뒀지만, §6-A가 이미 전부 답변받고 구현까지 끝낸 상태
  (부팅 순서 함정으로 힙 대신 정적 범프 풀로 교체한 실측 버그
  수정 포함)였다 - `minicore/kernel/syscall.h`의
  `kMakeSyscallEndpointId`/`kSyscallGroupOf`/`kSyscallCallOf` 실제
  존재를 git_grep으로 확인, §7 각주로 정정. 갭 없음(코드 기준),
  또 문서 요약 절만 낡아 있던 사례.

- **[점검 완료, 2026-09-21] `SP-83A07867`(스케줄러 디스패치 CR3
  동기화 통합, approved)** - §3.2/§8의 CR3 동기화 지점 전부(§3.2의
  네 지점 + §10이 발견한 다섯 번째 Task-to-Task 직접 전환 지점)
  구현·실측 검증 완료(`PN-40210D5A`/`PN-A3C7471F`/`PN-B5FD7B75` 전부
  completed). §9-사전이 추적하던 세 번째 재개 경로(coroHandle.resume())
  CR3 미동기화 갭도 위 §1-Q에서 확인한 대로 이미 완료 - 문서의
  교차 참조 각주만 낡아 있어 정정. 갭 없음(코드 기준).

- **[점검 완료, 2026-09-21] `SP-9F1DB1D8`(gCurrentTask 크로스코어
  접근 보호 - RwSpinlock 설계, approved)** - §2/§6/§7(모든
  `gCurrentTask` 접근에 예외 없이 `RwSpinlock` 적용, `currentTask()`
  포함)이 `PN-D3597800`(completed, commit 2d0da74)으로 구현 완료 -
  구현 중 최초 조사(§1)가 놓친 두 접근부(`retireCurrentTask()`/
  `handleFpuTrap()`)를 추가로 발견해 함께 수정했다고 정직하게 기록.
  §7이 별도 설계로 분리한 "onExec() currentTask() 오용 재발 방지"는
  `PN-C536F352`(completed, commit 75bb8f0, `TaskOwnerRef` 도입)로
  별도 완료 - 설계 스케치의 무인자 정적 팩토리(`captureCurrentFrame`류
  자동 캡처)가 `Task`가 `EnableSharedFromThis`를 상속하지 않아 실제로는
  불가능했다는 점, 기존 ~40개 `submitterTask.lock()` 호출부를 건드리지
  않기 위해 `TaskOwnerRef::lock()`을 호환 별칭으로 남긴 최소 침습
  선택까지 문서에 정직하게 남아 있다. 갭 없음.

- **[점검 완료, 2026-09-21] `SP-CA3C3E57`(Channel/BridgeHandle 안전한
  핸들 해석 세부 설계, approved)** - §2/§4/§5(세대 태그 슬롯 테이블
  `kResolveChannelId`/`kAllocateChannelId`/`kFreeChannelId` + 세
  호출부 교체)와 §6.1(소유자 필드 `Channel::ownerProcess`, Accept/
  Destroy 권한 검증) 전부 commit 74f0f75로 구현 완료(문서 자체가
  이미 정직하게 명시 - 실제 syscall 왕복을 통한 PermissionDenied
  거부 경로는 유저랜드 소비자 부재로 end-to-end 미검증이라는 caveat
  포함). §6-A의 `DontDeref<T>` 타입 승격은 `PN-18FDBFF3`(completed,
  commit f454faf)로 별도 완료 확인. 문서가 범위 밖으로 분리해 둔
  `PN-260D7D73`(Channel을 SharedPtr 관리로 전면 마이그레이션)은
  착수 조건 충족을 이미 확인했으면서도 "DontDeref<T> 완료로 남은
  위험이 순수 확률적 ABA뿐이라 시급하지 않다"는 근거로 의도적으로
  `scheduled` 상태에서 보류 중 - 문서 자신의 우선순위 판단과 실제
  상태가 정확히 일치. 갭 없음.

- **[점검 완료, 2026-09-20] `SP-9CB55C5B`(Kill 대상 확장 - 안전한
  ProcessId 해석 메커니즘, approved)** - §2/§3의 세대 태그 슬롯
  테이블 + `kResolveProcessId()`는 `PN-C39882D0`(commit ba7e4f5)로
  구현 완료(`process.h`/`process.cpp`), §4가 분리 위임한 Kill 권한
  스코프는 `SP-30FCC8AE`(uid/gid+RWX+root)로 확정된 뒤
  `PN-88E62419`가 `kCanSendSignal()`(KernelService 예외 → 직계 부모
  예외 → `kCheckPermission()`)로 실제 배선까지 완료(코드 확인:
  process.cpp:1760 `kCanSendSignal`, 1807 `KillHandler::onExec`의
  `kResolveProcessId` 호출), §5의 테이블 동시성 보호도 `PN-AA30E4C8`
  (`gProcessTableLock`을 `RwSpinlock`으로 교체, commit 666b92b)로
  완료. **문서 자체의 결함 발견**: §7 요약 절이 QU-78E4159E 답변
  이전 시점 표현("제안, 확정 아님")과 §5의 후속 계획 귀속(엉뚱하게
  `PN-90BD044E`로 적었던 내 첫 시도의 오기까지 포함)이 낡아 있어
  `document_patch`로 정정(실제 담당은 `PN-AA30E4C8`, `PN-90BD044E`는
  "같은 패턴"이라는 순수 참고 관계일 뿐 무관). 코드 자체는 갭 없음 -
  문서만 낡아 있던 사례.

- **[점검 완료, 2026-09-20] `SP-5A255B7C`(비동기 프레임워크 우선 설계
  원칙 평가 - 동기 구현의 wrapper화 검토, approved)** - §5가 제안한
  `MutexCore`/정책 주입 구조(`BasicMutex<Policy>`, `Mutex =
  BasicMutex<ParkingPolicy>`, `AsyncMutex = BasicMutex<YieldingPolicy>`)가
  실제로 `minicore/kernel/mutex_core.h`/`semaphore_core.h`에 그대로
  구현돼 있음을 코드로 확인 - 문서 §9가 스스로 "SP-0666DB3C §13으로
  반영 완료"라 표시해 둔 그대로였다. 오히려 구현이 문서의 원 스케치를
  실측으로 넘어서는 개선(§13 원문은 `tryAcquire()`/`onContended()`를
  두 단계로 그렸으나, 그 사이 창에서 wakeup 유실 경쟁이 실측 확인돼
  - PN-C9625015와 동일 클래스 - 하나의 스핀락 보유 구간 안에서
  확인+대기등록을 원자적으로 묶는 Mesa 모니터 패턴으로 수정,
  `PN-B41D8C0E`가 이후 `EnableSharedFromThis`/`WeakPtr` 별칭 생성까지
  추가)까지 코드 주석에 전부 추적코드와 함께 정직하게 남아 있음 -
  이런 종류의 "설계 대비 개선"은 방치된 갭이 아니라 이 방법론이 찾는
  대상(§14)에 해당하지 않는다. §8의 열린 하위 과제 3개 중 1번(재진입
  Mutex)은 `PN-4D60D49C`(scheduled)로, 2번(`SemaphoreCore` 정리)은
  이미 코드 존재로 완료 확인, 3번(devmgr 비동기 소비자)은 범위 밖
  후속 과제로 별도 추적 대상(이 문서가 새로 등록할 필요 없음 - 실제
  devmgr 연결 시점에 자연히 §6-3 원칙을 적용하면 됨). 갭 없음.

- **[점검 완료, 2026-09-19] `SP-6BEAE0C1`(일반 프로세스 생성 syscall
  fork/exec류 - 동적 Process 풀/COW/프로세스 트리 원 설계, approved)** -
  §11의 4개 구체화 항목(프로세스 트리 자료구조/syscall 분할 형태/COW
  참조 카운트 배치/wait() ABI) 전부 실제로 구현·완료됐음을 코드로
  확인. §12가 약속한 두 후속 계획도 실제로 등록·완료돼 있음을 확인 -
  `PN-543C0CE9`(fork/exec류 syscall, completed)와
  `PN-7FF5DA89`(좀비/고아 프로세스 처리 QA 계획, completed, §6의
  QA 케이스 1/2/3 그대로) 둘 다 존재. §9 검증 계획이 스스로 "§6/§11
  확정 반영해 갱신 필요"라고 표시해 둔 채 방치된 것처럼 보였으나,
  실제 검증은 이 문서 갱신 대신 위 두 후속 PN과 이후 여러 세션(예:
  이번 세션의 SIGCHLD/Wait 실측)에 걸쳐 흩어져 완료됐다 - 문서 자체의
  §9 절만 낡아 있을 뿐 실제 코드/검증 갭은 아니다. 갭 없음(문서
  §9의 "갱신 필요" 문구가 사소하게 낡아 있다는 점만 기록 - 우선순위
  낮은 문서 정리 대상, 별도 PN 등록까지는 불필요).

- **[점검 완료, 2026-09-19] `SP-6A563A8F`(ResourceGroup CPU 쿼터/freeze/
  VFS 노출/syscall 확장 설계)** - §2(자료구조)/§3(`kCheckAndResetCpuPeriod()`
  주기 롤오버)/§4(스로틀 - `pickNext()` 큐 순회 변경)/§5(계층적 쿼터
  강제)/§5-A(동적 그룹 생성/삭제)/§6(`kResourceGroupOf(Task*)` 헬퍼)/
  §7(`ResourceGroupSetCpuQuota` 등 syscall 6종)을 실제 소스
  (`resource_group.h/.cpp`, `scheduler.cpp`)와 `PN-4190BBD3`(completed,
  전 항목 1-7 + devmgr TEMP 하네스 실측)의 완료 기록에 전부 정확히
  대응됨을 확인 - 특히 §6처럼 체크리스트 번호에 명시적으로 안 걸려
  있어 누락되기 쉬운 항목(`kResourceGroupOf`)도 실제로 존재하고
  `scheduler.cpp:1407`에서 호출되고 있음을 직접 grep으로 확인했다.
  갭 없음.

- **[점검 완료, 2026-09-19] `SP-30FCC8AE`(사용자/권한 체계, review→
  approved로 전환돼 새로 대상이 된 문서 - §3 절 자신이 다음 후보로
  미리 지목해 뒀던 것)** - §1(uid/gid 필드, root=0, 상속 규칙)/§2
  (`Permission` POSIX mode_t 9비트+S)/§3(`kCheckPermission()` 판정
  순서)/§4(`Kill`의 `kCanSendSignal`)를 실제 소스(`libkenv/permission.h`,
  `process.cpp`)와 전문 대조 - 전부 설계 그대로 정확히 구현돼 있음을
  확인. §3이 "직계 부모만 vs 조상 전체(임의 depth)" 중 하나를 확정
  안 하고 열어 둔 지점도, `PN-88E62419`(완료 기록)가 "DebugAttach의
  `kFindDebuggableChild`와 일관성을 맞추기 위해 직계 부모만 채택"이라고
  명시적으로 판단 근거까지 남겨 둔 것을 확인(CLAUDE.md 규칙4 예외에
  해당하는 사소한 구현 세부 - 숨겨진 임의 결정 아님). §7(v1 제외
  항목)의 서술도 실제 코드 상태와 일치(setuid 승격 경로 없음, 다중
  그룹 없음 등 전부 코드에도 그대로 미구현 상태로 확인). §8(후속
  계획)이 예고한 `PN-B6DB692C`(UserRecord+kSetuid, status=planned,
  PN-24A2B6F5에 의존)/`PN-24A2B6F5`(authmgr+sudo/su) 둘 다 실제로
  등록돼 있고 상태도 정확(아직 미착수 - authmgr 자체가 큰 신규
  서브시스템이라 당연함). `RM-32D06563`에도 `Uid`/`Gid`/`Permission`/
  `kCheckPermission` 이미 등록 완료(규칙12 준수 확인). **갭 없음.**

- **[점검 완료, 2026-09-19] `SP-DE19BB1C`(커널 영역 TLB 샷다운, IPI
  기반)** - §2(핵심 메커니즘: Mailbox 구조체/`Lapic::sendFixedIpi`/
  브로드캐스트/ISR)와 §5(다중 요청 슬롯 확장, 벡터 배정)가 이미
  확정 표시와 커밋 근거를 갖고 있었는데, 실제 소스(`tlb_shootdown.h/
  .cpp`, `lapic.h`, `address_space.cpp`)와 전문 대조한 결과 전부
  정확히 일치함을 재확인 - `TlbShootdownRequest`(요청자 코어 인덱스별
  슬롯, `gRequests[kAcpiMaxCpus]`)/`gPendingMask[]`(수신자별 Target
  Pending Mask)/벡터 `0xE0`/`Lapic::sendFixedIpi` 전부 문서 그대로
  구현돼 있고, `KernelAddressSpaceManager::unmapRegion()`(커널 영역,
  인자 생략)과 `ProcessAddressSpaceManager::unmapRegion()`/
  `resizeAnonymousRegion()`(유저 영역, `_pml4Phys` 전달) 세 호출부
  모두 실제로 존재함을 grep으로 확인 - **갭 없음.**

- **`PN-C4611402`(Channel IPC `onCancel`, "connectChannel 취소:
  대기열에서 자신의 PendingConnectRequest 제거")** - [2026-09-18]
  당시 코드 검토로만 확인했던 "댕글링 포인터 없음"을 `PN-B5C2845A`가
  열어 준 실제 Kill-중-파킹 레이스로 QEMU에서 처음 재현/확정했다.
  devmgr이 자기 채널에 `connectChannel`로 무기한 파킹 → 외부에서
  `raiseSignal(Kill)` → `ConnectChannelHandler::onCancel()` →
  `Channel::removePendingConnect()`가 실제로 그 대기 항목을 큐에서
  제거해 `pendingHead`/`pendingTail`을 정확히 비움을 3회 반복 확인
  (TEMP 스캐폴딩, 원복 완료 - 발행할 프로덕션 diff 없음). 갭 없음 -
  설계/코드/실측 3단이 전부 일치.

- **`SP-D7013B26`(Slab 할당자/libkmm)**: "확정된 최종 설계" 절이 나열한
  전 항목(더블 매거진 loaded/previous, `PreemptionGuard` 강제, 매거진
  용량 16 고정, 버킷 7단계 32/64/128/256/512/1024/2048, `sizeToBucket()`
  단일 경유, 슬랩 Order 0 고정, 고갈 시 즉시 nullptr 비블로킹, 2048B
  초과는 PageFrameAllocator 직행)를 `libkmm/slab.h`/`slab.cpp`와 한
  줄씩 대조 - 예외 없이 전부 설계 그대로 구현돼 있음을 확인
  (`Scheduler::disablePreemption`/`enablePreemption`도 scheduler.h에
  실재). 갭 없음 - 이 프로젝트에서 보기 드물게 처음부터 끝까지 정확히
  구현된 사례로 기록.

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
- **[갱신, 2026-09-20] `SP-39F18E30`(DMA 버퍼 관리자) - 구현 완료,
  갭 없음**: 이전 라운드에서 "`AllocDmaBuffer`/`FreeDmaBuffer` 미구현"
  으로 기록됐던 것이 이 세션(`PN-29614AD7`)에서 실제 구현 완료됐다 -
  §2/§3.1 할당 경로(4KiB 올림→order 계산→`physAddrLimit==32`면
  §5-B `allocOrderBelow`/아니면 `allocOrder`→`ProcessAddressSpaceManager::
  mapRegion(VmaBacking::FixedPhysical)`→`Process::dmaBuffers` 장부
  기록) 전부 코드와 정확히 대조 확인. §3.3 캐시 속성(`PAGE_CACHE_DISABLE`)
  도 실제 `mapRegion()` 호출에 반영됨 확인. §5-B `allocOrderBelow`도
  §5-C(a) 선형 탐색/§5-D(NUMA 지역성 없이 전체 노드 순회) 그대로
  구현. §6이 열어 뒀던 마지막 항목(프로세스 종료 시 물리 프레임
  반납 누수, `PN-FFC2F062`)도 `Process::destroy()`에 실제 배선
  완료(commit `0dc91c4`) - completed로 갱신 확인. **확정된 설계
  전부 코드로 반영됨 - 갭 없음.**
- **`SP-29D652AA`(진짜 컴파일러 thread_local)**: §7까지 전부 확정된
  approved 설계이지만 실제 구현은 `PN-22E5E9E7`(scheduled, 미착수)
  으로 이미 정확히 등록돼 있음을 `plan_get`으로 확인 - 디스패치
  핫패스를 건드리는 위험도 때문에 의도적으로 미착수 상태(minicore-88
  세션이 이미 인지하고 보류 중) - openly 추적 중, 숨은 갭 아님.
  **[갱신, 2026-09-17] 툴체인 자체가 원인이었던 것으로 최종 확정 -
  x86_64-elf-gcc 크로스컴파일러로 전면 교체 완료(commit e7943e0)**:
  이 문서가 여러 틱에 걸쳐 추적해 온 `PN-22E5E9E7`의 lld PT_TLS
  결함 조사(QU-58D13EAE~QU-90A616DA 5라운드, lld 18/19/20 전수 기각→
  gcc 혼입/오브젝트 신선도/PT_LOAD 구조/higher-half 전환 전부 배제→
  "실제 오브젝트 세트 조합에 의존하는 lld 다중 오브젝트 TLS 크기
  합산 결함"으로 좁혀짐)가 설계자 직접 지시로 해결됐다 - 호스트
  배포판 clang+lld 대신 `/opt/cross`의 `x86_64-elf-gcc 13.2.0 + GNU
  Binutils 2.42`(이 타깃 전용 진짜 크로스컴파일 툴체인)로 전면 교체,
  같은 오브젝트 세트로 `PT_TLS.p_memsz`가 정확히 계산됨을 실측
  확인. `DS-D4E5C451`(핵심 결정 문서, "컴파일러: WSL clang")에 정정
  각주가 이미 정확히 반영돼 있음을 독립 확인(원문 보존, 교체 사실+
  근거+영향받은 플래그 3종 명시). 코드젠 차이로 `-fcoroutines`/
  `-fno-threadsafe-statics` 플래그 추가, `.init_array` 출력 섹션
  신설+`kmain.cpp`의 `kRunGlobalConstructors()`(BSP 극초반 1회 순회)
  배선까지 확인 - GCC가 clang과 달리 일부 전역 객체를 진짜 동적
  초기화로 코드젠한다는 사실이 새로 드러난 것으로, 앞으로 이
  프로젝트의 "정적 초기화만으로 충분하다"는 암묵적 전제를 쓰는 전역
  객체가 있다면 재검토 가치 있음(당장 코드 갭은 아님 - 일반 방어
  메커니즘이 이미 배선됨). `PN-22E5E9E7` 자신의 원래 구현 범위(항목
  1-7, TLS 배선)는 이 블로커 해소로 이제 착수 가능하나 아직 미착수
  (계획 status 여전히 scheduled, 2026-09-17 기준) - 숨은 갭 아님,
  다음 착수 시 재대조 필요.
- **[갱신, 2026-09-20] `SP-C2670F69`(AHCI) - §3.1-3.4 구현 완료,
  §3.5(NCQ, 확정) 미반영 발견 → `PN-A401DDF9` 등록**: HBA 초기화
  (GHC.AE/CAP/PI), 포트 시작 절차(PxCLB/PxFB/FRE/ST 순서), IDENTIFY
  DEVICE/READ·WRITE DMA EXT/FLUSH CACHE EXT까지 §3.1-§3.4가 코드로
  실제 구현됨을 확인(`minicore/fs/ahci.h/.cpp`, PN-4E6EA13D/
  PN-F60E405A, QEMU 실측 완료). §3.3(인터럽트)은 "제안"(확정 아님)
  단계라 폴링으로 openly 대체 중 - 갭 아님. **그러나 §3.5(NCQ)는
  "[확정, 2026-09-18, 설계자 지시]"로 명시된 확정 설계인데 실제
  구현은 슬롯0 고정(비-NCQ)만 있다** - 코드 주석/계획 본문엔 이미
  기록돼 있었지만(openly 알려짐) CLAUDE.md 규칙7이 요구하는 별도
  PN 추적이 없었다 - `PN-A401DDF9`로 신규 등록(정확성엔 영향 없음,
  §3.5 자신이 "미지원 시 슬롯1개로 자연 일치" 명시 - 순수 성능
  후속 과제). 아키텍처 자체도 뒤집혔음(§3.1 - devmgr 자식 프로세스
  대신 fs 프로세스 자신이 직접 구동, QU-1FB6A7A4 답변) - 문서
  자체에 이미 정정 절 추가 완료.
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

- **`SP-04EE2A18`(Syscall 디스패치)**: 핵심 계약(submit/wait 분리,
  `SyscallRegistry`의 고정 슬롯+동적 subjectCode 간접화, 등록 안
  된 endpoint/Slab 고갈 시 블로킹 없이 실패) 전부 `syscall.h`/`.cpp`에
  그대로 구현돼 있음을 확인. `UserThread::pendingSyscall`이 원안의
  단일 필드에서 `ChunkedList` 기반 다중 슬롯(`pendingSyscalls`)으로
  진화했지만, 이는 숨겨진 이탈이 아니라 코드 주석에 명시된
  `QU-31402585`/`QU-F475C6C2`(2026-09-14, 같은 날 후속 답변)로
  확정된 정당한 확장 - `waitForMultipleSyscall`/`waitAnyForMultipleSyscall`
  까지 구현됐다. 문서가 "verb/레지스터 배치는 아직 범위 밖"이라고
  적어 둔 부분도 실제로 `kDispatchSyscallVerb`(idt.cpp)가 지금도
  verb 0(submit)/1(wait)만 처리하고 멀티웨이트용 verb는 배정 안 돼
  있음을 확인 - 문서 서술과 정확히 일치(유저랜드 자체가 아직 없어
  당장 필요하지도 않음, RM-23F4B687 §4 패턴). 갭 없음.

- **`SP-201238BB`(SharedPtr/WeakPtr/UniquePtr)**: 매우 큰 문서라
  `libkenv/shared_ptr.h`/`type_traits.h`/`spinlock.h`와 전면 대조 -
  `Atomic<T>`(AtomicU32/AtomicPtr 통합), `ControlBlockBase`/
  `ControlBlock<T,Deleter>`/`SharedPtr<T,Deleter>`/`WeakPtr<T,Deleter>`
  (별칭 생성자 포함)/`EnableSharedFromThis<T>`/`kMakeShared`/
  `UniquePtr<T,Deleter>`/`kIsBaseOf`(type_traits.h로 분리) 전부 실재.
  §2-B(`IntrusiveControlBlock`/`IntrusiveRefCounted`/
  `IntrusiveSharedPtr`/`IntrusiveWeakPtr`/`kMakeIntrusiveShared`)만
  `shared_ptr.h`에 없음 - 그러나 문서 자신이 "v1에서 실제로 적용할
  타입은 아직 미정(설계만 제공)"이라고 명시해 둔 항목이라 숨겨진
  갭이 아니라 공개적으로 유예된 설계(RM-23F4B687 §4 패턴). 갭 없음.

- **`SP-B071E628`(pubreg)**: `PN-185406F6`(pubreg 서비스 구현)이
  여전히 `scheduled`이고 `minicore/pubreg` 디렉터리 자체가 없어(git_tree
  확인) 코드 대조 자체가 성립하지 않는다 - 순수 설계 단계. 문서
  §8의 "9개 항목 전부 교차 확인 완료" 자체 선언을 표본으로 재검증
  (SP-EAB162FC §2.2의 `pubreg` 추가, `RM-48E1E610`의 `PublishInterface`/
  `QueryInterfaces` "폐기" 갱신) - 둘 다 정확함. 실제 코드 갭은 아직
  발생할 수조차 없는 단계 - 갭 없음(착수 시 재점검 대상으로 §3 유지
  가치 있음, 아래 이동 없이 여기 기록만).

### 1-F. `SP-9525C4C0`(Push/Pull 로드밸런싱) Push 경로에 §5.3 FPU 안전
가드 누락 (코드 갭, **완전 해소**)

- **출처**: 이번 스윕에서 `SP-9525C4C0`를 처음 대입 - `scheduler.cpp`와
  대조하던 중 이 문서 §5.3("이번 설계의 핵심 기여")이 확립한
  `kCanMigrateFpuSafely()` 가드가 Pull(`runLoop()`, ~1351행)에는
  정확히 있는데 **Push(`enqueue()`, ~929-943행)에는 없음**을 발견.
- **문제**: Task가 lazy FPU 소유("`gFpuOwner[coreIndex] == task`")를
  쥔 채로 Push가 다른 코어 큐로 옮기면, 원래 코어의 하드웨어 FPU
  레지스터에만 있는 최신 값이 저장되지 않고 유실될 수 있다 - §5.3이
  이미 "진짜 정합성 버그"로 명시한 바로 그 시나리오. Pull은 §5.3의
  v1 절충안(`kCanMigrateFpuSafely` 확인 후 안전하지 않으면 이관 스킵)을
  그대로 구현했지만 Push는 그 가드 없이 무조건 이관한다 - 타이밍
  의존적이라 드물게만 발현되는 데이터 손상.
- **[완료, 2026-09-17, minicore-88 세션, commit f753d19]** 원안이
  스스로 남긴 미결 질문("`coreIndex`가 항상 Task의 실제 FPU 소유
  코어와 일치하는가")을 착수 세션이 직접 조사 - `Scheduler::enqueue(`
  호출부 5곳(`debug_session.cpp`/`kmain.cpp`×2/`process.cpp`/
  `resource_group.cpp`) 전수 확인 결과 **전부 waker 자신의 코어일
  뿐 Task의 실제 FPU 소유 코어와 무관**함을 확인 - 원안의
  `kCanMigrateFpuSafely(task, coreIndex)` 재사용 방식은 채택하지
  않고, `gFpuOwner[]` 전체를 스캔해 Task의 실제 살아있는 FPU 소유
  코어를 찾는 `kFindFpuOwnerCore(const Task*)`를 신설 - Push 이관
  후보 코어가 그 소유 코어와 다르면 이번 이관을 스킵(Pull과 동일한
  보류 정책)한다. QEMU PVH(sentinel-only 경로)/GRUB SMP4(실제 스캔
  경로) 양쪽 TEMP 격리 검증 완료. `SP-9525C4C0`에도 최종 구현 반영
  완료.
- **의미**: §1-B/§1-E("onCancel 미구현")와는 다른 결함 클래스(비대칭
  가드 누락)이지만, 같은 교훈("한쪽 경로에만 적용된 안전장치가 대칭
  경로에서 빠질 수 있다")을 보여줌 - 이번 3-B 표준 절차에 "대칭 경로
  쌍은 양쪽 다 확인"이 추가된 계기. 원안이 열어 둔 미결 질문을
  착수 세션이 끝까지 추적해 원안과 다른(더 정확한) 해법으로
  귀결된 사례이기도 하다 - 설계 문서의 "확인 필요" 각주가 실제로
  후속 세션에게 정확히 전달돼 작동한 경우.
- **현재 상태**: 완전 해소.

- **`SP-6BEAE0C1`(fork/exec, SpawnProcess syscall)**: 매우 큰 문서 -
  `process.cpp`의 `SpawnProcessHandler`/`WaitHandler`와 전면 대조.
  §3의 `flags` 비트마스크(`kSpawnDebugStart` 검증+소비, `PN-A6E01B8A`/
  `PN-87D6B615` 항목8), §6의 프로세스 트리(`parent`/`children`,
  `PN-E2A114C1`의 WeakPtr 전환 포함), §4의 System V 스택 프레임
  (argv/envp가 실제로 `execImage()`에 전달돼 조립됨, `PN-E35294B8`),
  §11-3의 COW 참조 카운트 인프라(`PageFrameAllocator::retain/refCount`)
  전부 실재 확인 - 여러 세션에 걸친 다수 증분(PN-543C0CE9 등)이
  전부 정확히 교차 참조돼 있다. 갭 없음 - 대형 기능이 이 정도로
  빈틈없이 추적된 드문 사례.

- ~~**`SP-CCACB192`(libjson)**: 착수 전 순수 설계 단계라 갭 판정
  불가~~ **[갱신, 2026-09-17] 착수 완료(commit 12c7481) - §1-G로
  이동, 실제 갭 발견됨(§3 double 지원 전제가 틀림)** - 아래 §1-G
  참고.

- **`SP-F15B4A63`(지연 실행/타이머 인프라)**: 이 문서 자신이 "리액터가
  전용 Task에서 `runLoop()` 인라인 idle 경로로 재설계됐으니
  `pump()` 호출 배선도 재작업 대상"이라고 명시해 둔 항목 - 실제로
  `async_task.cpp`(`AsyncReactor::drainOnce()`의 idle 분기, ~594행)가
  `DelayedExecutionQueue::pump()`를 정확히 그 재설계된 인라인 경로에서
  호출하도록 배선돼 있음을 확인(§3의 최종 방향 그대로). 연결 리스트
  기반 `DelayedExecutionQueue`(§2, 고정 배열 폐기), Resurrect 백오프
  소비 지점(scheduler.cpp)도 실재. 갭 없음.

- **`SP-DF89897F`(커널 로깅 인프라)**: `logger.h`/`.cpp`에
  `Logger`/`LoggingDriver`/`LogLevel`/`SerialLoggingDriver`/전체
  지원 포맷터 전부 구현 확인, `kmain.cpp`(151개 호출부)/`panic.cpp`
  전부 `Logger::*`로 마이그레이션 완료 확인(`kernel::Logger::` 실제
  호출 다수 grep 확인). 유일한 미구현(`BufferedFileLoggingDriver`)은
  `PN-32696F0F` 자신이 "미착수, 범위 밖 유지 - fs 서비스 준비 후
  진행"으로 이미 공개 추적 중 - 숨은 갭 아니다. 갭 없음.

- **`SP-DABFCF9F`(QEMU gdb stub 디버깅 워크플로)**: 도구/스크립트
  문서(커널 C++ 코드 아님) - `run-qemu-gdb.sh`/`run-grub-gdb.sh`/
  `kernel.gdb` 전부 `scripts/`에 실재 확인. 문서 자신이 "설계/구현/
  실측 전부 완료"로 명시한 그대로. 갭 없음.

- **[재검증, 2026-09-17] `SP-B1E258D8`(RCU) - approved 전환 +
  `PN-495C11B7` 구현 완료 후 §5 확정 설계 대 코드 대조**: 재개 조건이
  도래한 정도가 아니라 이미 구현까지 완료됐음을 이번 틱에 확인 -
  `rcu.h`/`rcu.cpp`를 §5와 한 줄씩 대조. `RcuReadGuard`/`RcuCallback`/
  `RcuCallbackTraits`/`Rcu` 클래스 전부 실재, `noteQuiescentStateOnThisCore`
  /`startGracePeriod`/`isGracePeriodComplete`/`callAfterGracePeriod`/
  `drainCallbacksOnThisCore` 메서드 시그니처 그대로, `PerCpu<AtomicU64>
  _lastObservedSeq`/`AtomicU64 _currentSeq`/`PerCpu<List<RcuCallback,
  RcuCallbackTraits>> _pendingList`(libkcont 재사용, 손짜기 연결
  리스트 아님) 필드 구성도 일치. 소비 지점 두 곳도 직접 확인 -
  `scheduler.cpp`(preemption 카운터 0 도달 시 `Rcu::
  noteQuiescentStateOnThisCore()` 호출, ~1644행), `async_task.cpp`
  (`AsyncReactor::drainOnce()` 진입부에서 `Rcu::
  drainCallbacksOnThisCore()` 호출, ~571행 - 계획 본문이 스스로 정정한
  대로 원안의 `reactorTaskEntry()`가 아니라 `drainOnce()`로 배선됨).
  **갭 없음** - 드물게 설계-구현-검증까지 한 틱 안에 전부 정확히
  맞아떨어진 사례.

- **[재검증, 2026-09-17] `PN-2008220B`(§3 이관 항목) 해소 후
  `SP-F682B889` §7.3 재대조**: `PN-2008220B`가 completed로 전환됨에
  따라(코어당 전용 idle 스택 + `Scheduler::enterIdleLoop()` 트램폴린
  구현, commit 7e83ff1) 그 수정이 §7.3의 코루틴 재개 경로 서술과
  여전히 일치하는지 재확인. **결론: 일치, 갭 없음** - PN-2008220B의
  수정은 `runLoop()`의 idle 분기가 서 있는 **스택 자체**를 안전한
  전용 스택으로 옮긴 것일 뿐, §7.3이 서술하는 "`drainOnce()`가
  `coroHandle`이 설정돼 있으면 `coroHandle.resume()`을 직접 호출한다"
  는 호출 메커니즘 자체는 전혀 바뀌지 않았다 - §7.3은 스택 안전성을
  전제/서술한 적이 없어(그 위험 서술은 `scheduler.cpp`/`async_task.cpp`
  코드 주석에만 있었고, `PN-2008220B`가 이미 그 주석들에 정정 각주를
  남김) 애초에 낡아질 내용이 없었다. 단, §7.3이 드러내지 못했던
  "실제 CR3 값 동기화"(스택 안전성과 별개 문제)는 `PN-387C18A5`로,
  실제 co_await 핸들러 재개 시나리오 실측은 `PN-929CE93E`로 각각
  분리 추적 중 - 둘 다 openly 추적이라 숨은 갭 아님.

**[2026-09-17] approved 상태 SP 문서 후보 풀 소진** - 이 시점까지
확인 안 한 `approved` SP 문서가 더 없음(document_list로 재확인
필요시 다음 틱에). **[갱신, 2026-09-17]** §3에 있던 두 재검증 대기
항목(`PN-2008220B`/`SP-B1E258D8`) 전부 §2로 이관 완료 - §3은 이제
`SP-9A6D579F`(부분 착수 중, 완료되면 재대조) 한 항목만 남았다 -
`PN-B5C2845A`(Kill이 Syscall::wait 파킹 대상에 미도달)는 2026-09-18
완료돼 이 목록에서 빠짐(§1 참고).
다음부터는 PL/DC류 문서, 또는 새로 `approved` 전환되는 문서(예:
`SP-B26CDBDD`가 구현되면 그 시점에) 위주로 전환한다.

- **[신규, 2026-09-17] `PL-4BA2B446`(멀티 프로세스 실행 기반 완성) -
  PL류 문서 스윕 첫 사례**: 방법론을 SP/DC뿐 아니라 PL(실행 계획)류
  문서에도 처음 적용 - 이 문서는 2026-09-15 이후 자체 갱신이 멈춰
  있었지만, 그 시점에 "착수 가능"으로만 적어 둔 4단계 계획
  (`PN-0367CDBA`/`PN-B3DD3D19`/`PN-268F062B`/`PN-6D497EB0`) 전부
  이미 `completed`임을 `plan_get`으로 확인 - 코드 갭은 아니지만
  문서가 진행 상황을 못 따라간 사례(§5 기록 규칙과 같은 패턴).
  문서에 "진행 상황 갱신 3" 절을 추가해 4단계 완료 + 5단계(devmgr/fs
  등 하드웨어 착수 순서)가 이미 개별 SP 문서로 분리 추적 중임을
  반영 완료. 갭 없음(문서만 정정).

- **[신규, 2026-09-17] `PL-65C20380`(SMP AP 기동) - kApMain/kMain
  대칭 경로 점검**: 이 문서 자체는 2026-09-14 완료 기록이라 오래됐지만,
  §5(대칭 경로 확인 규칙)에 따라 그 이후 생긴 새 진입점
  (`Scheduler::enterIdleLoop()`, `PN-2008220B`)이 AP 경로(`kApMain`,
  실제로는 `smp.cpp`에 있음 - `kmain.cpp`가 아님, 문서의 오래된 서술과
  달리 파일이 옮겨진 상태이나 이건 문서 갱신 대상이라기보다 단순
  위치 정보라 각주 없이 기록만 함)에도 대칭으로 적용됐는지 직접 대조.
  `kApMain()`이 `runLoop()`을 직접 안 부르고 `Scheduler::
  enterIdleLoop()`을 부르는 것 확인, `Smp::markThisCoreOnline()`/
  `Rcu::initOnThisCore()`(`PN-907C5289`/`PN-495C11B7`가 각각 "BSP의
  kMain()과 대칭되는 지점"이라고 스스로 명시해 둔 두 호출) 둘 다
  `kApMain()`에도 실재 - **갭 없음**, 세 신규 메커니즘 전부 BSP/AP
  양쪽에 정확히 대칭 적용됨.

- **[신규, 2026-09-17] `PL-2D149D8F`(IOAPIC 외부 인터럽트 라우팅)**:
  ISO 파싱/8비트 목적지 한계/다중 IOAPIC 지원까지 3차 재작업이
  전부 반영된 상태 - 유일하게 열린 한계(다중 IOAPIC의 두 번째 이상
  인스턴스 경로, QEMU가 IOAPIC 1개만 에뮬레이트해 실측 불가)도
  `PN-6EB7F9D2`로 이미 openly 추적 중(실하드웨어 확보 전까지 정당히
  `planned` 유지). 갭 없음.

- **[신규, 2026-09-17] `PL-E68894CD`(HPET 지원)**: GSI 비트맵 라우팅/
  멀티 비교기 재작업까지 전부 반영된 상태. 유일하게 열린 항목
  (`PN-04258584`, 코어별 독립 HPET 비교기를 스케줄러와 연동)은
  그 계획 자신이 이미 "현재 스케줄러는 코어별 독립 LAPIC 타이머로
  선점을 잘 처리 중이라 실질 가치는 실측으로 필요성이 드러날 때"로
  스스로 재평가해 둔 정당한 저우선순위 백로그 - 숨은 갭 아님. 갭 없음.

- **[신규, 2026-09-17] `SP-B26CDBDD`(CPU 가중치 스케줄링/vruntime) -
  `PN-158B6B2F` 구현 완료(commit 01f5c591) 후 설계 대 코드 즉시 대조**:
  approved 직후(이 세션이 직접 설계해 4차 개정 끝에 승인받은 문서)
  구현까지 완료된 걸 확인해 바로 독립 검증 - `scheduler.cpp`를 §
  전체와 한 줄씩 대조. `TaskVruntimeTraits`(§3.1)/`kEffectiveWeightOf`
  (§2.1, clamp(100+weight,1,...))/`kVruntimeScale=1024` 고정소수점
  스케일(§2.1, `(kEffectiveWeightBase*kVruntimeScale)/kEffectiveWeightOf`
  - 이 세션이 설계 단계에서 잡은 "스케일 없으면 weight>0에서 몫이
  0으로 버려지는" 바로 그 버그의 수정이 정확히 그대로 구현됨)/§2.3
  굶주림 방지(새로 큐에 들어가는 Task의 vruntime을 큐의 현재 최솟값
  아래로는 못 내려가게 `minVruntime()`으로 보정, `OrderedList::insert`
  전에 적용)/`SetTaskWeight`(§7, `kSelfTaskWeightPid` 자기 자신
  경로만 허용, 직계 자식 경로는 `SP-30FCC8AE` 승인 전까지 항상
  `ok=false`로 명시적 거부 - 설계가 확정한 스코프 그대로)/
  `Process::memoryBytesUsed`(§6.2, `execImage()`/`destroy()`/
  Resurrect 재사용 시점 3곳 모두 그룹 합계와 함께 가산·감산) 전부
  실재 확인. **갭 없음** - 설계-승인-구현-검증이 한 세션 안에서
  전부 정확히 맞아떨어진 사례(SP-B1E258D8/RCU와 같은 급).

- **[신규, 2026-09-17] `PL-57CF86EF`(4K/2M 페이지 병합/분할)**: 경로
  (a)/(c)+분할(`Paging::mapRange`/`mergeRange`/`kSplitTwoMegabyte`,
  paging.cpp) 전부 실재 확인. 유일하게 남은 경로 (b)(임계치 기반
  사후 컴팩션)+재배치는 `PN-D28DD9F3`가 "실제 대량 4K 매핑 소비자가
  아직 없어 의미있는 임계치 계측 불가"로 정당하게 유예 - 숨은 갭
  아님. 갭 없음.

- **[신규, 2026-09-17] `PL-C8648D4D`(Channel IPC 구현) - 대칭/불변조건
  생존 점검**: 이 문서 자체(2026-09-14~16)의 "남은 것" 절은 이미
  전부 취소선으로 완료 표시돼 있어 문서 자체는 갱신이 잘 돼 있다 -
  대신 이 문서가 실측으로 발견/수정한 5개 스케줄러 동시성 버그 중
  근본 수정(버그 5, `Task::inRunQueue` 구조적 이중 스케줄링 방지
  플래그)이 그 이후 크게 재작성된 `gNormalQueues`(FIFO→vruntime
  정렬 `OrderedList`, `PN-158B6B2F`)에서도 여전히 정확히 유지되는지
  대조 - `enqueue()`/`scheduleImmediate()`의 cli-보호 확인+세팅,
  `pickNext()`의 `popMin()` 직후 해제, Pull 경로의 `insert()` 전
  해제(도둑질 실패 시 재`insert()`까지는 계속 `true` 유지) 전부
  정확히 원래 불변조건 그대로 보존됨을 확인. 갭 없음 - 대규모 스케줄러
  리팩터를 거치고도 과거에 실측으로 잡은 동시성 불변조건이 깨지지
  않은 좋은 사례.

- **[신규, 2026-09-17] `PL-21344323`(Syscall 디스패치 구현) - 문서
  정정 발견**: "남은 것" 절의 onCancel 실제 호출 경로 항목이
  "`PN-40E976F2`, Task/프로세스 종료 절차가 없어 아직 연결 불가"라고
  낡은 채 남아 있었으나, 실제로는 그 계획이 이미 2026-09-15/16에
  `completed`(commit eae13a9, `AsyncTaskState::Cancelled`+
  `AsyncTask::ownerTask`+`SelfTerminateHandler::onExec`의
  `pendingSyscalls` 사망 전파 목록 재사용)로 마무리돼 있었다 - 정정
  완료. 코드 갭 아님, 문서만 낡아 있었음.
- **[신규, 2026-09-17] `PL-1E247831`(AsyncTask 프레임워크 구현) -
  같은 낡은 서술 하나 더 발견**: PL-21344323과 완전히 동일한 문구가
  이 문서에도 그대로 복제돼 있었다(§5 기록 규칙이 경고하는 "하나
  고칠 때 다른 문서의 복제본도 확인" 패턴의 실제 사례) - 정정 완료.
  참고로 `SP-04EE2A18`(같은 계보의 설계 문서)는 이미
  "[해소, 2026-09-16, 재확인]" 절로 정확히 갱신돼 있었음(cross-check
  결과 이 세 문서 중 SP만 최신이었던 셈) - 갭 없음, PL 두 건만 문서
  정정.

- **[신규, 2026-09-17] `PL-FC38956C`(multiboot2+GRUB 부팅) - PL 스윕
  마지막 항목, 또 다른 낡은 문서 발견**: "이번엔 안 한 것" 절이
  "CPIO를 실제 initrd 마운트에 연결(PN-71C2B857) - 아직 안 함"이라고
  적혀 있었으나, 실제로는 그 계획이 2026-09-16에 `completed`(livefs
  마운트, `/sys/live/initrd.cpio` 노출) - 게다가 그 후속
  `PN-BC04D3DC`(KernelFsDriver 비동기 인터페이스 마이그레이션)까지
  완료돼 있었다. 정정 완료 - 유일하게 남은 하위 항목(실제 Open/Read
  syscall 배선, `PN-ABD23ACE`)은 `SP-2AAD7C8D` §9 착수와 함께 진행
  예정으로 이미 openly 추적 중. 코드 갭 아님, 문서만 정정.

**[2026-09-17] PL류 문서 스윕 완료** - PL-65C20380/PL-2D149D8F/
PL-E68894CD/PL-57CF86EF/PL-C8648D4D/PL-21344323/PL-1E247831/
PL-FC38956C 전부 점검(3건은 문서만 낡아 있던 정정, 나머지는 갭 없음
확인). 다음 스윕은 DC류 문서(요구분석/결정 문서) 또는 새로
approved 전환되는 문서 위주로 전환한다.

- **[신규, 2026-09-17] `DC-FB38F86F`(Paging::mapPage 동시성 락 전략
  결정) - DC류 스윕 첫 사례, 결정→구현 체인 전체 검증**: 설계자가
  (A) 주소공간별 전용 락 + higher-half 전역 락을 확정한 결정이
  `PN-90BD044E`로 실제 구현됐는지 코드 대조(`paging.cpp`의
  `gHigherHalfPagingLock`/`kLockForAddressSpaceOp`/
  `kMapPageUnlocked` 전부 실재, 재진입 회피까지 설계 그대로) - 갭
  없음. 그 검증 과정에서 파생된 두 후속 발견도 함께 확인: **①
  `PN-907C5289`**(SMP4+initrd 시나리오에서 콘솔에 안 보이는 실제
  Triple Fault - 아직 부팅 안 끝난 AP에 NMI가 도달하는 문제,
  `Smp::isCoreOnline()` 필터로 96% 감소) **② `PN-3081704A`**(남은
  4%의 근본 원인 - 두 코어가 동시에 `kPanic()`에 진입해 서로의
  stop-the-world NMI에 끼어들어 Serial 출력이 섞이는 경합,
  `kTryClaimFirstPanic()` CAS 래치로 40/40 완전 해소) - 둘 다
  `panic.cpp`/`paging.cpp`에 실제 구현 확인. 세 계획 모두 completed,
  결정→구현→실측 검증까지 전 사슬이 정확히 일치하는 좋은 사례.

### 1-G. `SP-CCACB192`(libjson) §3 "double까지 지원" - 컴파일러 제약으로
실제 구현 불가능함이 착수 중 드러남 (설계 갭, **[완전 해소,
2026-09-17, commit 586dcfc]**)

- **출처**: §3이 "(b) 정수+부동소수점 전부 지원... 이 프로젝트는
  이미 FPU 지연 컨텍스트(`PN-F258698E`)가 구현돼 있어 커널 코드에서
  부동소수점 연산 자체는 가능하다"고 확정(QU-8E75915F 답변 채택).
- **문제**: `PN-185406F6`(pubreg) 착수 중 libjson 실제 구현
  (2026-09-17, commit 12c7481)에서 **§3의 이 전제 자체가 틀렸음이
  드러났다** - `cmake/toolchain-x86_64.cmake`가 커널 빌드 전체를
  `-mgeneral-regs-only`로 컴파일해 SSE/x87 명령어 자체를 컴파일러가
  못 내게 막는다(`double` 산술이 있는 함수는 "SSE register return
  with SSE disabled" 컴파일 에러로 즉시 실패, 리턴값뿐 아니라 일반
  산술도 마찬가지 - 실측 확인). `PN-F258698E`(FPU 레지스터 상태
  저장/복원)는 Task 전환 시점의 런타임 메커니즘일 뿐 컴파일러가
  SSE 명령어를 내도 되는지와는 완전히 다른 층위 - §3 원문이 이 둘을
  혼동했다. 이 타겟/파일 단위로 `-mgeneral-regs-only`를 해제하는
  기존 패턴이 이 프로젝트에 없음도 코드 감사로 확인됨.
- **임시 우회(v1, 설계자 확정 전)**: 정수 아닌 JSON number를 double로
  계산하지 않고 원본 텍스트 그대로(zero-copy) `onNumber`에 넘김,
  `JsonWriter`도 `value(double)` 대신 `rawNumber()`(이미 포맷된
  텍스트 삽입)로 대체 - 실제 double 변환이 필요하면 그 제약이 없는
  유저랜드 호출부가 직접 수행.
- **[확정, 2026-09-17, 설계자 답변 QU-6A72AFE6]** ②(진짜 double 지원
  인프라 구축) 채택 - `-mgeneral-regs-only`를 제거하고 설계자가
  과거 커널 개발에 쓰던 플래그 세트(`-ffreestanding -O0 -nostdlib
  -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2
  -fno-exceptions -fno-rtti` 등, 필요한 것만 선별 적용)로 교체 지시 -
  `-mno-sse`류는 `-mgeneral-regs-only`처럼 부동소수점 자체를 완전히
  막는 게 아니라 MMX/SSE/SSE2를 인라인/외부 어셈블러 경로로만 국한
  시키는 것이라, 일반 C++ 코드의 `double` 산술은 컴파일러가 x87로
  처리하게 될 것으로 보인다.
- **[완료, 2026-09-17, commit 586dcfc]** `-mno-mmx -mno-sse -mno-sse2
  -mcmodel=large`로 실제 교체 완료 - **실측으로 제약이 애초 예상보다
  더 정밀하게 좁혀졌다**: x86-64 SysV ABI가 `double` 반환을 항상
  XMM0로 강제하므로, SSE 비활성 상태에서도 "함수가 double을 값으로
  반환"하는 경우만 여전히 컴파일 에러다 - `double`을 매개변수로
  받거나 함수 내부에서 계산하는 것은 x87 명령어(fldl/fadd/fstpl)로
  완전히 정상 컴파일됨을 objdump로 직접 확인. 독립 코드 검증 -
  `json.h`가 이 제약을 정확히 문서화해 뒀고(42-53행 주석), `onNumber`
  콜백/`JsonWriter::value(double)`/`kAppendDouble` 전부 `double`을
  매개변수로만 받고 반환하지 않는 관례로 실제 구현됨(§3 원안대로
  정수+부동소수점 전부 지원 복원, `rawNumber()` 임시 우회는 걷어냄).
  **갭 완전 해소.**
- **부수 발견**: 이 변경과 무관하게, SMP4+실제 3-ELF initrd(init/
  devmgr/pubreg) 조합에서 간헐적으로 여러 코어가 동시에 NMI로
  강제정지되며 Serial 출력이 섞이는 크래시가 발견됐다 - 구 플래그로
  되돌려도 동일 재현되는 사전 존재 버그로 차등 테스트로 확정,
  `PN-907C5289`/`PN-3081704A`(같은 "여러 코어 동시 kPanic" 결함
  클래스로 보이나 init+devmgr+pubreg 3-ELF 조합에서만 남은 잔여
  재현일 가능성)와 관계 확인 필요 - `PN-F7EBD6F5`로 별도 등록, 아직
  미착수(§3 다음 후보로 추가 가치 있음).

- **[신규, 2026-09-17] `DC-21647E46`(커널 전역 포인터 SharedPtr/
  WeakPtr 전환) - 5-Phase 로드맵 전체 완료 확인**: 이 문서 자체가
  이미 매우 상세히 자기 추적돼 있음(과거 이 세션이 갱신한 이력
  포함) - Phase 0-4(PN-E2A114C1/PN-21C2D4E9/PN-B41D8C0E/
  PN-B4987BF6/PN-9CC66142) 전부 completed로 정확히 기록됨. 보안
  관련 핵심 주장(Phase 4의 `kResolveOwnedBridge`가 위조된 bridge
  핸들을 막는다) 하나를 독립 재검증 - `channel.cpp`에 실재하고
  5개 syscall 핸들러 전부(`ChannelRead`/`Write`/`CloseBridge` 등)
  가 이를 통해서만 `args->bridge`를 해석함을 확인. 갭 없음 - 이
  문서가 스스로 관리한 대형 마이그레이션 로드맵이 실제 코드와
  정확히 일치하는 드문 완결 사례.

- **[신규, 2026-09-17] `DC-47000304`(DebugGetRegisters/SetRegisters
  레지스터 스냅숏 위치) - (A)안 채택 후 구현 확인**: (A)(DebugSession
  사본 저장 + write-back) 채택이 `PN-87D6B615`로 정확히 구현됨을
  코드 대조(`kSaveDebugRegistersSnapshot`/`liveFramePtr`/
  `savedRegisters` 전부 `debug_session.cpp`에 실재, write-back 로직도
  확인). 갭 없음. **부수 확인**: 이 문서가 "SMP4 검증은 별개의
  사전 존재 버그(`PN-9F8FF132`) 때문에 실행 못 함"이라고 적어 둔 그
  블로커가 **이후 완전히 해소됨**(부팅 순서 재배치, commit d720d57,
  10/10 SMP4 무결 검증) - DC-47000304 자체가 틀린 건 아니고(그
  시점엔 정확했던 서술), 이제 SMP4 하에서 DebugGetRegisters/
  SetRegisters의 완전한 E2E TEMP 재검증이 가능해진 상태(우선순위
  낮은 후속 기회로 기록만, 새 PN 등록은 보류 - RM-23F4B687 §4).

**[2026-09-17] DC류 문서 후보 풀 소진** - `DC-FB38F86F`/`DC-21647E46`/
`DC-47000304` 전부 점검 완료(갭 없음, 하나는 부수적으로 완화 기회
발견). `DC-235312EF`는 CNW 툴링(git_add_bulk 인코딩) 조사 문서라 이
방법론(설계 문서 vs 커널 코드) 대상이 아님 - 이미 자체 해소됨,
스킵. `DC-48565C0B`/`DC-427BB6B2`/`DC-79A2387A`/`DC-5AB13FFC`는
`DS-D4E5C451`에 이미 통합 반영돼 있고 그 문서 자체를 이번 세션
초반에 이미 상세 점검했으므로 개별 재확인 생략. 나머지 DC는 전부
`archived`(더 이상 인용 대상 아님). 다음 스윕은 새로 `approved`
전환되는 문서(SP-CCACB192처럼 착수와 동시에 갭이 드러나는 경우가
실제로 있었음 - §1-G 참고) 또는 QA류 문서 위주로 전환한다.

- **[신규, 2026-09-18] QA류 문서 점검 (`QA-26450C3E`/`QA-08F8C96F`) -**
  **이 방법론 대상 아님이 확인됨, 갭 없음**: QA 문서는 SP/DC처럼 "확정된
  설계"를 산문으로 선언하는 문서가 아니라 체크리스트 자체라, 이 문서의
  핵심 위험 패턴(목록 뒷부분 항목이 조용히 누락)이 애초에 잘 안 맞는다.
  실제로 대조해보니 두 문서 모두 미체크 항목마다 예외 없이 구체적인
  `PN-XXXXXXXX` 계획 참조가 이미 붙어 있어(`QA-26450C3E`의 BIOS/EFI→
  `PN-7FBF255A`, 인터럽트 라우팅/IPC→`PN-B3DD3D19`, procfs→`PN-48F0F90C`,
  devmgr/fs 실코드→`PN-BD9AAE2F`/`PN-452FF696`, 특권경계 3항목도 같은
  선행조건으로 명시; `QA-08F8C96F`의 부팅시간/컨텍스트스위칭 비용 실측→
  본문 자체가 방법론 미확정을 `CLAUDE.md` 규칙4에 따라 의도적으로 유보)
  "조용히 빠진 것"이 아니라 전부 openly 추적 중 - 갭 없음. 이 두 문서는
  이제 이 방법론의 스윕 대상 풀에서 제외(§3에도 추가 안 함), QA류는
  devmgr/fs 실코드 착수 등 실측 조건이 갖춰질 때 자연히 체크 항목이
  옮겨가는 것으로 충분.

### 1-H. `SP-1FBC0EEB`(Channel IPC) `OpenChannel`/`ConnectChannel`의
`name` 포인터 미검증 - `PN-B552E75F`(Read/Write 소급 적용)와 동일
결함 클래스가 대칭 핸들러 두 곳에 남아 있었음 (코드 갭, **발견 즉시
같은 커밋에서 해소, minicore-88 세션, commit 4eb17f1**)

- **출처**: `PN-B552E75F`(2026-09-16 completed)가 `ChannelReadHandler`/
  `ChannelWriteHandler`의 유저 포인터(`buffer`/`data`)에
  `kValidateUserBuffer()` 검증을 소급 적용했을 때, 같은 파일의
  `OpenChannelHandler`/`ConnectChannelHandler`가 쓰는 `name`/
  `nameLength`(마찬가지로 유저 포인터)는 그 소급 범위에서 빠졌다.
- **발견 경위**: `PN-EAB3A9AE`(libmc Channel IPC syscall 래퍼, 이
  프로젝트 최초의 진짜 유저랜드 Channel 호출부 준비) 착수 중,
  실제 syscall 트랩 검증을 설계하려고 `channel.cpp` 핸들러 전체를
  다시 읽다가 minicore-88이 직접 포착 - 이전까지 모든 Channel
  테스트가 내부 TEMP 스캐폴딩(가짜 UserThread가 핸들러를 직접 호출,
  실제 syscall 인자 검증 경계를 전혀 안 거침)이었기 때문에 잠복해
  있었다.
- **조치**: 발견과 동일 커밋(`4eb17f1`)에서 즉시 수정 - 두 핸들러
  모두에 `PN-B552E75F`와 동일한 `kValidateUserBuffer` 가드 추가
  (`nameLength > 0`일 때만 검사, 이름 없이 여는 `OpenChannel` 경로는
  원래도 `name`을 안 건드리므로 영향 없음). `channel.cpp` 코드로
  독립 확인 완료.
- **의미**: §1-B/§1-E/§1-F가 이미 세 번 확인한 것과 같은 일반 패턴
  ("한 결함 클래스를 한 서브시스템/경로 쌍에서 잡아도, 비슷한 시기에
  독립적으로 다뤄진 대칭 경로에는 그 교훈이 전파 안 될 수 있다")의
  네 번째 사례 - 다만 이번엔 발견부터 수정까지 같은 세션·같은 커밋
  안에서 끝나 **이 문서에 "열린 갭"으로 남은 적이 없다**(§1-B/E/F와
  달리 별도 PN 등록 없이 즉시 해소). 3-B 표준 절차("대칭 경로 쌍은
  양쪽 다 확인")가 실제로 작동을 검증받은 사례로 기록.
- **현재 상태**: 완전 해소. `PN-EAB3A9AE` 자신은 여전히 `scheduled`
  (두 개의 독립 유저랜드 프로세스 간 실제 connect/accept/read/write
  전체 핸드셰이크 실측은 아직 미완료로 명시적으로 남음) - 이건 이
  발견과 무관한 별개의 잔여 범위.

### 1-I. `SP-4DCD0E6A`(Lock-free/Concurrent 컨테이너) §3 `ConcurrentRbtree` -
실제 채택된 동시성 메커니즘이 문서에 반영 안 됨 (문서만 정정 - 코드
갭 아님, 2026-09-18)

- **출처**: 이번 스윕에서 아직 이 방법론이 다루지 않았던 approved
  SP 문서(`SP-4DCD0E6A`)를 처음 대입 - `minicore/kernel/
  concurrent_rbtree.h`(`PN-A8EF29F7`)와 대조.
- **문제**: §3 서두 산문은 `ConcurrentRbtree`의 트리 변경을 "새
  서브트리를 먼저 구성한 뒤 마지막에 원자적 포인터 1회 교체로 게시"
  (RCU식 copy-then-republish)하는 방식이어야 한다고 서술하는데, 바로
  아래 코드 스케치는 `find()`를 그냥 "락 없음"이라고만 적어 재시도
  로직조차 없다(서로 다른 두 이야기가 같은 절 안에 공존). 실제 구현
  (`PN-A8EF29F7`)은 **이 문서의 산문에도 코드 스케치에도 없는 세 번째
  방식**을 채택했다 - 착수 세션이 `RbCore::rotateLeft/rotateRight`가
  원자적 교체가 아니라 CLRS 표준 in-place 다중 필드 mutate임을 코드
  감사로 발견하고 등록한 `QU-B5CA4008`에서, 설계자가 "(A) 읽기
  seqlock류 검증/재시도"를 명시적으로 선택했다 - §3이 서술하는 "원자적
  교체"(선택 안 된 (B))는 채택되지 않았다. 실제 구현은 버전 카운터
  (홀수=쓰기 중)로 `find`/`first`/`next`가 순회 전후 버전을 비교해
  다르면 재시도(`kMaxRetries=64`)하고, `Rbtree::insert/remove`(회전
  포함) 자체는 전혀 수정하지 않는다.
- **부수 발견**: 같은 절이 "`ConcurrentRbtree`/`ConcurrentMap` 둘 다
  §1(`LockFreeList`)과 같은 논리적 마킹 삭제 방식이라 RCU가 노드 회수
  안전성 때문에 필요하다"고도 서술하는데, 실제 `concurrent_map.h`는
  마킹이 아니라 스트라이프 락을 쥔 채 직접 unlink한 뒤
  `Rcu::callAfterGracePeriod()`로 반납만 미루는 방식이고,
  `ConcurrentRbtree`가 RCU를 쓰는 이유도 노드 회수가 아니라 순수
  "읽기 도중 이 코어의 선점을 막는" 보조 용도다(이 컨테이너는
  `Rbtree`와 동일하게 `T`의 메모리를 전혀 소유/회수하지 않는 완전
  침습형이라 애초에 그런 회수 자체가 없음).
- **조치**: `SP-4DCD0E6A`에 정정 각주 추가(원문 보존) -
  `document_patch`로 §3 `ConcurrentRbtree` 코드 스케치 직후와 "착수
  불가" 문단 뒤 두 곳에 실제 채택된 seqlock 방식/RCU의 실제 용도를
  명시. 코드 쪽은 이미 `concurrent_rbtree.h` 자신의 클래스 주석이
  정확하고 정직하게(잔여 위험까지) 기록해 뒀으므로 조치 불필요 -
  **문서만 낡아 있던 것.**
- **현재 상태**: 완전 해소(문서 정정).

- **[신규, 2026-09-18] `SP-F146B7F8`(TLS/PerCpu 인프라)**: §1
  (`ThreadLocal<T>`/`TlsRegistry`)/§2(`PerCpu<T>`) 전부 `tls.h`/
  `percpu.h`와 한 줄씩 대조 - `Task::tlsSlots[kMaxTlsSlots=16]`
  (task.h:18/239), `ThreadLocal<T>::get/set`이 `Scheduler::
  currentTask()->tlsSlots[_slot]`를 그대로 씀, `PerCpu<T>::get/
  forCore`가 `_values[kAcpiMaxCpus]`+`Scheduler::currentCoreIndex()`
  를 그대로 씀 - 설계 스케치와 정확히 일치. 유일한 배치 차이
  (`PerCpu<T>`를 문서 제안 `libkenv` 대신 `kernel`에 둔 것)는 이미
  `percpu.h` 자신의 주석이 이유(get()이 kernel:: 의존 유발)까지 함께
  정확히 기록해 둠 - 숨은 갭 아니다. §2.4-1(rdtscp 기반
  `currentCoreIndex()` 전환)도 `scheduler.cpp`의 `gRdtscpSupported`
  분기(rdtscp 성공 시 그 결과, 아니면 `kScanCoreIndexByApicId()`
  폴백)로 정확히 구현돼 있음을 확인. **갭 없음.**

- **[신규, 2026-09-18] `SP-FAF768AB`(제네릭 컨테이너 템플릿 -
  Node/List/Vector/Rbtree/RbMultiTree/Map/OrderedList/LruList/Queue)**:
  이 문서는 이미 자체적으로 §6-A("착수 세션 실측 발견 - 원안 코드의
  실제 버그 2건")를 갖고 있어 위험 지대로 보고 정밀 대조했다 -
  `minicore/libs/libkcont/{intrusive_list.h, vector.h, rbtree.h,
  map.h}` 확인 결과, §6-A가 스스로 기록한 두 수정(①`List::init()`이
  `_sentinel = Node{}`(댕글링 유발) 대신 `_sentinel.prev/next = 
  &_sentinel` 개별 대입, ②`Rbtree`/`RbMultiTree::remove(T*)`가 원안의
  `static`이 아니라 인스턴스 메서드로 `_root` 갱신)가 정확히 코드에
  반영돼 있음을 직접 확인. `List`/`OrderedList`/`LruList`/`Queue`
  네 타입 전부 (파일 경로 주석이 "또는 별도 queue.h" 등으로 이미
  유연하게 열어 뒀던 대로) `intrusive_list.h` 한 파일에 통합 배치돼
  있고, `RbMultiTree`는 `rbtree.h`에 실재, `Vector<T, Policy>`의
  `DefaultContainerPolicy`/`moveElement`/`destroyElement` 훅도
  설계 그대로 구현돼 있음을 확인. **갭 없음** - 이 문서로 approved
  상태였으나 아직 이 방법론이 안 다뤘던 SP 문서 3건(`SP-4DCD0E6A`/
  `SP-F146B7F8`/`SP-FAF768AB`)을 전부 소진했다.

- **[신규, 2026-09-18] `SP-7CC5693A`(VFS 커널 서브시스템) §2.1/§2.2/§2.5 -**
  **PN-452FF696이 방금 구현한 실코드와 즉시 대조**: 이전까지는 §4-A/§9.1의
  교차 참조로만 언급되고 이 방법론이 직접 대입한 적은 없었던 문서 -
  `PN-452FF696`(VFS Mount/Unmount/ResolvePath/SignalUserlandReady/
  WaitForUserlandReady syscall 구현, commit 69d7fa9)로 처음 실코드가
  생겨 바로 대조했다. `minicore/kernel/mount_table.h`(§2.1)의
  `MountKind`/`MountEntry`/`MountTable::resolve/mount/mountKernel/unmount`
  전부 pseudocode와 정확히 일치(최장 접두사 일치 + `/` 경계 처리까지),
  `KernelFsDriver`가 `AsyncTaskHandler` 상속 형태(§2.1 2026-09-17 개정판)로
  구현된 것도 확인. `minicore/kernel/vfs_syscall.h/.cpp`(§2.2/§2.5)의
  `MountArgs`/`UnmountArgs`/`ResolvePathArgs`/`SignalUserlandReadyArgs`/
  `WaitForUserlandReadyArgs`와 5개 syscall endpoint(그룹3, `RM-48E1E610`
  갱신과 일치) 전부 문서 그대로. `SignalUserlandReady`의 "커널 전역 단
  1회만" 요구사항도 `AtomicU32::compareExchange(0,1)`로 정확히 구현.
  `ResolvePathArgs`가 `MountKind::KernelDriver`를 만나면 `NotSupported`로
  응답하는 것도 §2.1/§9.1이 이미 "§9 착수 시 확정"으로 열어 둔 것과
  일치(임의 결정 아님, CLAUDE.md 규칙4 준수를 코드 주석이 직접 인용).
  **갭 없음** - §3(드라이버 우선순위)/§9(표준 파일 API)는 이 커밋의 의도된
  범위 밖(코드 주석이 스스로 명시)이라 대상 아님, 다음 착수 시 재대조.
  minicore-88이 직접 검증 코드 안에 RM-F2DAFF66 §1-B/E를 인용해 이번
  핸들러가 그 결함 클래스와 무관한 이유까지 남겨 둔 점도 특기할 만함 -
  이 방법론의 교훈이 구현 단계에서 실제로 참조되고 있다는 방증.

**[2026-09-18] approved SP 문서 후보 풀 재소진** - document_list
전수 재대조로 찾아낸 미점검 approved SP 문서 3건을 전부 처리(1건
갭 발견/정정, 2건 갭 없음). 다음 스윕은 새로 approved 전환되는
문서 위주로 계속한다(SP-30FCC8AE(사용자/권한 체계)가 review에서
approved로 넘어가면 유력 후보 - 아직 review 상태라 대상 아님).

- **[신규, 2026-09-18] `SP-2AAD7C8D` §9(표준 파일 API) - `PN-EA4EE935`**
  **(Open/Close/Read/Write, KernelDriver 경로)와 즉시 대조**: §9.1이 이미
  스스로 "비판적 재검토로 발견한 공백"(`MountKind::KernelDriver`가 §9.2
  `FileDescriptor`에 원래 없던 판별자를 요구)으로 명시적으로 열어 둔
  자리 - `PN-ABD23ACE`(전신, PL-FC38956C의 후속) 항목2가 그 요구사항을
  이어받았고, 이번 커밋(`vfs_syscall.h/.cpp`+`process.h`)이 정확히 그대로
  구현했다. `Process::FileDescriptor::kind`(MountKind, process.h:231)가
  §9.1이 예고한 확장 그대로 실재 - 심지어 그 옆 주석이 `PN-CE6A04AB`(이
  문서가 이전에 다룬 Channel 핸들 위조 방지 보안 패턴, §2 참고)를 직접
  인용해 "임의의 정수를 그냥 믿지 않는다"는 같은 원칙을 재사용했음을
  밝혀 둠 - 이 감사 문서의 과거 발견이 실제로 후속 설계에 참조되는
  사례. `OpenArgs`/`CloseArgs`/`ReadArgs`/`WriteArgs`(vfs_syscall.h)
  전부 §9.3과 필드 단위로 일치, offset 소유권도 §9.2 그대로("offset은
  커널(fd 테이블)이 갖고 FileSystemDriver::read/write는 매번 명시적
  offset을 받는 무상태 오퍼레이션" - 코드가 `slot->value.offset +=
  bytesRead/bytesWritten`로 정확히 구현). `MountKind::Channel` 마운트는
  §9.1이 스스로 "아직 미확정, 착수 시점에 정한다"고 이미 열어 둔 대로
  `NotSupported`로 정직하게 응답(임의 결정 아님, CLAUDE.md 규칙4 코드
  주석 직접 인용) - 숨은 갭 아니다. **부수 발견**: 실측 중
  `submitterTask`를 내부 재제출 AsyncTask(KernelFsDriver 대상)에
  전파하지 않으면 `ProcFs::open()`의 "proc/self" 해석이 실패하는 실제
  버그를 찾아 4개 핸들러 전부에 전파 코드를 추가해 수정 -
  `async_task.h`의 `submitterTask` 문서 주석이 이미 예견해 둔 확장
  지점이었음을 커밋이 스스로 인용. **갭 없음** - 설계가 스스로 예고한
  공백이 정확히 그 설계 의도대로 메워진 사례(SP-9525C4C0/§1-F와 같은
  급 - 원안의 "확인 필요" 각주가 후속 세션에 정확히 전달돼 작동함).
  **[추가, 2026-09-18] `PN-238FD331`(Stat, §9.3/§9.4 call 10)도 같은 날
  후속 커밋으로 확인** - fd 없이 경로만으로 동작하는 것까지 §9.4
  규칙 그대로(ResolvePathHandler와 거의 동형), MountKind::Channel은
  Open과 동일한 스코프 결정으로 NotSupported 유지. 갭 없음, 같은
  패턴의 반복이라 별도 하위 절 없이 여기 한 줄로만 추가 기록.

- **[신규, 2026-09-18] `SP-B071E628` §6-6(pubreg register/query 완전
  바이너리) - 확정 직후 `PN-185406F6` 항목4(commit 974adce) 구현과
  즉시 대조**: 설계가 여러 차례(§6-4 절충안 반려→§6-5→QU-4B38857C
  답변→§6-6 확정) 급하게 뒤집힌 직후 착수된 구현이라 이 문서 방법론이
  가장 주목해 온 위험 패턴("승인 직후/막 구현된 문서")에 정확히
  해당 - `userland/libs/libmc/pubreg.h`의 `PubregRegisterRequest`/
  `PubregRegisterAck`/`PubregQueryRequest`/`PubregRegistrationEntry`
  전부 §6-6의 `PubregRegistration` 필드(registryId/protocolCode[4]/
  implementationId[28]/endpoint/featureFlags)와 정확히 일치,
  `PubregEndpointKind`(Channel/Tcp/Udp) discriminated union도 설계
  그대로. `minicore/pubreg/main.cpp`의 `kHandleRegister`/`kHandleQuery`가
  이 와이어 포맷을 정확히 그 레이아웃으로 파싱/조립함을 확인 -
  query의 mode 0(전체)/1(substring, `kImplementationIdMatches`)과
  offset/count 페이지네이션도 QU-E05A55AD 1번 답변 그대로. "닫힌
  파이프 → 등록 자동 해제"(`kReleaseRegistrationsOwnedBy`)도 §3/§6
  원 설계 그대로 보존. `PN-10EE096A`가 막 노출한
  `waitAnyForMultipleSyscall` 위에서 accept+다중 연결을 멀티플렉싱하는
  것도 그 syscall의 의도된 최초 소비처로 정확히 맞물림. **정직하게
  기록된 v1 단순화 하나**(코드 갭 아님, 설계 문서가 와이어 포맷까지만
  다루고 버퍼링 정책은 구현 세부로 남겨 둔 영역) - 한 `ChannelRead`가
  메시지 2개 이상을 한 번에 받아오면 첫 메시지만 처리하고
  `bytesBuffered`를 무조건 0으로 리셋해 나머지를 버린다(주석이 직접
  인정 - "다음 메시지 조각을 잃지 않으려면 별도 스크래치 필요, v1은
  파이프라이닝을 포기"); 이 프로토콜이 요청-응답 왕복이라 정상
  클라이언트는 응답 전 다음 메시지를 안 보내므로 지금은 안전하나,
  다중 메시지 파이프라이닝이 실제로 필요해지면 재검토 대상. **갭
  없음** - 설계 확정부터 구현까지 빠르게 이어졌음에도 와이어 포맷/
  페이지네이션/자동해제/신규 syscall 소비 전부 정확히 일치한 사례.

- **[신규, 2026-09-18] `PN-C39882D0`(pid ABI 마이그레이션) -
  `kFindDebuggableChild()` 놓침(코드 갭, 발견 즉시 같은 세션에서
  해소, commit 7d683d1)**: `PN-C39882D0` 자신의 문서 주석이 "이
  마이그레이션은 SpawnProcess/Wait만 다룬다 - Kill은 raw-pointer
  비교를 의도적으로 유지한다"고 명시적으로 예외를 하나만 적어 뒀는데,
  `debug_session.cpp`의 `kFindDebuggableChild()`는 그 예외 목록에
  없었음에도 여전히 옛 raw-pointer 비교를 쓰고 있었다 - 의도된
  과도기적 예외가 아니라 마이그레이션이 단순히 놓친 파일. §1-B/E/F/H가
  이미 세 번 이상 확인한 "한 결함 클래스를 고칠 때 비슷한 시기의
  다른 경로가 빠질 수 있다" 패턴의 또 다른 사례 - 이번엔 "대칭 핸들러
  쌍"이 아니라 "마이그레이션의 선언된 범위 vs 실제 커버리지" 형태.
  **영향**: 실제 `SpawnProcess`가 반환하는 `ProcessId`를 그대로
  `DebugAttach` 등에 넘기면 이 비교가 항상 실패해 `PermissionDenied`만
  반환 - PN-87D6B615의 모든 이전 TEMP 검증은 합성 `Process`+
  raw-pointer 값 조합만 써서 이 버그를 가리지 못했다(실측으로 실제
  경로를 탄 적이 없었다는 뜻). `child->processId == targetProcessId`로
  수정, 실측(4242 vs 9999 시뮬레이션 값)으로 확인. **갭 완전 해소.**
  minicore-88이 PN-87D6B615 본문에 이미 매우 상세히 자체 기록해 둠 -
  이 항목은 교차 참조용 짧은 기록.

- **[신규, 2026-09-18] `PN-49C2F890`(#DB 브레이크포인트 즉시 Blocked
  전환, commit `00fb1e7`) - 설계 대 코드 즉시 대조**: 이 세션이
  QU-396C2692(즉시 블록 지시)/QU-8172431E(IST4 공유 스택 위험, 코어당
  동시 파킹 1개 제한 정책 확정)를 직접 relay했던 바로 그 결정이
  구현까지 완료된 걸 확인해 바로 검증 - `debug_session.cpp`/`idt.cpp`
  diff를 한 줄씩 대조. `kHandleUserBreakpointHit()`이 `gDebugParkedOnCore
  [coreIndex]`가 비어 있을 때만 `Scheduler::parkCurrent()`를 직접
  호출해 즉시 파킹(정확히 지시된 메커니즘), 이미 서 있으면 기존
  지연 경로(pausedByDebugger만 세움)로 안전하게 대체 - QU-8172431E
  "코어당 동시 파킹 1개 제한" 그대로. `coreIndex`가 파킹 시점에
  지역변수로 캡처돼 있어, 재개가 로드밸런싱으로 **다른 코어**에서
  일어나도(idt.cpp 주석이 직접 명시) `gDebugParkedOnCore[coreIndex]
  = false`가 여전히 "원래 그 IST4를 점유했던" 코어의 플래그를
  정확히 내린다 - 실행 중인 코어가 아니라 점유 대상 코어 기준으로
  풀리는 것이 맞는 설계. `Scheduler::onTick()`의 기존
  `kIsPausedByDebugger()` 지연 경로(scheduler.cpp)는 제거되지 않고
  "두 번째 동시 히트" 케이스의 fallback으로 의도적으로 남겨졌다 -
  이 세션이 relay에서 "제거해도 되는지 판단 필요"로 남겨 뒀던 질문에
  구현 세션이 "유지"로 정확히 답한 셈. `kSaveDebugRegistersSnapshot()`
  도 파킹 직전으로 이동해 `DebugGetRegisters`/`SetRegisters`(§3.5)
  호환 유지. DR6 클리어를 콜백 호출 **전**으로 옮긴 동반 수정도
  재개가 다른 코어에서 일어날 수 있다는 것과 정확히 같은 근거로
  정당함(자체 발견/자체 수정, 새 설계 결정 아님 - RM 항목 대상
  아니지만 감사 과정에서 근거까지 확인). QEMU devmgr 브레이크포인트
  실측 + GRUB SMP4 스트레스 무회귀 대조까지 완료. **갭 없음.**

- **[신규, 2026-09-18] `SP-9A6D579F`(DebugSession, approved) - §목차
  나열형 대조 완료**: `PN-87D6B615`가 항목1(자료구조)/항목2(부모-자식
  권한, `kFindDebuggableChild`/`submitterTask.lock()` 사용 확인)에
  더해, 이번 세션이 직접 relay·감사한 항목3-8(브레이크포인트 설정/
  싱글스텝/정지-재개/레지스터 조회-설정/메모리 읽기-쓰기, `RM-48E1E610`
  그룹7 call0-8 전부 "구현 완료")까지 전부 단일 스레드 기준으로
  완료·검증됐다. **유일하게 남은 것은 §1-A(멀티스레드 디버깅)**인데,
  이는 설계 문서 자신이 처음부터 "프로세스가 여러 스레드를 가질 수
  있는 인프라 자체가 없다"는 선행 조건 부재를 이유로 별도 계획
  `PN-2E4E9D79`로 명시적으로 분리해 뒀다 - 숨겨진 누락이 아니라 처음부터
  openly 추적된 후속 과제.

  **[갱신, 2026-09-19]** 그 선행 조건(`PN-2E4E9D79`)은 `SP-76250478`→
  `PN-0EB2FABF`(멀티스레드 유저 프로세스 지원, 4단계 전부 완료 -
  commit b0f2753/02b2702/fd01642/5ee0fec)로 완전히 흡수돼 `completed`로
  전환됐다 - 단 `PN-2E4E9D79`가 다룬 범위는 "스레드 인프라 자체"뿐이고
  §1-A가 요구하는 디버깅 전용 구현(targetThread 파라미터 추가,
  DebugSession 스레드별 재설계, kSyncDebugRegs 경합 재검토 등)은
  포함하지 않았다. 그 실제 구현 작업은 새로 등록한 `PN-06A7C439`
  ("멀티스레드 디버깅 구현 - SP-9A6D579F §1-A/§3.4/§3.5", `PN-0EB2FABF`에
  의존)가 추적한다. **갭 없음**(§1-A 범위를 제외한 나머지 전부 -
  §1-A 자체는 이제 `PN-06A7C439`에서 실제 구현 진행 중).

- **[신규, 2026-09-18] `SP-8D206F11`(CPU 캐시 관리 정책, review→approved) -
  §2 전체 대조 완료**: 이번 세션이 §2.2를 코드와 대조하다 "AP 코어도
  kMain 경로를 타 PAT MSR이 자동 적용된다"는 전제가 실제로는 틀렸음을
  발견(AP는 별도의 `kApMain`을 타고 `Paging::init()`을 거치지 않음) -
  `QU-9F758912`로 등록해 설계자가 "PAT MSR 설정을 kApMain에도 추가해"로
  직접 확정. `PN-310C870F`(commit `1a5cb29`)가 그 답변 그대로
  `Paging::initPatForThisCore()`를 BSP(`kMain`)/AP(`kApMain`) 양쪽에
  배선하고 문서 §2.2/§2.4(기존 PCD 사용처가 실제로는 인덱스2가 아니라
  인덱스3이라고 잘못 적혀 있던 것)까지 함께 바로잡아 구현 완료 - 실제
  init/devmgr/fs/pubreg initrd SMP4 15회 반복(AP 3개 전부 정상 기동)
  무회귀 확인. **갭 없음(§2 전체)** - §3(하드웨어 캐시 스누핑 여부)은
  여전히 실사용처 없어 열린 질문으로 남아 있으나 이는 설계 문서 자신이
  명시적으로 미뤄 둔 범위라 갭이 아님.

- **`SP-245D130B`(ResourceGroup) §10 "요약" 절**: "지금 바로 설계+구현
  가능"(§1 트리/§2 Process 연결/§3 CPU 쿼터 스로틀/§4 freeze/§5 CPU
  계정) vs "설계만, 구현은 후속"(§6 메모리)/"전면 보류"(§7 I/O)로
  나눈 분류를 `PN-4190BBD3`(구현 계획)과 한 줄씩 대조 - §1/§2/§4는
  실제로 completed+실측 검증됨(2026-09-18, 3-ELF initrd로 루트 그룹
  가입 경로/`fork()` 경로 둘 다 확인), §3/§5/§6(syscall 번호 등록)은
  선행 조건이던 `SP-6A563A8F` 승인이 끝나 "착수 가능"으로 정확히
  갱신돼 있음, §6/§7은 문서 자체가 이미 "후속 PN"으로 명시적으로
  분리해 둠(§7은 `PN-DEC738B8`, §6은 `PN-A40787C8`로 각각 등록 확인).
  갭 없음 - 이 문서는 이미 여러 차례(2026-09-17/18) 자체 개정을 거쳐
  §목차-코드 대조가 실질적으로 상시 반영돼 있는 드문 사례.

- **[갱신, 2026-09-20] `SP-2BCE5D60`(fs 커널 서비스) - 실코드가 생겨
  재방문, §3.0만 대상(나머지는 이미 PN-452FF696이 정확히 추적 중) -
  갭 없음**: `minicore/fs`(main.cpp/ahci.h·cpp/block_device.h)가
  이제 실제로 존재한다(`PN-F60E405A`) - §3.0 `BlockDevice` 추상
  인터페이스(`blockSize`/`blockCount`/`readBlocks`/`writeBlocks`/
  `flush`/`trim`)를 `minicore/fs/block_device.h`와 한 줄씩 대조 -
  시그니처 전부 정확히 일치(가상 소멸자만 의도적으로 생략 - 이유는
  파일 자체 문서 주석에 기록, freestanding 툴체인 `operator delete`
  부재). `AhciBlockDevice`가 그 구현체로 §3.0이 지목한 그대로 존재.
  §3.1(`FileSystemDriver`)/§4(`SwapBackend`)/§5.1(`mtab`)은 여전히
  코드가 없다 - 그런데 이건 "확정된 설계가 조용히 누락"된 게 아니라
  `PN-452FF696`(fs 서비스 계획, 항목5)이 "libext4/libvfat 자체
  부재"로 정확하고 최신 상태로 이미 추적 중인 항목과 정확히 일치 -
  숨은 갭 아님. **§3.0만 놓고 보면 갭 없음, 나머지는 이미 openly
  추적 중이라 이 문서에서 새로 할 일 없음.**

- **`SP-B071E628`(pubreg) §6-6 최종 바이너리 와이어 포맷**:
  `PubregRegistration`(registryId/protocolCode[4]/implementationId[28]/
  endpoint/featureFlags)/`PubregMessageHeader`/query의 mode/offset/count
  페이지네이션을 `minicore/pubreg/main.cpp`와 `userland/libs/libmc/
  pubreg.h`와 한 줄씩 대조 - 예외 없이 설계 그대로 구현돼 있음을
  확인(`PN-185406F6` 항목4, completed, commit `974adce`). `RM-085694F8`
  ("Minicore Pubreg 프로토콜 할당표")의 할당 현황이 여전히 "아직
  없음"으로 남아 있는 것도 실제로는 정확하다 - `PN-185406F6` 검증에
  쓰인 protocolCode는 TEMP 임의값(devmgr 클라이언트 테스트용, 검증 후
  `git checkout --`로 완전히 원복)이었을 뿐이고, 이 표에 실제로
  영구 예약해야 할 "진짜 프로토콜 소비자"는 §5-A/§5-B 확정(커널
  서비스는 pubreg에 직접 등록 안 함, 대행 릴레이는 범위 밖)에 따라
  아직 하나도 존재하지 않는다 - 갭 없음.

- **[점검 완료, 2026-09-19, 갭 없음] `SP-201238BB`(lock-free
  SharedPtr/WeakPtr/UniquePtr 템플릿) → `minicore/libs/libkenv/shared_ptr.h`**:
  §2(`ControlBlockBase`/`ControlBlock<T,Deleter>`/`SharedPtr<T,Deleter>`/
  `WeakPtr<T,Deleter>`/`EnableSharedFromThis<T>`/`kMakeShared`)와
  §2-A(`UniquePtr<T,Deleter>` - `release()`/`reset()`/`initRaw()`/
  `operator[]` 포함)가 문서 코드블록과 실제 파일에서 API 일치 확인.
  §2-B(`IntrusiveControlBlock`)는 코드에 없지만 갭이 아니다 - 파일
  자신의 헤더 주석(shared_ptr.h:31-33)이 "`PN-68871BC9` 자신이 v1
  적용 대상 미정이라 범위 밖으로 명시해 둔 항목이라 이 파일에는
  포함하지 않는다"고 밝혀, 착수 당시 이미 확정된 의도적 범위 축소임을
  코드 스스로 기록해 뒀다(`SP-30FCC8AE`의 ancestor-scope 결정,
  `PN-88E62419`와 같은 패턴 - 문서가 침묵한 게 아니라 코드가 대신
  설명). §4.3("구체적 후보 목록")은 문서 자신이 "이 문서는 이 후보들을
  강제하지 않는다"고 명시한 비구속 후보 나열이라 이 감사 방법론(확정된
  설계 vs 실제 코드) 대상이 아님 - 대조 불필요. `<type_traits>`
  베어메탈 부재로 인한 `kIsBaseOf` 자체 구현 대체도 문서 본문에 이미
  인라인 정정으로 기록돼 있어 별도 갭 아님.

### 1-N. `SP-04EE2A18`(Syscall 디스패치) 본문이 자기 자신의 후속 Q&A
결정을 반영 안 하고 낡은 채로 남아 있었음 (문서만 정정 - 코드 갭 아님,
2026-09-19)

- **출처**: RM-F2DAFF66 정기 점검 중 `SP-04EE2A18` 감사.
- **발견**: 이 문서 자신의 "유저랜드 ABI" 절이 "`waitForMultipleSyscall`/
  `waitAnyForMultipleSyscall`용... 아직 범위 밖 - 필요해지면 별도
  질의"라고 서술하고 있었으나, 실제로는 **이 문서 자신에 대한
  후속 질의(QU-31402585/QU-F475C6C2, 2026-09-14 설계자 답변)로 이미
  결정되고 구현까지 끝난 상태**였다 - `UserThread::pendingSyscalls`
  (syscall.h)가 단일 필드가 아니라 `ChunkedList<PendingSyscall, 10>`
  로, 한 스레드가 동시에 여러 syscall을 제출/대기할 수 있게 구현돼
  있음을 코드로 확인. "커널 진입 흐름"/"완료·응답 흐름" 절도 여전히
  단수 `pendingSyscall`(단일 슬롯, `valid` 플래그)로 서술해 실제
  복수형 리스트(+`ChunkedList::Slot::used`로 소유권 표현) 구현과
  어긋나 있었다. 부수적으로 `SyscallRegistry`의 API 스케치도
  `AsyncTaskHandler* resolve(id)`로 적혀 있으나 실제로는
  `bool resolveSubjectCode(id, AsyncTaskSubjectCode*)`로 구현돼 있음을
  확인(설계 의도 자체가 어긋난 것은 아니고, `AsyncTask::submit` 경유
  디스패치 관례에 맞춘 자연스러운 시그니처 조정).
- **성격**: 코드 갭이 아니라 **문서가 자기 자신의 승인된 후속 결정을
  본문에 소급 반영하지 않은** 경우 - RM-23F4B687 §5의 "같은 설명이
  다른 문서에도 복제돼 있는지 의심" 원칙이 여기서는 **같은 문서
  안에서** 재현된 사례(앞부분 "확정된 설계" 서술과 뒷부분의 개정
  각주가 서로 모순된 채 공존).
- **조치**: `SP-04EE2A18` 본문에 두 군데 인라인 정정 각주 추가
  완료(2026-09-19) - `document_patch`.
- **현재 상태**: 완전 해소(문서 정정).

### 1-O. `SP-0666DB3C` §4.4 - "`Process::killPending` 플래그" 서술이
실제 구현과 다름 (문서만 정정 - 코드 갭 아님, 2026-09-19)

- **출처**: RM-F2DAFF66 정기 점검(오늘 `PN-59A60413`에서
  `kCheckSignalCheckpoint()`를 직접 수정한 김에 그 설계 원본인
  `SP-0666DB3C` §4를 대조).
- **발견**: §4.4가 "`SignalNumber::Kill`은 `Process::killPending`
  플래그로 모든 체크포인트가 공통으로 확인해 최대한 빨리 처리한다"고
  서술하지만, 실제 코드베이스 전체에 `killPending`이라는 필드/이름이
  존재한 적이 없다(grep 확인). 실제 구현은 `Kill`도 다른 모든 신호와
  완전히 같은 통합 경로(`pendingSignals` 순회 + `dispositions[]`
  확인)를 탄다 - `Kill`/`Stop`은 `SignalActionHandler`가 애초에
  `Ignore` 전환을 거부해(마스킹 불가 원칙) 그 값이 항상 `Default`로
  고정되므로, 결과적으로 문서가 의도한 "최대한 빨리 처리"와 동일한
  효과를 전용 플래그 없이 범용 메커니즘으로 달성한다.
- **성격**: 코드 갭이 아니라 구현 단계에서 "전용 플래그" 대신 "이미
  있는 범용 메커니즘 재사용"으로 자연스럽게 더 단순하게 수렴한
  경우(오히려 이 통합 덕에 `Terminate`의 `Ignore` 마스킹까지 부가로
  올바르게 동작 - `PN-59A60413` 참고) - 설계 의도 자체가 어긋난 게
  아니라 문서의 구현 스케치가 낡았을 뿐.
- **조치**: `SP-0666DB3C` §4.4에 인라인 정정 각주 추가 완료
  (2026-09-19).
- **현재 상태**: 완전 해소(문서 정정).

- **[점검 완료, 2026-09-20] `SP-CC1CF30E`(authmgr) - 코드 자체가 아직
  없어 대조 불성립, 갭 없음**: `minicore/authmgr`/`minicore/libs/
  libkvdb`/`minicore/libs/libkcrypto` 전부 디렉터리 자체가 없음(glob
  확인). `PN-24A2B6F5`(계획)가 이 문서의 §1-B~§1-D 전 항목(sudo/su,
  libkvdb 스키마, libkproto 추출 순서, 다중 그룹, 장애 정책)을 이미
  정확하고 최신 상태로 추적 중 - SMP E2E를 막던 위험 요인
  (`PN-9A5C0FC2`/`PN-907C5289`/`PN-3081704A`)도 전부 해소돼 "착수
  가능" 상태로 정확히 갱신돼 있음까지 확인. `SP-2BCE5D60`(fs, 이
  문서의 이전 갱신 항목)과 같은 패턴 - 순수 설계 단계라 코드 갭이
  성립하지 않는다. 착수 자체는 새 커널 서비스+라이브러리 3개를
  아우르는 대규모 작업(다수의 "착수 세션이 구체화" 표시가 있는
  하위 결정 포함)이라 이 감사 틱의 범위 밖 - 실제 착수는 별도
  세션에서 신중하게.

### 1-P. `SP-ECC59BAE`(Running Task 강제 이관) - `onForcedMigration()`이 `onTick()`과 달리 freeze/디버그 정지 검사 없음 (코드 갭, `PN-6CE4DD35`로 등록, 시급성 낮음)

- **출처**: RM-F2DAFF66 정기 점검 중 `SP-ECC59BAE`를 처음 대입.
  메커니즘(§2-4)은 오늘(2026-09-20) `PN-81E49523` 2단계의
  `kContextSwitchFromISR` 통일까지 정확히 함께 반영되어 있음을
  확인(`Scheduler::onForcedMigration()`, scheduler.cpp:1787~) - 그
  자체는 갭 없음.
- **발견**: `onForcedMigration()`의 재삽입 분기가 `onTick()`과 달리
  `kCheckAndMarkFrozen()`(ResourceGroup freeze)/`kIsPausedByDebugger()`
  (디버그 정지) 검사를 안 한다 - 코드 자신이 "[알려진 갭,
  2026-09-17]" 주석으로 이미 정직하게 남겨 둔 항목이었으나 PN으로
  승격된 적은 없었다(CLAUDE.md 규칙 7 위반 상태로 방치돼 있었음).
- **위험도**: 낮음 - `Scheduler::requestForcedMigration()`은 현재
  호출부가 전혀 없다(`SP-ECC59BAE` §5가 (C)안 채택 - 자동 트리거
  없음, 수동/진단 API만). 실제 소비자가 생기기 전까지 위험이 발현될
  경로 자체가 없다.
- **조치**: `PN-6CE4DD35`로 등록(우선순위 낮음, `requestForcedMigration()`에
  실제 호출부가 생기는 시점과 함께 처리 권장).
- **현재 상태**: 갭 등록 완료, 미해소(추적은 `PN-6CE4DD35`로 이관).

- **[점검 완료, 2026-09-20] `SP-5130284C`(인터럽트 컨텍스트 SharedPtr
  소멸 지연 메커니즘, approved)** - §7 착수 순서 5단계 전부 코드로
  확인됨: `gInterruptDepth[kAcpiMaxCpus]`+isr.S 증감 배선(1단계),
  `ControlBlockBase::_deferredNext`+`kPushDeferredDestructionImpl`/
  `kDrainDeferredDestructions`(deferred_destruction.cpp, 2단계),
  `releaseStrong()` 통합(§3.2-a 정정대로 `_destroyOwned`/
  `releaseWeak` 둘 다 지연, 3단계), `AsyncReactor::drainOnce()` 드레인
  호출(4단계), 표준 회귀 3종(5단계) - 전부 갭 없음. 다만 §3.2의
  "`isr_common_epilogue`에 감소를 걸면 충분"이라는 서술이 실제로는
  세 경로(자연 복귀/`kContextSwitchFromISR`/`kResumeForkedRing3`)로
  나뉘어 처리된다는 걸 놓치고 있어 문서에 §3.2-c로 보강 완료(코드
  갭 아님 - 실제 구현은 처음부터 세 경로를 정확히 구분해 뒀음, 이
  세션이 직접 작성한 코드라 대조 확인). §6의 "수정 전/후 스트레스
  재현" 항목은 `PN-4137C88C`가 이미 "실측 시도 안 함"으로 정직하게
  기록해 둔 별도의 QA 갭이라 이 문서에 중복 등록하지 않는다.

- **[점검 완료, 2026-09-21] `SP-677210E6`(TSS/IST 예외 스택 서브시스템,
  approved)** - GDT/TSS 확장(코어별 TSS 디스크립터 슬롯)/IST1-4 슬롯
  배정(#DF/NMI/#MC/#DB)/`Gdt::init()`·`reloadOnThisCore()`·
  `loadTssForThisCore()` 코어별 초기화 흐름을 `gdt.h`/`gdt.cpp`와
  대조 - 설계 그대로 구현됨(기존 완료 기록과 일치). 이 문서 후반부의
  "NMI 활용"(워치독+디버그 강제 정지)/"#MC 상세 설계"/"#DB 상세 설계"
  세 확장 절(전부 `PN-F443FE73` 귀속)까지 실제 코드와 전수 대조:
  `Nmi::send`/`Nmi::reasonForThisCore`/`Nmi::stopAllOtherCores`
  (`nmi.h`/`nmi.cpp`, 설계의 `kSendNmi`/`gNmiReason`를 클래스로 캡슐화한
  것 - 이름만 다르고 동작은 설계 그대로), `kPanic`의 두 진입점
  (`panic.cpp`의 `kPanic(const char*)`, `idt.cpp`의
  `kPanic(InterruptFrame*)`) 둘 다 `Nmi::stopAllOtherCores()`를 실제로
  호출함을 확인, `Scheduler::onTick()`의 `gHeartbeat[]`/
  `gHeartbeatLastSeen[]`/`gWatchdogTriggered[]`/
  `kWatchdogCheckIntervalTicks=100` 워치독 구현도 설계 그대로(
  `scheduler.cpp:183-197,1539,1562-1584`). `kHandleMachineCheck()`
  (idt.cpp:408)도 MCG_CAP 뱅크 수 순회+UC/PCC 비트 판정+MCG_STATUS
  클리어까지 설계 스케치와 일치(`kReadMsr`/`kWriteMsr` 파일-로컬 중복을
  `arch::kReadMsr64`/`kWriteMsr64`로 통합하라던 "사소한 정리" 권고사항도
  이미 반영돼 있음). `kHandleDebugException()`(idt.cpp:362)도 DR6 판독
  +`gDebugCallback` 위임+DR6 클리어까지 설계 그대로, 실제 소비자
  (`kHandleUserBreakpointHit`, PN-81E49523/PN-EA968DF0)가 이미 이
  콜백 슬롯에 연결돼 있음도 확인.
  - **사소한 관찰(갭 아님)**: 설계 스케치의 `kHandleMachineCheck`
    의사코드는 "MCG_STATUS의 MCIP 비트가 꺼져 있으면(비정상 상황) 안전
    쪽으로 fatal 취급"이라는 방어적 조건을 포함했으나, 실제
    `kHandleMachineCheck()`는 MCIP 여부와 무관하게 항상 뱅크를 순회해
    UC/PCC만으로 fatal을 판정한다 - 이 차이가 실질적 위험으로 이어지는
    경로가 없어(스퓨리어스 #MC 자체가 극히 드물고, 판정 기준이 더
    엄격해지는 방향이 아니라 방어 조건 하나가 빠진 것뿐) 별도 PN
    등록 없이 이 각주로만 남긴다.
  - **현재 상태**: 완전 갭 없음(사소한 관찰 제외 전부 설계 그대로).

### 1-Q. `SP-71DA77B3`(인터럽트 구독 서브시스템) §6 항목2 - 종료 시
자동 정리가 `PN-40E976F2` 완료에도 불구하고 실제로는 배선되지 않음
(코드 갭, 2026-09-21, 추적 `PN-4048116F`)

- **출처**: 이번 스윕에서 아직 이 방법론이 다루지 않았던 approved SP
  문서(`SP-71DA77B3`)를 처음 대입 - `interrupt_subscription.h/.cpp`와
  전문 대조.
- **문제**: §6이 "종료 시 자동 정리는 `PN-40E976F2`가 사망 전파 목록
  인프라를 구현한 뒤에야 배선 가능"이라고 명시했고, `PN-40E976F2`는
  이미 completed다 - 그런데 실제로 그 인프라가 구현한 것은 **죽는
  Task가 제출한 미완료 AsyncTask(pendingSyscalls)를 취소하는 것**뿐,
  "이 Task가 소유한 임의의 영속 자원을 정리하라"는 범용 후크가
  아니었다. `InterruptSubscriber` 슬롯은 AsyncTask가 아니라
  `SubscribeInterrupt` 호출로 만들어져 `Unsubscribe`가 올 때까지
  독립적으로 남는 영속 상태라 이 메커니즘의 대상이 아니다 - 저장소
  전체에서 `gSubscriptions[]`를 UserThread/Process 종료 경로에서
  순회하는 코드가 전무함을 실측 확인(`scheduler.cpp`/`process.cpp`
  어디에도 없음, `kmain.cpp`의 부팅 시 syscall 등록 호출 외엔
  `interrupt_subscription.cpp/.h`만 이 상태를 다룸).
  `WaitInterruptHandler::onCancel`(`PN-BD276A24`로 이미 고친 부분)은
  그 순간 파킹돼 있던 AsyncTask 하나만 큐에서 빼낼 뿐, 슬롯 자체
  (`used=true`)는 그대로 남는다.
- **조치**: `PN-4048116F`로 등록(해결 방식 두 후보 - WeakPtr 지연 GC
  vs 종료 경로에서 명시적 순회 - 는 CLAUDE.md 규칙4에 따라 착수 세션이
  설계자 확인 후 결정).
- **현재 상태**: [해소, 2026-09-21, 커밋 `c61a8b9`] `PN-4048116F`가
  설계자 답변(`QU-5BC539E2`)에 따라 "종료 경로에서 명시적 순회" 방식으로
  구현 완료 - `InterruptSubscriptionService::releaseAllForTask()`를
  `kFinalizeProcessTermination`/`SelfTerminateThreadHandler`(기존
  종료 감지 지점)에서 호출. 자세한 내용은 `PN-4048116F` 참고. 같은
  파일의 별개 결함(`PN-BD276A24`, onCancel 댕글링 포인터)도 이미
  해소돼 있어 §2에 별도 기록하지 않고 여기서 함께 언급만 한다.

- **[점검 완료, 2026-09-21] `SP-7CC5693A`(VFS 커널 서브시스템,
  approved)** - 이 문서가 실제로 소유하는 범위(§3+는 이미
  `SP-2BCE5D60`로 위임됨, 아래 §2 기존 항목 참고)인 §1/§2/§2.1-2.5/
  §4/§4-A를 `mount_table.h`/`vfs_syscall.h`/`livefs.h/.cpp`와 전문
  대조. `MountTable`(§2.1, Channel/KernelDriver 두 종류+최장 접두사
  일치)/`KernelFsDriver`(AsyncTaskHandler 상속 + 9개 op 구조체,
  §2.1 개정대로)/`Mount`/`Unmount`/`ResolvePath`/
  `SignalUserlandReady`/`WaitForUserlandReady` syscall 5종(§2.2/§2.5,
  RM-48E1E610 그룹3)/livefs의 `named`·`initrd.cpio`·`kernel/<name>`
  세 하위 경로(§2.4, `KernelReservedTable`은 실제로는
  `kernel_service_ring.h`라는 이름으로 구현돼 있음 - 이름만 다르고
  설계 그대로) 전부 실제 구현됨을 확인. §4의 "아직 열려 있는 설계
  영역" 5개 항목도 문서 자신이 이미 openly 미결로 표시해 둔 것과
  일치(숨겨진 게 아님). 갭 없음 - `vfs_syscall.h`가 스스로 "정직하게
  기록"이라 표시해 둔 의도적 범위 축소(`MountKind::Channel` 마운트의
  Open/Read 계열이 아직 `NotSupported`인 것 등)도 전부 §9(SP-2AAD7C8D)
  가 이미 별도로 추적 중인 열린 범위라 이 문서의 갭이 아니다.

- **[점검 완료, 2026-09-21] `SP-29D652AA`(컴파일러 진짜 thread_local
  도입, approved)** - §4(커널 TCB/`kSyncFsBase`)/§5(유저랜드 PT_TLS
  파싱+인스턴스 생성+커널↔유저 전환 FS_BASE 스왑)/§6(부팅 극초반)
  전부 `PN-22E5E9E7`(completed, 항목1-7 전체 완료)로 실제 구현됨을
  확인 - `tls.h/.cpp`(`gTlsSlots` 진짜 `thread_local` 배열),
  `task.h/.cpp`(`Task::kernelFsBase`/`kMakeTaskTlsBlock()`),
  `scheduler.cpp`(`kSyncFsBase`/`kSyncFsBaseToUser`, 5개 디스패치
  지점 배선), `process.cpp`(`makeUserTlsInstance`), `idt.cpp`
  (`kDispatchSyscallVerb`의 FS_BASE 스왑 래퍼) 전부 직접 대조.
  §7(§4.3 "`ThreadLocal<T>`와의 통합")도 실제로 `gTlsSlots` 하나를
  공유하는 방식으로 정확히 확정대로 구현됨. §4의 부팅 극초반 코어별
  TCB(항목4)는 실측 근거로 "불필요"라고 명시적으로 판단해 만들지
  않았다고 문서화돼 있음(숨겨진 누락이 아니라 조사 후 의도적 생략).
  갭 없음.

### 1-R. `SP-00CA7175`(커널 ↔ 커널 서비스 통신 채널) - "devmgr/fs/
net/tty" 4개 예시가 `PN-D6A05E78`(devmgr/fs KernelThread 흡수) 이후
낡음 (문서만 정정 - 코드 갭 아님, 2026-09-21)

- **출처**: 이 문서는 2026-09-17에 이미 자체 재검증(§4/§6 "전부
  완료")까지 거쳤으나, 그 재검증 시점(2026-09-17)이 devmgr/fs가
  KernelThread로 흡수되기(`PN-D6A05E78`, 2026-09-21) **이전**이라
  그 뒤에 생긴 낡음은 아직 반영이 안 돼 있었다.
- **문제**: §2.0/§2.1/§4 곳곳이 "devmgr/fs/net/tty" 4개를 이 Tier
  A/B 메커니즘의 대상 커널서비스로 예시한다 - 그러나 devmgr/fs는
  이제 Process 없는 순수 커널 `KernelThread`라 `/sys/live/kernel/
  <name>` 경로로 커널과 IPC할 이유 자체가 없다(커널 자신의 코드이므로
  직접 함수 호출). 실측: `kmain.cpp`의 `gServiceManifest[]`(411행
  `reserveForKernelService()` 호출 루프의 실제 대상)는 이미 `net`/
  `tty`/`pubreg`/`authmgr` 4개로 정확히 갱신돼 있다 - **코드는 이미
  옳고, 이 문서의 예시 목록만 낡았다.**
- **조치**: `SP-00CA7175`에 정정 각주 추가(원문 보존, §2.0 앞). 코드
  쪽 조치 불필요.
- **현재 상태**: 완전 해소(문서 정정).

- **[점검 완료, 2026-09-21] `SP-68182FBD`(프로세스 모델) §4(SIGCHLD
  자식 종료 통지, 2026-09-18 확정)** - `parent->raiseSignal(
  SignalNumber::Chld)` 호출이 §4.2가 지목한 정확한 지점(scheduler.cpp
  961-978행, `SelfTerminateHandler::onExec`의 좀비화 직후)에 실제로
  존재함을 확인. §4.3이 "착수 세션이 코드 감사로 확정" 대상으로 남겨
  둔 열린 질문(Kill로 강제 종료된 자식도 같은 지점을 거치는지)도
  직접 콜체인을 추적해 확인 완료 - `KillHandler::onExec`(process.cpp)
  는 `target->raiseSignal(args->signal)`만 호출하고, 그 신호가
  `kCheckSignalCheckpoint()`(idt.cpp:657)에서 Default(비-Ignore)
  disposition으로 판정되면 `kTaskOnFallingToEnd()`(scheduler.cpp)를
  호출해 자기종료 syscall을 제출하고, 그 결과 다시
  `SelfTerminateHandler::onExec`의 §4.2 지점을 거친다 - 즉 Kill이든
  자연 종료든 **모든 종료 경로가 결국 하나의 좀비화 지점으로
  수렴**해 SIGCHLD가 빠짐없이 발신됨을 코드로 확인(별도 경로 없음,
  §4.3 항목1 우려는 기우였음이 확정). 갭 없음.

- **[점검 완료, 2026-09-21] `SP-A252E82F`(인터럽트 컨텍스트 재설계 -
  `#PF` IST5 격리 + 일반 인터럽트 무조건 단일 스택 스왑 +
  `gInterruptDepth` 폐기)** - 승인 직후 같은 세션이 곧바로 구현까지
  마쳐(`PN-160AC313`, commit `e44006b`) 이 문서의 통상적인 "승인 vs
  코드" 시차 자체가 거의 없었던 드문 사례. §1(#PF IST5 배정)/§2(#PF
  재진입 감지+즉시 정지)/§3(일반 벡터 무조건 스왑, `gInterruptDepth`/
  `kEnterInterruptDepth`/`kLeaveInterruptDepth`/`kCurrentInterruptDepth`/
  `gInterruptDepthGuardPage` 삭제)/§4(디스패치 스택 크기 32KiB 유지)
  전부 실제 코드(`gdt.cpp`/`gdt.h`/`idt.cpp`/`isr.S`/`context_switch.S`/
  `deferred_destruction.h`/`.cpp`)에 반영됨을 구현 세션 자신이 이미
  확인(빌드+표준 4시나리오+`pn584_repro_count.sh` 40회=0/40으로
  검증). "위험/미해결 지점" 절 항목1(`Paging::handlePageFault()`가
  영구 매핑 메모리만 건드리는지 감사)도 실제 소스(`paging.cpp`의
  `handlePageFault`/`kHandleCowWriteFault`/`kAsTable`)를 직접 읽어
  전부 direct map(`kPhysToVirt`)과 정적 커널 구조체만 거침을 확인
  완료. 갭 없음.

- **[점검 완료, 2026-09-21] `SP-DE19BB1C`(커널 영역 TLB 샷다운,
  approved)** - `tlb_shootdown.h/.cpp`를 §2(기본 IPI-ISR-ACK 골격)/
  §5-1(유저 영역 확장, PN-D132A1E9가 이미 완료 기록)과 전문 대조.
  `Lapic::sendFixedIpi`/`TlbShootdown::broadcast()`/
  `kTlbShootdownHandler` 전부 구현 확인 - 특히 §5-1이 확정한 후기
  설계(요청자 코어별 전용 슬롯 `gRequests[kAcpiMaxCpus]` + 수신자별
  Target Pending Mask `gPendingMask[]`, 락 불필요 구조)가 실제
  코드이고, 문서 본문이 §2에 스케치해 둔 초기 "슬롯 1개" 버전은
  이미 폐기된 설계로 문서 자신이 §5-1에서 명시적으로 대체해 뒀음을
  재확인(코드가 최신 설계와 일치, 갭 아님). 벡터 `0xE0`도 §5-1/
  RM-28225668과 정확히 일치. 부팅 초기 AP 미기동 구간의 실측 버그
  수정(`Smp::startedCount()` 상한, PN-012E8C1A 발견)까지 코드 주석에
  근거와 함께 남아 있음 - 갭 없음.

- **[점검 완료, 2026-09-21, 신규 승인 문서] `SP-A252E82F`(인터럽트
  컨텍스트 재설계 - #PF만 IST5 격리, 일반 인터럽트 무조건 단일 스택
  스왑, `gInterruptDepth` 폐기, approved)** - `PN-160AC313`(completed)
  구현분을 코드로 직접 대조(규칙14, 새로 승인된 SP라 즉시 점검).
  `idt.cpp`(`kPageFaultIst = 5`, `gIdt[kPageFaultVector].ist` 배정)/
  `isr.S`(`isr_common_stub`이 vector 1/2/8/14/18 다섯 개만 IST 경로로
  분기하고 나머지 전부 `kEnterInterruptStack`/`kLeaveInterruptStack`
  무조건 호출로 통일 - §3 설계 그대로)/`deferred_destruction.h/.cpp`
  (`gInterruptDepth`/`kEnterInterruptDepth`/`kLeaveInterruptDepth`
  실제 삭제 확인 - 남은 문자열은 전부 "예전엔 ~했지만"류 역사적
  주석뿐, 코드 자체엔 없음)까지 설계 그대로 구현됨을 확인. 갭 없음
  - 이 세션이 처음부터 추적해 온 `PN-9326B06F`/`PN-1DFCB337`/
  `PN-584DB994` 계열 조사의 최종 산출물이 실제로 코드에 반영된
  것까지 직접 검증 완료.

## §2-추가. [점검 완료, 2026-09-21] `SP-43331889`(devmgr/fs 커널 흡수 + 유저모드 드라이버 지원, review)

§8의 두 승인 요청(§6 (a)/(b) 중 (a) 선택, §1 Process 껍데기 제거)
모두 각주로 해소 표시돼 있고, §7 단계별 착수 순서도 §7-1-a/§7-1-b가
"1/2/3/5/6/8번을 devmgr/fs 둘 다에 대해 전부 완료"라고 명시 -
`PN-615C48D5`(completed)로 실제 구현까지 끝났음을 확인. 남은 항목
(kSpawnUserModeDriver/PN-A0F72A3A 교체/AllocDmaBuffer 커널 모드
매핑)은 전부 `PN-A8BE8BED`(scheduled)로 이관돼 누락 없이 추적 중.
**갭 없음** - 이 문서 자체가 `review` 상태로 남아 있는 것은 설계
공백이 아니라 순수 행정 절차(문서 상태 전이는 이 세션 권한 정책상
AI가 스스로 승인 처리할 수 없어 보류 - `SP-A21DD889`에서도 동일한
제약 확인) - 설계자가 직접 `approved`로 전이하면 될 항목.
`DC-91ABD922`(이 설계를 촉발한 결정 문서, 역시 review)도 §8의 답변
전부 받아 내용상 종결됐다는 점에서 동일한 상태.

## §3. 아직 점검 안 한 영역 (다음 틱 대상)

**[2026-09-24] `SP-CC2B18C6`(UEFI physicalBase 재배치 설계) 신규
승인 - 점검 보류(구현 착수 전)** - 규칙14가 approved 전환마다
이 절에 추가하라고 하지만, 이 문서는 아직 코드가 한 줄도 없다(설계
문서 자체가 "실측 검증 전체"를 §5 미결로 명시). §목차 나열형
"확정된 설계" vs 실제 코드 대조는 구현이 실제로 진행된 뒤에나
의미가 있으므로, `PN-7FBF255A`(착수 가능 상태로 전환됨) 진행 상황을
봐 가며 코드가 생기기 시작하면 그때 이 절로 다시 끌어와 점검한다.

같은 방법론(§목차 나열형 "확정된 설계" 절 vs 실제 코드)을 아직
적용 안 해본 주요 SP 문서/영역 - 매 틱 1-2개씩 골라 점검하고
결과를 이 절에서 §1(발견) 또는 §2(갭 없음)로 옮긴다:

- [ ] (`SP-9A6D579F` 항목은 §2로 이동 - 2026-09-18 갱신 완료)
- [ ] (`SP-9DD4F3EA`/`SP-CC1CF30E` 항목은 각각 §1/§2로 이동 - 2026-09-20 갱신 완료)
- [ ] (`SP-ECC59BAE` 항목은 §1로 이동 - 2026-09-20 갱신 완료, PN-6CE4DD35 등록)
- [ ] (`SP-5A255B7C` 항목은 §2로 이동 - 2026-09-20 갱신 완료, 갭 없음)

(`SP-B1E258D8`(RCU) 항목은 approved 전환 + `PN-495C11B7` 구현
완료까지 끝나 아래 §2로 이동했다.)
(`PN-C4611402`의 "실제 취소 레이스" 재검증 - `PN-B5C2845A`가 열어
준 뒤 이 세션이 실제로 QEMU에서 재현/확정했다. 아래 §2로 이동.)
(`PN-2008220B` 재검증 완료 - 아래 §2로 이동.)

**[2026-09-23] 2026-09-22 승인분 5개 SP 문서 재소진 확인 완료** -
`SP-D02C4A73`(libswapfs)/`SP-A658A124`(libvfat)/`SP-7A9CED3E`
(libext4)/`SP-F1987EF8`(libexfat)/`SP-AA6DF406`(libntfs) 전부 이
방법론으로 대조 완료(libvfat/libext4는 이번 세션 직접 구현 작업
중 자연스럽게 대조됨, 나머지 3개는 이 §3 스윕으로 별도 확인) -
발견 1건(libexfat 체크섬, 아래 §1-S)+블로커 갱신 1건(libswapfs,
§2)+갭 없음 확인 2건(libext4/libntfs).

**[2026-09-23, 추가 틱] 2026-09-23 승인분 2건 확인 완료** -
`SP-0C7A4F3B`(Power 서브시스템)/`DC-F367AD5D`(그 결정 요청 원본)
대조 완료 - 갭 없음(§2 참고, 항목5는 §1-W에 이미 기록된 알려진
비활성화). `document_list(status=approved)` 전수 재확인 결과 이
방법론을 아직 안 적용한 SP/DC가 더 이상 없음 - 다음 approved
전환 시까지 이 §3 스윕은 다시 건너뛴다(2026-09-21/2026-09-22
절과 동일한 재소진 패턴).

**[2026-09-23] `SP-F1987EF8`(libexfat) 점검 완료 - 갭 발견/해소**
(`PN-831A3998`, commit `863113f`) - §3.5가 "setChecksum은 읽기 시
검증"을 확정된 설계로 명시했으나 `PN-09970F05`(ExfatDriver 구현)가
필드만 파싱하고 실제 검증을 빠뜨렸었다. `kExfatEntrySetChecksum()`
추가로 해소, 정상/손상 이미지 양쪽으로 실측 검증(mkfs.exfat+
exfat-fuse) 완료. **§2(갭 없음)가 아니라 §1(발견)에 해당** - 아래
새 항목 1-S로 기록.

**[2026-09-21] approved SP/DC 문서 후보 풀 재소진 확인** - 이번 세션이
`SP-677210E6`/`SP-7CC5693A`/`SP-29D652AA`/`SP-00CA7175`/`SP-71DA77B3`/
`SP-68182FBD`/`SP-DE19BB1C`/`SP-D7013B26`(마지막 항목은 §2에 이미
있던 기존 점검과 중복 발견 - 새로 추가하지 않고 원복)까지 마저
대조한 결과, `document_list(status=approved)` 전수(~70건)에서 이
방법론을 아직 안 적용해 본 SP/DC가 더 이상 없음을 확인했다 - 새
SP/DC가 approved로 전환될 때마다(규칙 14) 이 절에 다시 채워질
것이다. 다음 틱들은 `document_list`로 새로 approved된 문서가
있는지부터 확인하고, 없으면 이 §3 스윕은 건너뛰고 §0-2(메시지/
계획) 루틴에 집중한다.

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

- **[점검 완료, 2026-09-18, 갭 없음] `SP-76250478`(멀티스레드 유저
  프로세스 지원) → `PN-0EB2FABF`(completed)**: 예고했던 다섯 항목
  전부 커밋으로 반영됨을 `PN-0EB2FABF` 완료 기록으로 대조 확인 -
  §2.1(commit b0f2753, `Process::threads`/`ThreadId`/`UserThread`
  종료 필드), §2.2(commit 02b2702, `CreateThread`), §3
  항목2-3(commit fd01642, `SelfTerminateThread` + 좀비/Join 정책),
  §3.1(commit 5ee0fec, `Join`/`Detach` 진짜 블로킹) 전부 "뒷부분
  항목 누락" 없이 완결. §4가 "착수 세션이 코드 감사로 확정"하라고
  넘긴 두 실구현 세부(스케줄링 1:1 가정 전수 재검토, AsyncTask
  코루틴 강제 재개 방식)도 각각 1단계(`WaitHandler`/`raiseSignal()`/
  `ResourceGroup::thaw()`/`DebugContinueHandler`/`kFormatStatus()`
  전체 순회로 수정)와 4단계(`AsyncTaskWeakRef` 공개 승격 + `JoinAwaiter`
  커스텀 `co_await`, 그 과정에서 코루틴 프레임 누수/재개 경로 CR3
  미동기화 잠재 버그 2건도 함께 발견·수정)에서 실제로 처리됨을
  확인 - **갭 없음.**

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
- **계획이 스스로 건 "착수 조건"(예: "QU-XXXXXXXX 해소 대기")을
  다시 확인할 땐 `docs pending`/`pending_list`가 아니라
  `question_list`(status=all) 또는 대상 문서의 전체 질의 스레드로
  확인한다** - `pending_list`는 설계상 미해결(open+pending)만
  보여주고 이미 `resolved`로 확인 처리된 질의는 제외하므로, "그
  질의로는 안 보인다"가 "그 질의가 애초에 없었다"를 뜻하지 않는다
  (2026-09-17 사례: minicore-88이 `PN-4190BBD3`의 "QU-C10BAA06 해소
  대기"를 `docs pending`으로만 확인해 "한 번도 등록된 적 없었다"고
  잘못 결론 - 실제로는 minicore-f8이 이미 그 답변을 확인·ack까지
  마친 상태였다, PN-4190BBD3에 정정 기록).
- 이 문서 자체는 "완결"되는 문서가 아니다 - §3이 빌 때까지, 그리고
  그 이후로도 새 SP 문서가 승인될 때마다 계속 대상에 추가한다(루프
  표준 절차 0-3번에 편입, 별도 지시 참고).

