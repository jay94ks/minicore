# VFS 디렉토리 구성 스펙

**관련 결정**: ADR-005, ADR-008, ADR-018, ADR-022, ADR-044~050, ADR-056, ADR-058, ADR-059, ADR-065, ADR-131
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`servers/vfs`, `servers/fs`, `servers/devmgr`, `servers/procsrv`)

이 문서는 minicore가 부팅한 뒤 유저에게 보이는 **런타임 파일시스템
계층**을 정의한다. `repo-layout.md`(minicore 소스 저장소 자체의
디렉토리 구조)와는 완전히 다른 문서다 — 혼동하지 않는다.

## 1. 전체 트리

```
/
├── boot/                    # 부팅 루트 (ADR-046)
│   ├── (커널 이미지, initrd — Multiboot2 경로용, 일반 파일)
│   └── uefi/                # ESP(FAT32) 마운트 지점 — UEFI 경로 전용
│
├── sys/                     # OS를 구성하는 모든 것의 루트 (ADR-044)
│   ├── proc/                 # procsrv가 FS 서버 겸임 (ADR-047)
│   ├── live/                 # 여러 서버가 하위 경로별로 나눠 담당 (ADR-047)
│   │   ├── network/           # 예: netsrv 담당
│   │   ├── sched/              # 예: 스케줄러 노출용 서버 담당
│   │   └── ...                 # 서비스가 늘어날 때마다 하위 마운트 추가
│   ├── etc/                  # 전역 시스템 설정 (영속, 일반 FS)
│   ├── bin/                  # 전역 시스템 바이너리 (영속, 일반 FS)
│   ├── srv/                  # 코어 서버 실행파일·시동 정보 (ADR-131)
│   │   ├── bin/                # 서버 실행파일
│   │   └── lib/                # NNN-이름.ini 시동 파일(발견 순서=파일명순)
│   ├── mnt/                  # 시스템 수준 외부 매체 마운트 루트
│   ├── dev/                  # devmgr가 FS 서버 겸임 (ADR-048)
│   └── tmp/                  # memfs 마운트, 휘발성 (ADR-048)
│
├── run/                     # 공용 설치 경로 — 사용자별 관리 불가 (ADR-044)
│
├── usr/                     # 사용자별 격리 루트 (ADR-044)
│   └── {사용자명}/
│       ├── bin/               # 이 사용자 전용 바이너리
│       ├── lib/               # 이 사용자 전용 라이브러리
│       ├── etc/               # 이 사용자 전용 설정
│       └── home/              # 기본 작업 디렉터리 (= /home/{사용자명}, ADR-065)
│
└── home/                    # /usr/{사용자명}/home으로의 VFS 별칭 (ADR-065, 실체 없음)
    └── {사용자명}/            # → /usr/{사용자명}/home 으로 재작성되어 조회됨
```

## 2. 경로별 소유·마운트 방식

| 경로 | 백엔드 | 담당 서버 | 비고 |
|---|---|---|---|
| `/` | 최소 synthetic root — 마운트 지점만 제공 | vfs | 자체 콘텐츠 없음, 하위 마운트들의 합류점 |
| `/boot` | 일반 영속 FS (초기엔 memfs, ADR-008 수직 슬라이스) | fs(초기 memfs) | 커널/initrd가 직접 파일로 위치 |
| `/boot/uefi` | FAT32 (ESP) | fs(FAT32, ADR-057로 로드맵 편입 확정) | UEFI 부팅 경로에서만 필요 |
| `/sys/proc` | synthetic | procsrv (FS 서버 겸임, ADR-047) | 프로세스 상태 열람 |
| `/sys/live/*` | synthetic, 하위 경로별로 상이 | 주제별 서버 (ADR-047) | 매핑표는 ADR-058(§5) |
| `/sys/etc`, `/sys/bin` | 일반 영속 FS | fs | 시스템 전역, 사용자 쓰기 불가(권한은 별도 결정) |
| `/sys/srv/bin`, `/sys/srv/lib` | 일반 영속 FS(부팅 극초기엔 initrun이 직접 읽는 일회성 cpio 이미지, ADR-131 — Linux initramfs처럼 부트스트랩 후 다시 쓰이지 않는다) | fs | 코어 서버 실행파일(`bin`) + `NNN-이름.ini` 시동 파일(`lib`) — initrun이 이 경로 관례로 부팅 극초기 서비스를 찾는다(ADR-131 §결정5) |
| `/sys/mnt/*` | 마운트 시점에 결정 | 매체별 FS 서버 | USB=FAT/exFAT, CDROM=ISO9660 등, 로드맵은 실제 필요 시점에 정의 |
| `/sys/dev` | synthetic | devmgr (FS 서버 겸임, ADR-048) | PCIe 등에서 열거된 장치 노드 |
| `/sys/tmp` | memfs | fs(memfs) | 새 구현 불필요 — 기존 memfs 재마운트 |
| `/run` | 일반 영속 FS | fs | 공용 설치 소프트웨어 |
| `/usr/{사용자명}/*` | 일반 영속 FS (사용자별 서브트리) | fs | procsrv가 사용자 생성 시 서브트리 초기화 담당(세부는 procsrv 설계 시 정의) |
| `/home/{사용자명}/*` | 실체 없음 — `/usr/{사용자명}/home`으로 경로 재작성 | vfs (별칭 규칙만, ADR-065) | 별도 FS 서버·마운트 불필요, 순수 조회 단계 리다이렉트 |

## 3. POSIX 경로 호환 (ADR-045)

이 레이아웃은 표준 FHS와 의도적으로 다르다 — 특히 `/usr`(표준: 공용
자원)와 `/run`(표준: 휘발성 런타임)의 의미가 바뀌었다. 포팅되는
소프트웨어(ADR-005)의 하드코딩된 경로 문제는 **심볼릭 링크 호환
계층을 두지 않고**, 다음 두 가지로 대응한다:

1. `libc`의 `sysdeps/minicore`(repo-layout.md)에 표준 검색 경로 상수를
   minicore 실제 경로로 정의 — 대부분의 소프트웨어는 이 상수만
   따라가면 별도 패치 없이 동작한다.
2. 그래도 경로를 하드코딩한 소프트웨어는 `third_party/patches/<project>/`
   (ADR-022)에 개별 패치로 대응한다.
3. 패치로도 현실적이지 않을 만큼 개념 자체가 다른 소프트웨어는
   ADR-049에 따라 포팅을 포기하고 대체 구현을 검토할 수 있다.

## 4. /boot 구성 (ADR-046)

- Multiboot2 경로(ADR-009 1순위)는 `/boot` 안의 일반 파일(커널
  이미지, initrd)만으로 충분하며 별도 마운트가 필요 없다.
- UEFI 경로(ADR-017)는 `/boot/uefi`에 FAT32 ESP를 마운트해야 한다.
  이 FAT32 FS 서버는 드라이버 로드맵([boot-and-drivers.md](../design/boot-and-drivers.md)
  ADR-043)에 virtio-blk 검증 직후 착수하는 항목으로 편입되었다
  (ADR-057).
- `kernel-bootstrap.md`(M1~M8, Multiboot2 전용)에는 `/boot/uefi`가
  필요 없다.

## 5. 합성 파일시스템 (ADR-047)

`/sys/proc`, `/sys/dev`처럼 담당 서버가 명확한 단일 트리는 그 서버가
직접 ADR-018의 FS 서버 프로토콜을 구현해 VFS에 마운트로 등록한다.

`/sys/live`는 여러 서버의 상태를 한데 모아 보여줘야 하므로, **하나의
서버가 통으로 서빙하지 않고** 하위 경로마다 별도 마운트로 나눈다.
최소 매핑표(ADR-058)는 다음과 같다:

| 하위 경로 | 담당 서버 |
|---|---|
| `/sys/live/network` | netsrv |
| `/sys/live/sched` | procsrv |
| `/sys/live/mem` | 정책 서버 (ADR-024) |

"외부에서 수정하면 즉각 반영"(ADR-044) 요구사항은 각 담당 서버가
`write()` 요청을 받는 즉시 자신의 실제 상태(스케줄러 파라미터, 네트워크
설정 등)를 변경하는 것으로 만족한다 — VFS나 중간 계층의 캐싱을
거치지 않는다.

## 6. /sys/dev, /sys/tmp (ADR-048)

- `/sys/dev`: devmgr가 FS 서버를 겸해 제공한다. PCIe 열거(pcie.md)로
  발견된 장치가 여기 노드로 나타난다. 노드 이름 규칙(예:
  `/sys/dev/blk0`, `/sys/dev/net0`)은 devmgr 설계 시 정의한다.
- `/sys/tmp`: 새 구현 없이 기존 memfs(ADR-008 수직 슬라이스)를
  그대로 재마운트한다.

## 7. 부팅 시 마운트 준비 순서 (개요)

이 순서의 세부 구현(누가 언제 마운트를 등록하는지)은 initrun의 기동
매니페스트 설계(OPEN-29, boot.md 참고)와 함께 확정한다.
현재로선 다음 순서만 원칙으로 삼는다:

1. VFS 서버 기동, `/` synthetic root 준비.
2. `/boot`(및 initrd가 제공한 memfs)를 마운트 — 커널이 이미 로드한
   initrd 콘텐츠를 재사용할 수 있는지는 구현 시 정한다.
3. `/sys/etc`, `/sys/bin`, `/run`, `/usr/*` 등 영속 FS 마운트 —
   실제 디스크 드라이버(ADR-043 로드맵)가 준비되기 전까지는 전부
   memfs 위에서 시작한다.
4. `/sys/proc`, `/sys/dev`, `/sys/live/*` 등 synthetic 마운트는 각
   담당 서버(procsrv, devmgr 등)가 기동하면서 스스로 등록한다.

## 8. 사용자별 트리의 마운트 형태 (ADR-050)

`/usr/{사용자명}`은 다른 VFS 경로와 특별히 다르게 취급하지 않는다 —
기본값은 `/usr`를 구성하는 FS의 평범한 서브디렉토리이지만, 필요하면
특정 사용자의 트리를 별도 마운트(전용 볼륨, 쿼터 파티션, 네트워크
홈 등)로 대체할 수 있다. 어떤 사용자는 서브디렉토리, 어떤 사용자는
별도 마운트여도 무방하다 — 전통적 Unix에서 `/home`이 별도 파티션일
수도 루트 FS의 일부일 수도 있는 것과 같은 유연성이다.

VFS 마운트 테이블 입장에서 사용자 트리는 "이 경로가 마운트 지점인가"
검사만 받는 평범한 경로이며, 사용자 계정 생성(procsrv가 서브트리를
초기화하는 것)과 마운트 여부는 완전히 독립적이다. §2 표의 `일반
영속 FS`는 이 기본값을 뜻하며, 필요 시 그 자리에 다른 마운트가
대신 들어간다.

## 9. /sys/mnt 마운트 권한 (ADR-059)

시스템 수준 마운트(USB, CDROM, 네트워크 경로 등)는 **root 권한
프로세스의 명시적 요청**으로만 트리거된다. devmgr는 매체 삽입을
감지해 `/sys/live` 또는 `/sys/dev`에 "마운트 가능한 장치가 나타남"을
노출할 뿐, 자동으로 마운트하지 않는다 — 자동 마운트 편의 기능이
필요하면 root 권한을 가진 별도의 유저 세션 데몬이 이 경로를 대신
호출해야 한다.

## 아직 정하지 않은 것

- **OPEN-29**: initrun 기동 매니페스트 형식(어떤 서버를 언제, 어떤
  마운트·권한과 함께 띄우는지)이 확정되지 않아, 예를 들어 devmgr가
  PCIe 핫플러그 notification을 어떻게 받는지(pcie.md §3)나 `/sys/dev`,
  `/sys/proc` 등 synthetic 마운트를 실제로 언제 누가 등록하는지의
  세부는 이 항목이 풀려야 완전히 정의된다.
