# Minicore 사용자/권한 체계(uid/gid + RWX + root) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-30FCC8AE
  status: review
  updatedAt: 2026-09-17T03:26:49.164Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# Minicore 사용자/권한 체계(uid/gid + RWX + root) — 설계 제안

**배경**: `QU-78E4159E`(`SP-9CB55C5B` §4, Kill 권한 스코프) 답변
(2026-09-17): "조상-자손 + 커널/커널서비스는 예외로 두되, 시스템
사용자를 의미하는 사용자의 권한 체계를 구성하고(그룹 RWX, 소유자
RWX, 그 외 RWX, 그리고 특수 비트 S) root(uid = 0)라는 특별한
사용자라는 개념을 만들어야해. 권한 모델 자체가 linux와 유사해야해."
`PN-617F4E52`로 분리 등록된 후속 설계 과제(minicore-f8 착수, 세션
간 조율 완료 - minicore-88은 ResourceGroup 구현 중).

**페이싱 원칙**: 설계자가 `SP-245D130B`(cgroup류) 검토 중 남긴
지침을 여기도 그대로 따른다 - "이 기능 자체가 워낙 거대한 기능이라서,
현 단계에서 설계하는 것은 설계 공백을 감당할 수 없을 거야." uid/gid+
RWX+root도 Linux 기준 방대한 기능 영역(setuid, 다중 그룹, ACL,
capability 등)이라, 이 문서는 **지금 실제로 막혀 있는 것(Kill의
권한 판정)을 푸는 데 필요한 최소 골격**만 확정하고, 나머지는 §7에서
명시적으로 미룬다.

## 1. 신원 - `uid`/`gid` 필드

```cpp
// minicore/kernel, 가칭 uid.h 또는 process.h에 직접
using Uid = uint32_t;
using Gid = uint32_t;
constexpr Uid kRootUid = 0;
constexpr Gid kRootGid = 0;
```

`Process`에 `Uid uid`/`Gid gid` 필드 추가 - `role`/`startFlags`와
동일한 관례(SP-EAB162FC §1/§2.1 - 생성 시점에 고정, 이후 바꾸는
setter를 두지 않는다, v1은 setuid류 승격 syscall 자체가 없음, §7).

- **커널이 직접 스폰하는 고정 프로세스**(`gInitProcess`/
  `gServiceProcess[]`, `SP-EAB162FC` §2.2의 `ProcessRole::
  KernelService`와 동일한 스폰 경로) - `uid=gid=0`(root)으로
  고정. 드라이버 자식(devmgr의 PnP 로더 경로)도 같은 원칙으로
  root 상속(그 경로 자체가 이미 `KernelService`를 상속하는 것과
  대칭).
- **`SpawnProcess`로 만든 일반 프로세스**(`ProcessRole::Normal`) -
  **부모의 uid/gid를 그대로 상속**(Linux `fork()`와 동일, 새
  syscall 인자 불필요 - `SpawnProcessArgs`에 uid/gid 필드를 추가
  하지 않는다). `gInitProcess`가 root이므로, 지금 이 커널에서
  스폰되는 모든 프로세스 체인은 결국 root까지 거슬러 올라간다 -
  **v1에는 "다른 사용자로 전환"하는 방법이 전혀 없다**(§7).
- **다중 그룹(supplementary groups)은 v1 범위 밖** - `gid` 하나뿐,
  Linux의 "주 그룹 + 보조 그룹 목록" 구분을 도입하지 않는다(§7).

## 2. 권한 비트 - `Permission`(재사용 가능한 범용 타입)

```cpp
// minicore/libs/libkenv, 가칭 permission.h - Process 전용이 아니라
// 앞으로 다른 자원(procfs 엔트리, ResourceGroup 등)에도 재사용
// 가능하도록 커널/유저 공용 위치에 둔다(RM-7C249618 등재 대상,
// 착수 시).
using Permission = uint16_t;

constexpr Permission kPermOwnerRead  = 1u << 8;
constexpr Permission kPermOwnerWrite = 1u << 7;
constexpr Permission kPermOwnerExec  = 1u << 6;
constexpr Permission kPermGroupRead  = 1u << 5;
constexpr Permission kPermGroupWrite = 1u << 4;
constexpr Permission kPermGroupExec  = 1u << 3;
constexpr Permission kPermOtherRead  = 1u << 2;
constexpr Permission kPermOtherWrite = 1u << 1;
constexpr Permission kPermOtherExec  = 1u << 0;
constexpr Permission kPermSpecialS   = 1u << 9;  // 의미 미정 - §7
```

POSIX `mode_t`의 하위 9비트(rwxrwxrwx) + 특수 비트 S 하나를 그대로
차용(설계자가 "linux와 유사해야" 요구한 것과 직접 대응) - Linux처럼
setuid/setgid/sticky **세 비트**를 두지 않고 **한 비트 S**만
두는 이유는 지시 원문("특수 비트 S")이 단수형이고, 이 커널에
exec 시점 uid 승격(setuid 실행) 자체가 아직 없어(§7) 그 세분화가
지금은 의미가 없기 때문 - 나중에 실제 용도가 생기면 그때 비트를
더 쪼갠다(RM-23F4B687 §4).

## 3. 범용 판정 함수 - `kCheckPermission()`

```cpp
// permission.h (이어서)
// resourceUid/resourceGid/mode = 그 자원(지금은 Process 하나뿐,
// §4)의 소유자/모드. requested = 요청하는 접근(kPermOwnerWrite류
// 하나만 - 호출부가 "이건 쓰기 요청"처럼 이미 어떤 카테고리인지
// 알고 owner/group/other 중 어느 비트를 볼지는 이 함수가 uid/gid
// 비교로 스스로 고른다).
bool kCheckPermission(Uid callerUid, Gid callerGid,
                      Uid resourceUid, Gid resourceGid,
                      Permission mode, Permission requested);
```

판정 순서(`PN-617F4E52` §5가 이미 제안한 순서 그대로 확정, 앞
단계가 통과하면 뒤 단계는 안 본다):

1. **커널/`ProcessRole::KernelService`** - 항상 허용(`SP-EAB162FC`
   §2.1이 이미 확정한 별개의 신뢰 축, uid/gid와 통합하지 않는다 -
   `PN-617F4E52` §의 "설계 시 다뤄야 할 것" 2번 항목에 대한 답:
   **통합 안 함, 병렬 유지**. 커널 서비스는 uid로 뭐든 될 수 있지만
   role 자체가 이미 무제한이라 uid 판정 자체를 아예 건너뛴다).
2. **`callerUid == kRootUid`** - 항상 허용(root 특권).
3. **조상-자손 관계** - `SP-9CB55C5B`가 이미 확정한 판정(직계
   부모부터 시작, 그 세부는 그 문서가 소유) - 통과 시 허용.
4. **uid/gid RWX 비교** - `callerUid == resourceUid`면
   `mode & (requested의 owner 비트)`, `callerGid == resourceGid`면
   `mode & (requested의 group 비트)`, 둘 다 아니면
   `mode & (requested의 other 비트)` - Linux `access()`와 동일한
   3분기 규칙.

## 4. v1의 유일한 소비자 - `Kill`

`Process`에 `Permission signalPermission`(기본값 - §7 미정, 착수
세션이 실측하며 확정) 필드를 추가한다 - "누가 이 프로세스에 신호를
보낼 수 있는가"를 이 한 비트마스크로 표현한다(Linux 자체는 실제로
`kill()`을 mode 비트가 아니라 uid 일치로만 판정하지만, 설계자가
명시적으로 RWX+S 모델을 요구했으므로 그 요구를 그대로 따른다 - "w"
비트 하나만 실질적 의미를 가진다, r/x는 Process 자원에는 아직
쓰임이 없어 항상 0으로 취급).

```cpp
// signal.h - KillHandler::onExec 갱신 (기존 process.h 819-826행의
// "children 순회 후 정확히 일치하는 것만" v1 스코프를 대체)
bool kCanSendSignal(const Process& caller, const Process& target) {
    return kCheckPermission(caller.uid, caller.gid,
                             target.uid, target.gid,
                             target.signalPermission, kPermOwnerWrite);
}
```

**전제 조건**: 이 함수가 실제로 임의 대상에 적용되려면 `target`을
안전하게 얻는 수단이 먼저 있어야 한다 - `SP-9CB55C5B`의
`kResolveProcessId()`(`PN-88E62419`, `PN-C39882D0` 답변 대기 중)가
그 전제다. 이 문서는 권한 판정 로직만 확정하고, 실제로 "임의
프로세스 대상 Kill"이 풀리는 시점은 그 체인 완료 이후다 - 그 전까지
`Kill`의 실사용 스코프는 여전히 v1(직계 자식)로 유지된다(순서
바뀌지 않음, `PN-88E62419`가 이미 이렇게 정리해 둠).

## 5. `DebugAttach`도 같은 모델 공유 - 후속 계획으로 분리

`PN-617F4E52` §의 "설계 시 다뤄야 할 것" 6번 항목 - `SP-9A6D579F`
§3.2가 이미 `kResolveProcessId` 재사용을 확정해 둔 만큼 권한 판정도
같은 `kCheckPermission()`을 재사용할 가능성이 높지만, `DebugAttach`
전용 `Permission` 필드(`Process::debugPermission`류, 별도로 둘지
`signalPermission`과 통합할지)는 이 문서에서 확정하지 않는다 -
착수 세션이 실제 소비 시점에 결정(§8 후속 계획).

## 6. procfs 자식 pid 열람 / cgroup Join 권한과의 관계

`PN-85FA4992`(procfs 자식 pid 열람)와 `SP-245D130B` §9의 2번 질문
(`ResourceGroupJoin` 권한)이 둘 다 "이 uid/gid 체계가 나오면 그걸
쓰겠다"고 이 문서를 선행 조건으로 걸어 뒀다 - 이 문서의 §3
`kCheckPermission()`이 바로 그 재사용 대상이다. 각 소비자가 구체적으로
어떤 `Permission` 필드/기본값을 쓸지는 각자의 문서가 착수 시 확정
(procfs는 "읽기"(kPermOwnerRead류) 의미로, ResourceGroupJoin은
아직 없는 개념(그룹 자체에 소유자가 없음)이라 §7이 지적하는 대로
더 넓은 후속 설계가 필요할 수 있음).

## 7. v1이 하지 않는 것 (명시적 축소 - 설계 공백 방지)

- **setuid/승격 없음**: uid를 바꾸는 방법이 전혀 없다(exec 시점
  승격도, 명시적 syscall도) - §2의 특수 비트 S는 필드만 예약해
  두고 의미를 정하지 않는다. 이 커널의 ELF 로더가 setuid 비트를
  해석하게 되는 시점(아직 로드맵에 없음)에 재검토.
- **다중 그룹(보조 그룹) 없음**: `gid` 하나뿐.
- **사용자 데이터베이스/관리 없음**: `/etc/passwd`류 개념 자체가
  없다 - uid/gid는 그냥 정수이고, 부모로부터 상속되는 것 말고는
  의미를 부여할 방법이 없다(이름↔uid 매핑, "사용자 생성" 같은
  개념은 fs 서비스가 실제로 생기고 설정 파일 개념이 생긴 뒤에나
  의미가 있다).
- **파일시스템 권한과의 통합 없음**: `SP-7CC5693A`(VFS)가 실제
  파일 모드/소유자를 가지려면 이 §2의 `Permission` 타입을 재사용할
  수 있겠지만, fs 서비스 자체가 0% 구현이라 지금 통합할 게 없다.
- **`DebugAttach` 실제 배선 없음**: §5 참고, 후속.
- **`Kill` 실제 배선도 즉시는 아님**: §4 참고, `kResolveProcessId`
  체인 완료가 선행돼야 한다.

## 8. 후속 계획 (승인 이후 등록 예정)

- `Uid`/`Gid`/`Permission` 타입 + `Process::uid/gid/signalPermission`
  필드 + `kCheckPermission()` 구현.
- `Kill`의 권한 판정을 이 함수로 교체(`kResolveProcessId` 완료 이후).
- `DebugAttach` 배선(§5).
- `RM-32D06563`(용어 및 개념)에 `Uid`/`Gid`/`Permission`/
  `kCheckPermission` 등록(CLAUDE.md 규칙 12).

