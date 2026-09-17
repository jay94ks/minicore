# Minicore 사용자/권한 체계(uid/gid + RWX + root) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-30FCC8AE
  status: review
  updatedAt: 2026-09-17T04:33:57.305Z
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

## 1-A. 사용자 신원 트리 - Linux와의 결정적 차이 (2026-09-17, 설계자
의견)

> "Linux 커널과는 다르게, 특정 사용자가 그 하위 사용자를 가질 수
> 있는 트리 구조로 만들거야.
> 1. root는 모든 사용자의 프로세스에 대해 setuid를 할 수 있어.
> 2. 본인 아래의 하위 사용자들의 프로세스에 대해 setuid는 할 수 있어.
> 3. 본인보다 상위 사용자의 프로세스에 대해 setuid는 할 수 없어.
> 4. sudo나 su를 구현하기 위하여, 특수한 권한(sudoers)에 포함된
>    사용자는 특정한 절차를 통해 root 권한을 얻을 수 있어.
>    (setuid 비트의 용도) --> 추가 설계 필요.
> 5. 이러한 신원 인증 트리를 관리하는 시스템은 `파일 시스템`과
>    별도로 `파일 시스템`으로 전혀 노출되지 않는 특수한 API들을
>    필수 요건으로 갖춰져야 해. --> 추가 설계 필요."

Linux는 uid를 평평한 정수 공간으로 취급하고 "누가 누구로 setuid할
수 있는가"를 uid 자체가 아니라 별도 권한(capability `CAP_SETUID`)
으로 판정한다 - 이 프로젝트는 **uid 공간 자체가 트리**라는 점에서
근본적으로 다르다. **프로세스 트리(`Process::parent`/`children`)와
사용자 트리는 서로 다른 축**이다(§1의 uid/gid 필드가 프로세스별로
붙는 것과 별개로, uid 값들 사이에도 독립적인 부모-자식 관계가
존재) - `Process::parent`가 "이 프로세스를 누가 스폰했는가"를
답한다면, 사용자 트리는 "이 uid를 누가 발급/관리하는가"를 답한다.

```cpp
// 사용자 신원 자체의 트리 구조 - Process와 별개의 레코드.
// 실제 저장/관리 메커니즘은 §1-C(항목5)가 다루는 후속 설계 영역 -
// 여기서는 논리적 모델과 판정 규칙만 확정한다.
struct UserRecord {
    Uid uid;
    Uid parentUid;  // root(uid=0)는 자기 자신을 가리켜 트리 루트를 표시
};
```

**[정정, 2026-09-17, 설계자 의견]** "kCanSetuid는 kSetuid로
통합하고, 할 수 있는지 없는지는 질의할 수 없어. 시도해서 안되면
포기하게 만들어야 해." - 원안의 순수 판정 함수 `kCanSetuid()`(질의
전용, 부작용 없음)를 폐기하고, **판정과 실행을 한 번에 하는
`kSetuid()`**로 통합한다 - "할 수 있는지 미리 물어보고, 되면 그
다음에 실행"하는 2단계 API 자체를 없애는 것이 목적이다(TOCTOU류
경쟁/정보 유출 표면을 원천 차단 - 이 프로젝트가 이미 다른 곳에서
확립한 "질의 따로/실행 따로 대신 원자적 시도-실패" 패턴과 결이
같다, 예: `MutexLock`이 "잠글 수 있는지"를 먼저 안 물어보고 바로
시도하는 것과 동일한 철학).

```cpp
// setuid 시도 - 판정과 실행이 한 함수 안에서 원자적으로 일어난다.
// 실패하면(root도 아니고 targetUid가 callerUid의 하위도 아니면)
// 대상 Process의 uid는 전혀 바뀌지 않고 에러만 반환 - "미리 물어볼"
// 방법 자체가 없다.
ChannelError kSetuid(Process& caller, Uid targetUid) {
    if (caller.uid != kRootUid &&
        !kIsDescendantUser(targetUid, caller.uid)) {  // 항목 1~3 판정
        return ChannelError::PermissionDenied;
    }
    caller.uid = targetUid;  // 성공 - 호출자 자신의 uid를 targetUid로 전환
    return ChannelError::None;
}
```

`kIsDescendantUser(targetUid, ancestorUid)`는 `targetUid`에서
`parentUid`를 따라 거슬러 올라가며 `ancestorUid`에 도달하는지 확인한다
(도달 전에 root에 닿으면 거짓 - `ancestorUid`의 하위가 아니라는 뜻) -
`kSetuid()` 내부 구현 세부로만 남고 별도 공개 API가 아니다. **이
판정은 §3의 `kCheckPermission()`(RWX 기반, Kill 등 syscall별 개별
자원 접근 판정, 질의 전용으로 유지 - 이번 정정 대상 아님)과 별개의
메커니즘**이다 - `kCheckPermission`은 "이 프로세스에 무엇을 할 수
있는가"를 순수 질의하고(Kill이 실제로 신호를 보내기 **직전**의
판정이라 "시도-실패"와 이미 같은 성격), `kSetuid`는 "이 uid로
전환을 시도"까지 함께 한다. 둘 다 root 특권을 공유하지만 합쳐지지
않는다.

### 1-A.1 `UserRecord` 실제 필드/캐시 구조 (2026-09-17, 설계자 의견,
PN-B6DB692C 대상)

> "1. 신원 관리에 있어서, UID와 GID는 정수형(32비트)으로만.
> 2. UserRecord를 커널에서 캐시하고 있어야 하고, Root 사용자는
>    캐시에 항상 있어야 해.
> 3. 캐시의 최대 값은 1024개, 빈 슬롯이 필요하면 마지막 Hit 시간을
>    기록해두고, Hit 시간이 가장 오래된 것을 대체하는 방식.
> 4. 사용자 정보는 { 부모 유저, 소속 그룹, 로그인 명, 패스워드
>    해시(알고리즘:값 형태로 저장) 기본 쉘 } - SHA256이 지원되어야
>    하고, 해시 하기 전의 패스워드 최대 길이는 64자야.
> 5. 로그인 명은 별도로 모아서 저장되어야 해.
> 6. 이 정보들이 최종적으로 파일시스템에 저장되긴 해야 하지만, VFS상
>    노출은 없어야 해."

`Uid`/`Gid`는 이미 §1에서 `uint32_t`로 확정돼 있어 항목1은 재확인.
나머지는 `UserRecord`를 구체화한다:

```cpp
// permission.h 또는 별도 user.h - 캐시 항목 하나.
struct UserRecord {
    Uid uid;
    Uid parentUid;      // §1-A의 사용자 트리
    Gid gid;            // 소속 그룹(v1은 단일 gid, §7 "다중 그룹 없음" 유지)
    char loginName[32];  // 정확한 길이는 착수 시 확정 - §1-A.1 항목5로 "로그인
                          // 명 -> uid" 역방향 조회는 별도 인덱스가 맡으므로
                          // 여기 있는 사본은 순방향(uid -> 이름) 표시용.
    char passwordHash[72];  // "algorithm:value" 형식(예: "sha256:<64 hex문자>"),
                             // 정확한 버퍼 크기는 "sha256:" 접두사(7) + SHA256
                             // hex 표현(64) + null(1) = 72 이상.
    char defaultShell[64];  // 정확한 길이는 착수 시 확정.
    uint64_t lastHitTime;   // §1-A.1 항목3의 LRU 대체 정책이 쓰는 타임스탬프.
};

constexpr uint32_t kMaxUserRecordCacheSize = 1024;
UserRecord gUserRecordCache[kMaxUserRecordCacheSize];
// root(uid=0)는 캐시에서 절대 축출되지 않는다(항목2) - LRU 후보에서
// 항상 제외하거나, 인덱스 0을 root 전용으로 고정 배정하는 두 방식
// 중 착수 시 더 단순한 쪽으로 확정(RM-23F4B687 §4).
```

**로그인 명 별도 저장(항목5)**: `UserRecord::loginName`과 별개로,
"로그인 명 -> uid" 조회를 빠르게 하기 위한 **별도 이름 인덱스**가
필요하다는 뜻으로 해석 - 정확한 자료구조(정렬 배열 이진 탐색, 별도
해시 테이블 등)는 착수 시 확정(§1-C, PN-24A2B6F5).

**SHA256/AES256 - 새 라이브러리 `libkcrypto`(2026-09-17, 설계자
의견으로 이름 확정)**: 이 프로젝트에 지금 암호학적 해시/암호화
구현이 전혀 없다(`RM-7C249618` 확인 완료, 0건) - `minicore/libs/
libkcrypto`(커널/유저 공용, `libjson`/`libutf8`/`libelf`와 동일한
`MINICORE_LIBKCRYPTO_KERNEL`류 매크로 게이팅 패턴)로 신설한다
(RM-7C249618 등재 대상, 착수 시). SHA256(패스워드 해시)과
AES256(§1-C의 UserRecord 레코드 암호화) 둘 다 이 라이브러리가
담당. 패스워드 해싱 전 평문 최대 64바이트 제약(항목4)은 syscall
인자 구조체의 고정 버퍼 크기로 직접 반영.

**파일시스템 저장, VFS 비노출(항목6)**: §1-C(파일시스템과 별도의
전용 API)의 "파일시스템으로 노출 안 됨"과 "최종적으로는 파일시스템에
저장됨"이 동시에 참이어야 한다는 뜻 - VFS **마운트/경로**로는 절대
접근 불가능하지만, 내부적으로는 fs 서비스가 관리하는 저장 블록을
쓴다(VFS 트리에 잡히지 않는 fs 서비스 전용 영역, 이 커널에 아직
그런 개념 자체가 없다 - fs 서비스가 0% 구현이라 §1-C가 이 부분을
계속 후속으로 남겨 둔다). v1은 fs가 없으므로 이 영속화 자체가
불가능 - **메모리 내 캐시만으로 시작**(재부팅 시 root 외 전부
소실)하고, fs가 생기는 시점에 영속화를 얹는다(§1-C 후속).

**[정정, 2026-09-17, 설계자 의견]** "VFS 노출 금지"는 파일 자체를
숨김 처리(존재를 안 보이게)하라는 뜻이 아니었다 - 실제 요구사항은
**저장 데이터를 AES256으로 암호화**하는 것이다: `UserRecord`들을
바이너리로 직렬화해 각 레코드("줄")를 AES256으로 암호화해 보관하고,
암호화 키는 **별도 파일**에 저장한다. 즉 §1-C의 "전용 API, 파일시스템
비노출"은 **접근 경로**(일반 `Open`/`Read` syscall로 이 데이터를
열 수 없고, 반드시 전용 `UserRecord*` syscall을 거쳐야 함)에 대한
요구이고, **저장 자체의 기밀성**은 암호화가 맡는다 - 두 요구사항이
동시에 적용된다(경로 제한 + 암호화, 어느 한쪽으로 대체되지 않음).
이건 §2의 SHA256뿐 아니라 **AES256도 필요**하다는 뜻이라 새
crypto 라이브러리의 범위가 넓어진다(아래 §1-C 갱신).

## 1-B. sudo/su 메커니즘 (항목4) - 후속 설계 필요

설계자가 명시적으로 "추가 설계 필요"라고 표시한 항목 - 이 문서는
확정하지 않고 후속 계획으로 분리한다(§8). 다뤄야 할 것(초안):
sudoers류 멤버십을 어떻게 표현할지(예: 특정 `Gid`에 속하면 자격,
또는 별도 화이트리스트), §2의 특수 비트 S가 "이 실행 파일은 실행
시 소유자 uid로 승격된다"는 Linux setuid 비트 의미로 확정될지,
그 승격이 일어나는 지점(exec() 시점 - 이 커널의 ELF 로더가 아직
이런 승격을 전혀 모름, `SP-6BEAE0C1`과 교차 확인 필요).

## 1-C. 사용자 신원 관리 API (항목5) - 후속 설계 필요, 파일시스템
노출 금지

설계자가 명시적으로 요구한 제약: `UserRecord` 트리를 만들고/조회하고
관리하는 수단은 **VFS 경로로 노출되지 않는 전용 API**여야 한다(Linux의
`/etc/passwd` 파일 기반 모델과 정반대 - "파일 시스템과 별도로,
파일 시스템으로 전혀 노출되지 않는"). 이 문서는 확정하지 않고 후속
계획으로 분리한다(§8). 다뤄야 할 것(초안): 이 프로젝트가 이미 가진
"파일시스템이 아닌 전용 API" 선례(Channel IPC의 `/sys/live/named/`는
사실 VFS처럼 보이지만 실제로는 커널 내부 이름 테이블이었던 것과
달리, 이건 그조차도 아닌 순수 syscall 집합이어야 한다는 뜻으로 해석)
- `UserRecordCreate`/`UserRecordQuery`류 신규 syscall 그룹
(`SP-E9B44929`의 그룹+call 체계로 번호 배정), 그리고 **이 API 자체를
누가 호출할 수 있는지**(새 사용자 생성 권한 - 아마도 root 또는
그 상위 사용자가 자기 하위에 새 사용자를 만드는 구조, §1-A와 정합성
필요) 확정.

## 1-D. `authmgr` 커널 서비스 + Key-Value 데이터베이스 (2026-09-17,
설계자 의견, PN-24A2B6F5 대상)

> "신원 관리의 전속 처리를 위한 `authmgr`라는 `커널 서비스`를
> 추가로 설계하자." / "단순하게 UserRecord들을 바이너리로
> 직렬화하는 것이 아니라 Key-Value 데이터베이스를 구현해야 하는
> 과제라고 해석해."

**§1-C가 다루던 "신원 관리 API"의 소유자가 확정됐다** - 커널 자신이
아니라 `pubreg`와 같은 층위의 **6번째 커널 서비스 `authmgr`**
(`SP-8B6B8D25` §3.1 항목8, 유저랜드 구동/§2-A 원칙 그대로)이다.
`UserRecord` 저장은 단순 바이너리 직렬화가 아니라 **실제 Key-Value
데이터베이스**(키=uid 또는 loginName, 값=UserRecord류 레코드) 구현
과제로 재정의됐다.

### 아키텍처 재정리 - 커널 캐시(§1-A.1) vs authmgr KV DB(신규)의 역할 분담

`PN-B6DB692C`가 이미 설계한 `gUserRecordCache[1024]`(커널 상주,
LRU)와 이번에 확정된 authmgr의 KV DB(영속·AES256 암호화)는
**서로 대체 관계가 아니라 역할이 다르다** - 이 프로젝트에 이미
있는 "빠른 kernel-resident 캐시 + 느리지만 권위 있는 유저랜드
서비스" 분리 패턴(예: `RM-C65F7760`의 `/sys/live/named/`가 fs
서비스 이전 단계의 임시 커널 내부 테이블인 것과 유사한 결)을
그대로 적용한다:

- **`authmgr`(KV DB)가 유일한 권위 있는 저장소(source of truth)** -
  생성/수정/삭제는 전부 authmgr을 거친다. §1-C의 "파일시스템 별도
  전용 API"는 결국 **authmgr에게 보내는 Channel IPC 메시지**로
  구현된다(`pubreg`의 register/query 패턴과 동일 - 새 커널
  syscall/메커니즘 불필요, `SP-1FBC0EEB` Channel IPC 재사용).
- **`gUserRecordCache[1024]`(커널 상주)는 authmgr의 read-through
  캐시** - `kSetuid()`/`kCheckPermission()`이 조회할 때 먼저 이
  캐시를 보고, 없으면(cache miss) authmgr에 비동기 Channel 질의를
  보내 채운다. 이 프로젝트가 이미 "비동기 프레임워크 우선"
  (`SP-5A255B7C`)을 커널 전체 설계 원칙으로 확정해 뒀으므로,
  `KillHandler::onExec` 같은 `AsyncTaskHandler`가 `co_await`로
  authmgr 응답을 기다리는 것 자체는 이 코드베이스의 표준 패턴과
  완전히 일치한다(Channel Read/Write가 이미 이렇게 동작). **root만
  예외**(§1-A.1 항목2, 캐시에 항상 존재) - 커널 자신이 root(uid=0)의
  최소 정보(적어도 "root는 존재하고 모든 권한을 가짐")를 authmgr
  기동 전부터도 알아야 하므로, root 항목은 부팅 시 커널이 직접
  하드코딩해 캐시에 심어 둔다(authmgr 기동 실패/지연에도 root 판정은
  깨지지 않아야 함 - Kill의 root 특권 같은 가장 근본적인 경로가
  유저랜드 서비스 가용성에 의존하면 안 된다는 판단, 착수 시 확정
  필요).
- **Tier A/B(`SP-00CA7175`) 공유메모리 채널은 v1에서 굳이 쓰지
  않는다** - 위 read-through 캐시+비동기 질의 패턴이 이미 이
  프로젝트의 async-first 철학과 기존 Channel IPC만으로 충분하다고
  판단했다(RM-23F4B687 §4 과설계 방지) - 실측으로 지연이 문제가
  되면 그때 Tier A류 최적화를 재검토.

### KV DB 자체의 설계 (authmgr 내부, 커널 범위 밖) - `libkvdb`로
일반화 (2026-09-17, 설계자 의견)

authmgr이 유저랜드 프로세스이므로 이 KV DB 자체는 **커널 코드가
아니다** - `libkcrypto`(§1-A.1)를 링크해 각 레코드를 AES256으로
암호화해 저장한다. **"KV DB 자체를 일반화하여 `minicore/libs/
libkvdb`로 구현해. 커널 구성요소가 아닌데 여기 배치되는 이유는,
`authmgr`라는 `커널 서비스`의 의존성이기 때문이야."** - `libkvdb`는
authmgr 전용이 아니라 **범용 Key-Value DB 라이브러리**로 설계하고
(다른 커널 서비스도 나중에 재사용 가능), `minicore/libs`(보통
커널/유저 공용 또는 커널 전용이 있는 자리)에 두는 이유는 순수
"커널 서비스(kernel service)의 의존성"이라는 자격이지 커널 코드
여부가 아니다 - `RM-7C249618`에 이 배치 원칙을 명시적으로 기록해
둔다(아래 §참고, minicore/libs가 "커널 서비스" 의존성까지 포괄한다는
것은 이 프로젝트 라이브러리 배치 관례의 중요한 확장이라 `RM-7C249618`
서두에도 남긴다). 실제 온디스크 포맷/인덱싱 전략(B-tree, 로그구조
등)은 `libkvdb` 자신의 설계 시 착수 세부(RM-23F4B687 §4).

### 아직 열려 있던 것 - authmgr 장애/root 처리 정책 확정 (2026-09-17,
설계자 의견)

1. **[확정]** root 캐시 항목: "root는 커널에 하드코딩 하고,
   `authmgr`에 보관된 정보로 전환하는 방식." - 부팅 시 커널이
   root(uid=0)의 최소 정보를 직접 하드코딩해 두되(authmgr 기동
   전에도 root 판정이 항상 성립), **authmgr이 기동해 자신의 KV DB에
   보관된 진짜 root 레코드를 제공하면 그쪽으로 전환**한다(부팅용
   임시 하드코딩 → authmgr 권위 데이터로 승격). 전환 시점/방식
   (authmgr이 기동 직후 스스로 커널에 알리는지, 커널이 첫 조회 때
   확인하는지)은 착수 시 `PN-24A2B6F5`가 확정.
2. **[확정]** authmgr 장애 시 정책: "authmgr이 죽으면 그게 다시
   살아나기 전까지 캐쉬된 범위 내에서만 허가하고, 그외에 전부
   `서비스 불가, 잠시후 재시도 할것`으로 응답하도록 해." -
   **fail-closed(무조건 거부)가 아니라 "캐시 히트는 정상 판정,
   캐시 미스는 명시적 재시도 요구"** - `kSetuid()`/
   `kCheckPermission()`이 캐시에 있는 항목은 authmgr 생사와 무관하게
   그대로 판정하고, 캐시에 없는 조회만 새 에러 코드류(예:
   `ServiceUnavailable`/`TryAgain` - 정확한 이름은 착수 시 기존
   `ChannelError` 관례에 맞춰 확정)로 응답한다 - 이 프로젝트의 다른
   서비스 장애 정책(`SP-EAB162FC` §6 `essential` 재시작)과 결이
   맞다(무조건 커널 패닉/거부가 아니라 점진적 회복 지향).
3. **[확정, 2026-09-17, 설계자 의견, 이후 `libkproto`로 일반화]**
   Channel IPC 프로토콜: "authmgr와의 Channel IPC 프로토콜은
   바이너리로 직렬화해서 Channel IPC 위에서 동작하는 Request/
   Response/Notification 구조로 구성해. Request 자체에 요청 구분을
   넣으면 다수의 채널을 열 필요가 없지." - `libjson`이 아니라
   **바이너리 직렬화** 채택, 3종 메시지(Request/Response/
   Notification), Request 자신이 요청 종류 discriminator를 가져
   단일 Channel로 다 처리. **[일반화, 2026-09-17, 후속 설계자
   의견]** "커널과 커널 서비스간의 IPC 등, 제어에 관한 프로토콜은
   기본적으로 `바이너리`로 제한하고, `minicore/libs/libkproto`
   라이브러리로 분리 구현하도록 해. (유저/커널 공용으로 구현)" -
   authmgr 하나만의 프로토콜이 아니라 **커널이 직접 당사자인 모든
   커널-서비스 프로토콜**(Request/Response/Notification 프레이밍,
   discriminator 처리 등 공통 기계 장치)이 `libkproto`(커널/유저
   공용, `libjson`/`libutf8`/`libkcrypto`와 동일한 매크로 게이팅
   패턴)로 일반화된다. **범위 명확화**: 이건 "**커널 자신**이
   당사자인" 프로토콜(예: `SP-00CA7175`의 Tier A/B, authmgr의 이
   Request/Response/Notification)에 적용되지, "커널이 중개만 하고
   실제 양 당사자는 유저 프로세스인" 프로토콜(예: `pubreg`의
   tool 등록/조회 - 등록자/조회자 모두 임의 유저 프로세스, `SP-CCACB192`
   `libjson` 채택 이미 확정)과는 무관하다 - 서로 다른 관계라 충돌
   아님. **열린 질문(결정 안 함)**: `SP-00CA7175` Tier B처럼 이미
   구현된 커널-서비스 프로토콜(raw struct 직접 사용)을 `libkproto`
   등장 이후 소급 리팩터링할지는 이 문서가 결정하지 않는다 - 별도
   판단 필요(과설계/불필요한 리스크 가능성도 있어 임의로 정하지
   않음, RM-23F4B687 §4).

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

- **setuid *권한 판정*은 §1-A로 확정됐지만, 실제로 그걸 호출하는
  경로(syscall/API)는 아직 없다**: `kSetuid()`가 "누가 누구로
  전환할 수 있는가"를 시도-실패 방식으로 판정하지만, 그 소비 지점
  (sudo/su 메커니즘, §1-B)과 uid 트리 자체를 관리하는 API(§1-C)
  둘 다 설계자가 "추가 설계 필요"로 명시해 이 문서 범위 밖으로
  분리됐다(§8 후속 계획). §2의 특수 비트 S도 §1-B가 확정되기 전까지
  의미를 정하지 않는다.
- **커널 자신은 다중 그룹(보조 그룹)을 모른다** - `Process::gid`는
  여전히 단일 필드(주 그룹만). **[정정, 2026-09-17, 설계자 의견]**
  "다중 그룹이 있긴 하겠지만, 그것은 커널 자체가 몰라도 되는 형태로
  `authmgr`이라는 커널 서비스가 가공해서 공급할거야." - 즉 보조
  그룹 자체는 시스템에 실제로 존재한다(v1에서 완전히 빠지는 기능이
  아니다), 다만 그 전체 그래프(한 uid가 속한 모든 그룹 목록)를
  커널이 직접 알 필요가 없다는 뜻 - `authmgr`(§1-D)이 내부 KV DB에서
  다중 그룹 소속을 추적하고, 커널이 필요로 하는 건 "이미 가공된"
  단순한 답(예: "이 uid가 이 gid에 속하는가?" 같은 boolean류
  질의 결과)뿐이다. `kCheckPermission()`의 group 비교(§3 항목4)가
  다중 그룹까지 반영하려면 결국 §1-D의 authmgr read-through 캐시
  경로를 타야 한다는 뜻 - 정확한 질의 형태(단일 gid 비교로 충분한
  v1과, "다중 그룹 포함 여부"까지 authmgr에 위임하는 확장판의 경계)
  는 착수 시 `PN-24A2B6F5`가 확정.
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
- `UserRecord` 트리 + `kSetuid()` 구현(§1-A) - **`PN-B6DB692C`**
  (등록 완료).
- sudo/su 메커니즘(§1-B, 특수 비트 S의 실제 의미) + 사용자 신원
  관리 API(§1-C, 파일시스템 비노출) - **`PN-24A2B6F5`**(등록 완료,
  두 항목을 한 계획으로 묶음 - 서로 의존적이라 분리가 부자연스러움).
  `PN-B6DB692C`가 이 계획을 선행 조건으로 대기 중(plan_depend 등록
  완료).
- `RM-32D06563`(용어 및 개념)에 `Uid`/`Gid`/`Permission`/
  `kCheckPermission`/`UserRecord`/`kSetuid` 등록(CLAUDE.md 규칙 12).

