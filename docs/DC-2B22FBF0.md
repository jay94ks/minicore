# authmgr GrantSudoPermission 권한 검사 - 누구나 임의 (callerUid,targetUid) 쌍을 sudo 화이트리스트에 등록 가능

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-2B22FBF0
  status: approved
  updatedAt: 2026-09-28T17:26:48.790Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

`PN-812A139B`(scheduled)가 `DC-34764C25` 항목1 구현(commit `c26c89a`)
과정에서 남긴 공백을 정리한 계획이다 - authmgr의 `GrantSudoPermission`
요청(`mc::AuthmgrRequestType::GrantSudoPermission=5`, sudo 화이트리스트
`(callerUid, targetUid)` 쌍 등록)은 코드 자신의 주석이 명시하듯
**권한 검사가 전혀 없다**:

```
case mc::AuthmgrRequestType::GrantSudoPermission: {
    // [알려진 제약, libmc/authmgr.h 문서 주석 참고] CreateUser와
    // 동일한 이유로 권한 검사 없음 - DC-1526389A 답변(관리 API
    // 설계) 전까지 테스트/시딩 전용.
    ...
```

(`minicore/authmgr/main.cpp`) 이 주석이 가리키던 "DC-1526389A 답변"은
실제로는 VFS uid/gid/mode 공백(`SP-9039F955`로 이미 해소·구현 완료)
얘기였다 - "관리 API 설계"라는 자기 참조가 가리켜야 했던 진짜 대상이
바로 이 DC다(`PN-812A139B` 본문에도 이미 이 정정이 적혀 있음).

**심각도**: `CreateUser`(동일 패턴의 선례, `DC-CC83F7BE`로 이미 해결)
보다 더 민감하다 - 새 계정을 만드는 것과 달리, sudo 화이트리스트
등록은 **기존 계정에 직접 권한 승격 경로를 열어주는 행위**라 악용
시 즉시 임의 uid로의 승격으로 이어진다(`kSetuidOnExecImpl`이 root/
조상-자손 판정에 실패했을 때 이 화이트리스트를 마지막 수단으로
확인 - `CheckSudoPermission`이 허용하면 그대로 setuid 성공).

## 실제 코드 조사 결과

- `GrantSudoPermission` 요청 본문(`AuthmgrSudoPermissionRequestBody`)엔
  `callerUid`/`targetUid` 두 필드뿐이고, `CreateUser`와 마찬가지로
  **"누가 이 요청을 보냈는지" 식별자 자체가 와이어에 없다**.
- authmgr은 순수 유저랜드 프로세스이고, 지금은 임의의 프로세스가
  authmgr에 직접 Channel(이름 "authmgr")로 연결해 `GrantSudoPermission`을
  보낼 수 있다 - 요청 본문에 "누가 등록을 요청했는지"를 자기 신고로
  덧붙여도 authmgr은 검증할 방법이 없다(`DC-CC83F7BE`가 `CreateUser`에
  대해 이미 정리한 것과 동일한 구조적 이유).
- `CreateUser`는 이미 커널 중개 경로(`kSyscallEndpointCreateUser`,
  그룹0 call12)로 해결됐고, `authmgr_client.h`가 그 패턴("커널 자신이
  authmgr에 신뢰할 수 있는 Channel 클라이언트로 접속해 대신 요청")을
  이미 갖고 있다 - `kAuthmgrCreateUser()`/`kAuthmgrCheckSudoPermission()`
  옆에 `kAuthmgrGrantSudoPermission()`을 추가하는 건 기계적으로
  대칭적이다.

## 왜 지금 결정이 필요한가

`CreateUser`와 똑같은 신뢰 모델 결정이 필요하지만(CLAUDE.md 규칙 4,
이 세션이 임의로 결정하지 않고 여쭙는다), **권한 규칙 자체는
`CreateUser`와 대칭이 아닐 수 있다** - `CreateUser`는 "callerUid가
root이거나 새 uid의 조상"이면 허용했지만(자기 자손 계정을 만드는
건 상대적으로 안전), `GrantSudoPermission`은 "누군가에게 다른 uid로
승격할 권한을 준다"는 행위라 같은 조상-자손 규칙을 그대로 쓰면
비-root 사용자가 자기 자손에게 임의 targetUid로의 승격 권한을
스스로 부여할 수 있게 된다 - 이게 의도인지부터 확인이 필요하다.

**(A) 커널 중개 방식** - `GrantSudoPermission`을 새 kernel
syscall(그룹0 call13, `RM-48E1E610` 다음 미사용 번호)로 노출하고
`authmgr_client.h`의 기존 패턴을 재사용해 커널이 호출자의 실제
`Process::uid`를 직접 실어 authmgr에 전달한다. 이 경우 **정확한
허용 규칙**을 아래 중 하나로 정해야 한다:
  - (A-1) **root만 허용** - Linux의 `/etc/sudoers`가 root(정확히는
    root 권한의 `visudo`)만 편집 가능한 것과 대칭. 가장 보수적.
  - (A-2) **root 또는 targetUid 자신의 조상**(`CreateUser`와 동일한
    `kIsDescendantUser` 규칙을 그대로 재사용) - "내가 만든 자손
    계정에게 내 권한으로 sudo할 자격을 준다"는 시나리오를 허용.
  - (A-3) 그 외 별도 규칙(예: callerUid 자신이 이미 root로 sudo할
    자격이 있어야만 남에게도 부여 가능 등) - 있다면 명시 필요.

**(B) Channel 레벨 peer 신원 첨부** - `DC-CC83F7BE`가 이미 제시했던
것과 동일한 범용 옵션(`Channel`/`BridgePipe`에 `SO_PEERCRED`류 기능
신설) - `CreateUser` 때도 선택되지 않았던 더 큰 인프라 작업이다.

**(C) 지금은 노출하지 않음** - `GrantSudoPermission`을 계속 테스트/
시딩 전용으로 남겨 두고(현재 상태 그대로), 실제 관리자 도구가
구체적으로 필요해지는 시점까지 이 결정을 미룬다. `PN-812A139B` 본문이
이미 이 선택지도 유효하다고 적어 뒀다.

## [답변, 2026-09-28, `QU-74FC561E`] (A-2) 조상-자손 규칙 재사용 채택

설계자 답변: "(A-2) 조상-자손 규칙 재사용" - `GrantSudoPermission`을
`CreateUser`와 대칭인 커널 중개 syscall(그룹0 call13,
`RM-48E1E610` 다음 미사용 번호)로 노출하고, 허용 판정은
`kIsDescendantUser`(`DC-CC83F7BE`가 `CreateUser`에 쓴 것과 동일한
tree-walk)를 그대로 재사용한다 - callerUid가 root이거나
targetUid의 조상이면 허용. 비-root가 자기 자손에게 임의 targetUid로
승격할 sudo 권한을 부여할 수 있게 되는 것이 이 답변으로 의도적으로
확정됐다(§ "왜 지금 결정이 필요한가"가 제기했던 우려에 대한 명시적
답변).

착수 세션은 `kernel::authmgr_client.h`에 `kAuthmgrCreateUser()`/
`kAuthmgrCheckSudoPermission()`과 같은 패턴으로
`kAuthmgrGrantSudoPermission()`을 추가하고, 새 `GrantSudoPermission`
syscall 핸들러가 `kIsDescendantUser(callerUid, targetUid)`(root
예외 포함) 판정을 통과했을 때만 이를 호출하도록 구현하면 된다.

## [구현+실측 검증 완료, 2026-09-29]

설계자 답변("(A-2) 조상-자손 규칙 재사용") 반영해 구현 완료.
`kSyscallEndpointGrantSudoPermission`(그룹0 call13) 신설 -
`GrantSudoPermissionHandler`(process.cpp)가
`kGrantSudoPermissionOnExecImpl`(user_record.cpp)로 위임
(`CreateUserHandler`/`kCreateUserOnExecImpl`과 완전히 동일한 패턴).
권한 판정은 `kIsDescendantUser(proc->uid, args->targetUid)` -
callerUid(=caller 자신의 실제 uid)가 root이거나 targetUid의 조상일
때만 authmgr에 (callerUid, targetUid) 쌍 등록을 대신 요청한다
(`kAuthmgrGrantSudoPermission()`, authmgr_client.h 신설 - 기존
`kAuthmgrCheckSudoPermission()`과 동일한 와이어 왕복 모양).

`minicore/granttest`(신규, 영구 보존)로 3가지 시나리오 실측 검증:
(1) root가 임의 targetUid에 대해 GrantSudoPermission - 허용, (2)
비-root(uid80)가 자기 직계 자식(uid82)에 대해 GrantSudoPermission -
허용, (3) 비-root(uid80)가 무관한 uid(uid83, 80의 자손이 아님)에
대해 시도 - `PermissionDenied`(권한 상승 회귀 없음 확인). 부팅 경합
재시도(lrutest/createusertest와 동일 패턴) 적용 후 GRUB SMP4에서
4회 연속 통과, 기대값을 의도적으로 틀리게 바꾼 음성 대조군에서도
정직하게 실패 코드 반환 확인. 표준 회귀 3종(PVH/GRUB SMP1/SMP4)
클린. TEMP 스폰/진단 훅은 검증 직후 완전히 원복(`git status` 클린).

**의도적으로 남긴 잔여 공백**(CreateUser와 동일한 이유) - authmgr의
raw Channel(이름 "authmgr")에 직접 연결해 GrantSudoPermission을
보내는 경로는 여전히 무검증이다. 설계자가 (A)를 선택하고 (B)(Channel
레벨 peer 신원 첨부)를 선택하지 않았으므로, 이 syscall은 "올바른
문"을 새로 만든 것이지 "예전 문을 잠근" 것은 아니다.

`PN-812A139B`가 이걸로 완료됐다.

## 참고

- `PN-812A139B`(completed) - 이 공백을 사후 등록한 계획.
- `DC-CC83F7BE`(approved, 구현 완료) - `CreateUser`의 동일한 구조적
  문제를 해결한 선례((A) 채택, 조상-자손 규칙).
- `DC-34764C25`(approved) - `GrantSudoPermission`/`CheckSudoPermission`을
  최초로 도입한 결정(항목1).
- `minicore/kernel/authmgr_client.h` - `kAuthmgrCheckSudoPermission()`/
  `kAuthmgrCreateUser()`/`kAuthmgrGrantSudoPermission()`(신규),
  (A)가 재사용한 기존 패턴.
- `userland/libs/libmc/authmgr.h` - `GrantSudoPermission` 요청/
  `AuthmgrSudoPermissionRequestBody` 실제 와이어 정의.
- `RM-48E1E610` 그룹0(Process) call13 - 구현 완료로 갱신.

