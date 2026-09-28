# authmgr GrantSudoPermission 권한 검사 - 누구나 임의 (callerUid,targetUid) 쌍을 sudo 화이트리스트에 등록 가능

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-2B22FBF0
  status: review
  updatedAt: 2026-09-28T15:20:42.491Z
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

## 참고

- `PN-812A139B`(scheduled) - 이 공백을 사후 등록한 계획.
- `DC-CC83F7BE`(approved, 구현 완료) - `CreateUser`의 동일한 구조적
  문제를 해결한 선례((A) 채택, 조상-자손 규칙).
- `DC-34764C25`(approved) - `GrantSudoPermission`/`CheckSudoPermission`을
  최초로 도입한 결정(항목1).
- `minicore/kernel/authmgr_client.h` - `kAuthmgrCheckSudoPermission()`/
  `kAuthmgrCreateUser()`, (A)가 재사용할 기존 패턴.
- `userland/libs/libmc/authmgr.h` - `GrantSudoPermission` 요청/
  `AuthmgrSudoPermissionRequestBody` 실제 와이어 정의.
- `RM-48E1E610` 그룹0(Process) - 다음 미사용 call 13((A) 채택 시
  `GrantSudoPermission`이 예약할 번호).

