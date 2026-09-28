# authmgr CreateUser 권한 검사 - caller uid를 와이어로 신뢰성 있게 전달하는 규약이 없음

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-CC83F7BE
  status: approved
  updatedAt: 2026-09-28T11:20:42.866Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

`PN-24A2B6F5`(authmgr 계획)의 "남은 것" 목록 항목2 - `CreateUser`
요청(`mc::AuthmgrRequestType::CreateUser=3`, `userland/libs/libmc/authmgr.h`)
은 코드 자체 주석이 명시하듯 **권한 검사가 전혀 없다**:

```
// [신규, 2026-09-23] 새 UserRecord 생성 - **[알려진 제약]** 이
// 요청은 아직 어떤 kernel syscall 경로로도 노출되지 않는다.
// "누가 요청했는지"(caller uid)를 Channel IPC 와이어에 실어
// 보내는 규약 자체가 아직 없어(...) 권한 검사를 전혀 하지 않는다 -
// 지금은 authmgr 자신의 libkvdb 배선을 검증하기 위한 내부용/테스트
// 전용 요청이다.
```

`kSetuid()`는 uid 트리 구조(부모→자손만 승격 가능)라는 명확한 정책이
있는데, `CreateUser`도 유사하게 "누가 새 uid를 만들 수 있는가"를
정해야 한다 - 예를 들어 "root만", 또는 "자기 자신의 자손 uid만
만들 수 있다"(kSetuid와 대칭) 등.

## 실제 코드 조사 결과

- `CreateUser` 요청 본문(`AuthmgrUserRecord`)엔 `uid`/`parentUid`/`gid`/
  `loginName`/`passwordHash`/`defaultShell`만 있고, **"누가 이 요청을
  보냈는지" 식별자 자체가 와이어에 없다**.
- authmgr은 순수 유저랜드 프로세스이고, `CreateUser`는 지금 임의의
  프로세스가 authmgr에 직접 Channel로 연결해 보낼 수 있는 요청이다 -
  만약 요청 본문에 그냥 `callerUid` 필드를 추가해도, authmgr은 그
  값이 진짜인지 검증할 방법이 없다(보낸 쪽이 자기 uid를 거짓으로
  적어도 authmgr 입장에선 구분 불가 - 신뢰할 수 없는 자기 신고).
- 반면 `kSetuid()`의 캐시미스 질의(`DC-90A66932`로 이번에 새로 만든
  `minicore/kernel/authmgr_client.h/.cpp`)는 **커널 자신**이 authmgr에
  Channel 클라이언트로 접속해 질의를 대신 보내는 구조다 - 이 경로는
  이미 "커널이 중개자로서 신뢰할 수 있는 값을 실어 보낸다"는 선례가
  있다.

## 왜 지금 결정이 필요한가

이 요청을 실제 syscall로 노출하는 순간(v1은 테스트 전용이라 미노출
상태라 당장 위험은 없음) 아래 중 하나를 정해야 하는데, 커널/authmgr
경계를 가로지르는 신뢰 모델 설계라 이 세션이 임의로 결정하지 않고
여쭙는다(CLAUDE.md 규칙 4):

**(A) 커널 중개 방식** - `CreateUser`를 새 kernel syscall(그룹0
다음 미사용 call)로 노출하고, `authmgr_client.h`가 이미 갖고 있는
"커널이 authmgr에 신뢰할 수 있는 Channel 클라이언트로 접속" 패턴을
재사용해 커널이 호출자의 실제 `Process::uid`를 직접 실어 authmgr에
전달한다. authmgr은 이제 "이 요청은 항상 커널이 보낸 것"이라는
전제 위에서 `callerUid` 필드를 그대로 신뢰할 수 있다. `kSetuid()`와
대칭적인 권한 규칙(예: "callerUid가 root이거나, 새로 만들 uid의
`parentUid`가 곧 callerUid일 때만 허용" - 자기 자손만 만들 수 있음)을
커널 쪽에서 판정 후 authmgr엔 이미 통과한 요청만 전달, 또는 판정
자체를 authmgr에 맡기고 신뢰된 callerUid만 실어 보낸다(둘 중 어느
계층이 최종 판정하는지도 답변 필요).

**(B) Channel 레벨 peer 신원 첨부** - Unix `SO_PEERCRED`류 기능을
`Channel`/`BridgePipe`에 새로 추가해, accept 시점에 커널이 연결
상대의 uid를 자동으로 실어 두고 authmgr이 `Read`/`Accept` 결과에서
그 값을 신뢰할 수 있게 한다 - `CreateUser`뿐 아니라 향후 다른
"caller 신원이 필요한 Channel 요청" 전부에 재사용 가능한 범용
인프라가 되지만, `Channel`/`BridgePipe` 계층 자체를 건드리는 더 큰
작업이다.

**(C) 지금은 노출하지 않음** - `CreateUser`를 계속 테스트 전용으로
남겨 두고(v1 범위 밖으로 명시적으로 유예), 실제 "새 사용자를 만드는"
유저랜드 시나리오(예: 관리자 도구)가 구체적으로 필요해지는 시점까지
이 결정을 미룬다.

## 참고

- `PN-24A2B6F5` - 이 공백을 남긴 상위 계획, "남은 것" 항목2.
- `DC-90A66932`(approved) - `authmgr_client.h`(커널 전용 Channel
  클라이언트) 선례, (A)의 재사용 대상.
- `PN-B6DB692C`(scheduled) - `kSetuid()`의 uid 트리(조상-자손) 판정
  정책, (A)가 대칭시킬 참고 규칙.
- `userland/libs/libmc/authmgr.h` - `CreateUser` 요청/`AuthmgrUserRecord`
  실제 와이어 정의.

## [구현+실측 검증 완료, 2026-09-28]

설계자 답변("(A) 커널 중개... 최종 권한 판정은 커널이") 반영해
구현 완료, commit `bcdc4d8`로 발행했다. `kSyscallEndpointCreateUser`
(그룹0 call12) 신설 - `CreateUserHandler`(process.cpp)가
`kCreateUserOnExecImpl`(user_record.cpp)로 위임(`SetuidHandler`와
동일한 패턴). 권한 판정은 `kSetuid()`가 내부적으로 쓰던
`kIsDescendantUser`를 헤더에 노출해 그대로 재사용 - "callerUid가
새로 만들 uid의 parentUid 자신이거나 그 조상일 때만 허용"(root는
예외적으로 항상 허용). 통과하면 `authmgr_client.h`(DC-90A66932)로
authmgr에 CreateUser를 대신 요청한다.

**실측 중 발견/수정한 버그**: 생성 성공 후 그 레코드를 커널 자신의
`UserRecordCache`에 채우지 않으면, 방금 만든 uid를 곧바로 parentUid로
삼아 손자를 생성(또는 Setuid)하려 할 때 캐시 미스로 `ServiceUnavailable`
이 나는 실제 문제를 발견했다(`minicore/createusertest`로 재현) -
`kAuthmgrCreateUser()` 성공 직후 `UserRecordCache::insertOrUpdate(record)`
를 호출하도록 수정해 해소했다(어차피 커널 자신이 방금 authmgr에
만들라고 보낸 값이라 재조회 없이 그대로 신뢰).

`minicore/createusertest`(신규, 영구 보존)로 4가지 시나리오 실측
검증: (1) root가 임의 parentUid로 생성 - 허용, (2) 비-root가 자기
직계 자식을 생성 - 허용, (3) 비-root가 무관한(조상 아닌) parentUid로
생성 시도 - `PermissionDenied`(권한 상승 회귀 없음 확인), (4) 비-root가
자기 손자(조상 체인 2단계)를 생성 - 허용(직계 부모 판정이 아니라
조상 체인 전체 판정임을 확인, 위 캐시 버그가 바로 이 시나리오에서
발견됨). 표준 회귀 3종(PVH/GRUB SMP1/SMP4) 클린. TEMP 진단 로그/스폰
훅은 검증 직후 완전히 원복(`git status` 클린).

**의도적으로 남긴 잔여 공백**(선택하지 않은 (B)의 결과) - authmgr의
raw Channel(이름 "authmgr")에 직접 연결해 CreateUser를 보내는 경로는
여전히 무검증이다. 설계자가 (A)를 선택하고 (B)(Channel 레벨 peer
신원 첨부)를 선택하지 않았으므로, 이 syscall은 "올바른 문"을 새로
만든 것이지 "예전 문을 잠근" 것은 아니다 - authmgr 자신의
권한-검사-없음 상태(코드 주석에 이미 명시돼 있던 그대로)는 의도적으로
변경하지 않았다.

`PN-24A2B6F5`(authmgr 계획)의 "남은 것" 항목2가 이걸로 완료됐다.
