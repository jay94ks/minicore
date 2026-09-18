# authmgr 커널 서비스(사용자 신원 관리, Key-Value DB) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-CC1CF30E
  status: review
  updatedAt: 2026-09-18T02:26:37.583Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

설계자 opinion(2026-09-18, `SP-30FCC8AE` 대상): "사용자 데이터베이스/
관리는 커널 서비스 `authmgr`에 대한 별도 설계안을 작성해서 등록해줘야
결정이 가능해. 이것도 진행해줘." - `SP-30FCC8AE` §1-D("authmgr
커널 서비스 + Key-Value 데이터베이스")가 이미 상당히 구체화돼 있고
`PN-24A2B6F5`(계획, 착수 조건 대기)가 그 실행 항목까지 정리해 뒀지만,
정식 승인 대상인 **독립 SP 문서**로는 아직 존재하지 않았다 - 이
문서가 그 역할을 한다. `SP-B26CDBDD`(CPU 가중치)가 `SP-245D130B`
§0/§6에서, 방금 `SP-6A563A8F`(CPU 쿼터)가 `SP-245D130B` §3에서
분리된 것과 같은 패턴.

이 문서는 `SP-30FCC8AE` §1-B(sudo/su)/§1-C(신원 관리 API)/§1-D
(authmgr+KV DB)에 이미 기록된 설계자 결정들과 `PN-24A2B6F5`가 정리해 둔
실행 세부를 **새로 결정하지 않고 그대로 옮겨 하나의 승인 가능한
설계 문서로 재구성**한다(CLAUDE.md 규칙 4 - 명시 안 된 부분은
추측하지 않음, 전부 원 출처 인용).

## 1. 서비스 개요 - `authmgr` (6번째 커널 서비스)

`SP-8B6B8D25` §3.1 항목8이 이미 자리를 예약해 둔 6번째 커널
서비스(devmgr/fs/net/tty/pubreg와 같은 층위, 유저랜드 구동 -
`SP-EAB162FC` §2.1 커널 서비스 신뢰 축) - **사용자 신원(`UserRecord`
트리) 관리의 유일한 권위 있는 소유자**다. `SP-30FCC8AE` §1-C가
요구한 "파일시스템과 별도로, 파일시스템으로 전혀 노출되지 않는
특수 API"는 이 문서 §4의 Channel IPC 프로토콜로 구현된다 - 새 커널
syscall이 아니라 authmgr에게 보내는 메시지다(`pubreg`의 register/
query 패턴과 동일한 관례, `SP-1FBC0EEB` Channel IPC 재사용).

## 2. 저장소 - `libkvdb` (범용 Key-Value DB 라이브러리)

**"KV DB 자체를 일반화하여 `minicore/libs/libkvdb`로 구현해. 커널
구성요소가 아닌데 여기 배치되는 이유는, `authmgr`라는 `커널 서비스`의
의존성이기 때문이야."**(설계자 의견, 2026-09-17) - authmgr 전용이
아니라 범용 KV DB 라이브러리로 설계하고, 커널 코드 여부가 아니라
"커널 서비스의 의존성"이라는 자격으로 `minicore/libs`에 둔다(착수 시
`RM-7C249618`에 이 배치 원칙 자체를 명시적으로 기록 - 이 프로젝트
라이브러리 배치 관례의 확장).

- 키=`uid` 또는 `loginName`(§3의 별도 이름 인덱스), 값=`UserRecord`류
  레코드.
- 각 레코드는 §8의 `libkcrypto` AES256으로 암호화해 저장, 암호화
  키는 별도 파일에 보관(`SP-30FCC8AE` §1-A.1 정정 - "VFS 노출 금지"는
  숨김 처리가 아니라 **암호화**가 기밀성을 담당한다는 뜻, 접근 경로
  제한(§1 전용 API)과 저장 기밀성(암호화)이 동시에 적용됨).
- fs 서비스가 아직 0% 구현이라 v1은 **메모리 전용**(재부팅 시 root
  외 전부 소실) - fs가 생기는 시점에 영속화를 얹는다(후속, §9).
- 온디스크 포맷/인덱싱 전략(B-tree, 로그구조 등)은 `libkvdb` 자신을
  설계할 때 착수 세부로 확정(RM-23F4B687 §4 - 지금 과설계하지 않음).

## 3. 신원 데이터 모델 - 커널 캐시와의 역할 분담

`authmgr`(KV DB)이 유일한 권위 있는 저장소이고, 커널의
`gUserRecordCache[1024]`(`PN-B6DB692C`, `SP-30FCC8AE` §1-A.1)는
**authmgr의 read-through 캐시**다 - 대체 관계가 아니라 역할 분담
(`RM-C65F7760`의 `/sys/live/named/`가 fs 서비스 이전 단계의 임시
커널 내부 테이블인 것과 유사한 패턴):

- `kSetuid()`/`kCheckPermission()`이 조회 시 먼저 캐시를 보고,
  cache miss면 authmgr에 **비동기** Channel 질의(`co_await`)로
  채운다 - 이 프로젝트가 이미 "비동기 프레임워크 우선"(`SP-5A255B7C`)
  을 확정해 둔 것과 완전히 일치(Channel Read/Write가 이미 이렇게
  동작, 새 패턴 아님).
- **root(uid=0)만 예외** - 커널이 부팅 시 root 최소 정보를 직접
  하드코딩해 캐시에 심어 둔다(authmgr 기동 전/장애 중에도 root 판정이
  항상 성립해야 함 - Kill의 root 특권 같은 가장 근본적인 경로가
  유저랜드 서비스 가용성에 의존하면 안 됨). authmgr이 기동해 자신의
  KV DB에 보관된 진짜 root 레코드를 제공하면 **그쪽 정보로 전환**한다
  (부팅용 임시 하드코딩 → authmgr 권위 데이터로 승격, 전환 시점/방식은
  착수 시 확정).
- **로그인 명 별도 인덱스**(설계자 의견 항목5) - "로그인 명 -> uid"
  조회를 빠르게 하기 위한 별도 이름 인덱스가 필요하다는 뜻 - 정확한
  자료구조(정렬 배열 이진 탐색/해시 테이블 등)는 착수 시 확정.
- `UserRecord` 필드 자체(`{ parentUid, gid, loginName, passwordHash,
  defaultShell, lastHitTime }`)와 캐시 LRU 정책(1024개 상한, root
  축출 금지)은 `SP-30FCC8AE` §1-A.1/`PN-B6DB692C`가 이미 확정 -
  이 문서가 재정의하지 않는다.

## 4. Channel IPC 프로토콜 - Request/Response/Notification

**"authmgr와의 Channel IPC 프로토콜은 바이너리로 직렬화해서 Channel
IPC 위에서 동작하는 Request/Response/Notification 구조로 구성해.
Request 자체에 요청 구분을 넣으면 다수의 채널을 열 필요가 없지."**
(설계자 의견, 2026-09-17):

- `libjson`이 아니라 **바이너리 직렬화**.
- 3종 메시지: `Request`/`Response`/`Notification`.
- `Request`가 discriminator(요청 종류 구분 필드)를 직접 가져 **단일
  Channel**로 조회/생성/수정/삭제 등 여러 종류의 요청을 전부 처리
  (다수 채널을 열 필요 없음).

**일반화 - `libkproto`**(설계자 의견, 후속): "커널과 커널 서비스간의
IPC 등, 제어에 관한 프로토콜은 기본적으로 바이너리로 제한하고,
`minicore/libs/libkproto` 라이브러리로 분리 구현하도록 해(유저/커널
공용)." - authmgr 하나만의 프로토콜이 아니라 **커널 자신이 당사자인
모든 커널-서비스 프로토콜**(예: `SP-00CA7175`의 Tier A/B)의 공통
프레이밍/discriminator 처리를 `libkproto`로 일반화한다. "커널이
중개만 하고 실제 양 당사자는 유저 프로세스인" `pubreg`의 프로토콜
(`SP-CCACB192` libjson 채택)과는 무관 - 서로 다른 관계라 충돌 아님.

**착수 순서 고정(설계자 정정, 2026-09-17)**: "이 문서가 정하는 것은
`libkproto`(응용 계층 라이브러리)의 `부모`격이야. 저 `부모`격 구현이
없으면 `kproto` 라이브러리가 존재하는 의미가 없어." - `libkproto`는
Tier B와의 비교/통합으로 만들어지는 게 아니라, **이 문서가 구현할
authmgr 프로토콜(위 3종 메시지 + discriminator) 자체를 부모로 삼아
그걸 일반화해 뽑아내는** 라이브러리다. 착수 순서는 반드시 "authmgr
프로토콜 구현 → 그 구현을 일반화해 `libkproto`로 추출"이지, 역순이나
Tier B와의 선비교가 아니다. Tier B를 나중에 `libkproto`로 갈아탈지는
이 부모-자식 관계와 무관한 별개 질문으로 지금 범위 밖.

## 5. 장애 정책 - fail-closed 아님

**"authmgr이 죽으면 그게 다시 살아나기 전까지 캐쉬된 범위 내에서만
허가하고, 그외에 전부 `서비스 불가, 잠시후 재시도 할것`으로 응답하도록
해."**(설계자 의견, 2026-09-17):

- **캐시 히트**: authmgr 생사와 무관하게 정상 판정(`kSetuid()`/
  `kCheckPermission()`이 그대로 진행).
- **캐시 미스**: 새 에러 코드류(예: `ServiceUnavailable`/`TryAgain` -
  정확한 이름은 착수 시 기존 `ChannelError` 관례에 맞춰 확정)로 응답 -
  전면 거부(fail-closed)가 아니라 "재시도 요구"다. `SP-EAB162FC` §6
  (`essential` 재시작 정책)과 결이 맞는, 무조건 패닉/거부가 아닌
  점진적 회복 지향 설계.

## 6. 다중 그룹 - authmgr 내부에서만 관리, 커널은 모름

**"다중 그룹이 있긴 하겠지만, 그것은 커널 자체가 몰라도 되는 형태로
`authmgr`이라는 커널 서비스가 가공해서 공급할거야."**(설계자 의견,
2026-09-17) - 보조 그룹(supplementary groups)은 시스템에 실제로
존재하지만, 커널(`Process::gid` 단일 필드, `kCheckPermission()`)은
그 전체 그래프를 모른다. authmgr이 `libkvdb`에서 uid→소속 그룹들
(복수) 매핑을 추적하고, 커널에는 이미 가공된 단순 질의 결과(예: "이
uid가 이 gid에 속하는가?" boolean류)만 §4의 프로토콜로 공급한다 -
`kCheckPermission()`의 group 비교(`SP-30FCC8AE` §3 항목4)가 다중
그룹까지 반영하려면 결국 이 read-through 캐시 경로를 타야 한다는
뜻 - 정확한 질의 형태는 착수 시 확정.

## 7. sudo/su 메커니즘 (`SP-30FCC8AE` §1-B, 초안 - 착수 시 구체화)

설계자 지시: "sudo나 su를 구현하기 위하여, 특수한 권한(sudoers)에
포함된 사용자는 특정한 절차를 통해 root 권한을 얻을 수 있어(setuid
비트의 용도)." 이 문서는 확정하지 않고 착수 세션이 구체화할 항목을
정리한다:

1. sudoers류 멤버십 표현 방식 - 특정 `Gid`(§6의 다중 그룹 포함
   가능성)에 속하면 자격을 주는 방식인지, authmgr 내부의 별도
   화이트리스트인지.
2. `SP-30FCC8AE` §2의 특수 비트 `S`(Permission)가 "이 실행 파일은
   실행 시 소유자 uid로 승격"이라는 Linux setuid 비트 의미로
   확정될지.
3. 승격이 실제로 일어나는 지점 - exec() 시점이라면 이 커널의 ELF
   로더(`minicore/libs/libelf`)가 지금 전혀 모르는 새 동작이라
   `SP-6BEAE0C1`(일반 프로세스 생성 syscall)과 교차 확인 필요.
4. "특정한 절차"(sudo 명령 자체)가 syscall 레벨에서 무엇을 의미하는지
   - 승격된 uid로 새 프로세스를 실행하는지, 기존 프로세스의 uid를
   바꾸는지.
5. 인증(비밀번호 요구 시) - authmgr이 자신의 `libkvdb`에 저장된
   `passwordHash`(§8 SHA256, "algorithm:value" 형식)를 authmgr
   **자신이** 검증한다(커널이 아니라 authmgr이 인증 로직을 갖는 편이
   이 문서의 아키텍처와 일관적 - 착수 시 확정).

## 8. 암호화/해싱 - `libkcrypto` (신규 라이브러리)

이 프로젝트에 암호학적 해시/암호화 구현이 전혀 없다(`RM-7C249618`
확인 완료, 0건) - `minicore/libs/libkcrypto`(커널/유저 공용,
`libjson`/`libutf8`/`libelf`와 동일한 매크로 게이팅 패턴)를 신설한다
(착수 시 `RM-7C249618` 등재):

- **SHA256** - 패스워드 해싱. 평문 최대 64바이트(syscall/프로토콜
  인자 구조체의 고정 버퍼로 직접 반영), 저장 형식은
  `"algorithm:value"`(예: `"sha256:<64 hex문자>"`).
- **AES256** - §2의 `libkvdb` 레코드 암호화.

## 9. 범위 밖 / 후속

- `libkvdb`/`libkproto`/`libkcrypto` 자체의 상세 설계(온디스크
  포맷, 프레이밍 바이트 레이아웃, 라운드 함수 구현) - 각 라이브러리
  착수 시 별도 확정(RM-23F4B687 §4, 지금 과설계하지 않음).
- fs 서비스 연동(영속화) - fs가 0% 구현이라 후속.
- sudo/su 세부(§7) - 착수 세션이 구체화.
- authmgr 서비스 자체의 스캐폴딩(부팅 매니페스트 추가, 디렉터리
  등)은 `pubreg`의 `PN-185406F6`과 같은 패턴으로 별도 계획 분리
  (`PN-24A2B6F5` 착수 시).

## 10. 착수 조건

- `SP-30FCC8AE` 승인 - uid/gid 트리 모델(§1-A) 자체가 이 문서의
  전제.
- `SP-8B6B8D25` §3.1(6번째 커널 서비스 자리, 이미 확정).
- `SP-1FBC0EEB`(Channel IPC) - §4 프로토콜의 전송 계층. 완료.
- `PN-B6DB692C`(커널 측 `gUserRecordCache`/`kSetuid`)와는 **상호
  의존** - `PN-B6DB692C`는 이 문서(authmgr 존재)를 착수 조건으로
  대기 중이지만, authmgr의 read-through 캐시 소비자가 바로
  `kSetuid()`이므로 실제 구현 순서는 착수 세션이 "authmgr 스캐폴딩
  먼저 vs 캐시 자료구조 먼저"를 실용적으로 판단(둘 다 상대 없이는
  왕복 검증이 불가능하므로 같은 증분에서 함께 진행하는 편이 자연스러움
  - `PN-24A2B6F5` 착수 조건 절이 이미 이렇게 정리해 둠).

## 11. 검증 계획

1. authmgr 기동 - root 레코드가 커널 하드코딩 → authmgr KV DB 정보로
   정상 전환되는지.
2. 캐시 히트/미스 왕복 - 캐시에 없는 uid 조회가 실제로 authmgr에
   비동기 질의를 보내고 응답으로 캐시가 채워지는지.
3. authmgr 강제 종료 중 캐시 히트는 정상 판정, 캐시 미스는
   `ServiceUnavailable`류로 응답하는지(fail-closed 아님을 직접 확인).
4. SHA256/AES256 - 알려진 테스트 벡터(NIST 표준 벡터 등)와 일치하는지.
5. §4 프로토콜 - 여러 요청 종류가 단일 Channel + discriminator로
   정상 왕복하는지.
6. QEMU 4개 표준 시나리오 무회귀(새 커널 서비스 + 캐시 미스 시
   비동기 대기 경로가 다른 서비스 부팅 순서에 영향 없는지 포함).
