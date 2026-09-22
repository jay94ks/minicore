# fs 커널 서비스(파일시스템 드라이버: ext4/swapfs/FAT32-16) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-2BCE5D60
  status: approved
  updatedAt: 2026-09-22T04:53:27.453Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## 배경

설계자 opinion(2026-09-18, `SP-CC1CF30E` 대상): "fs 커널 서비스도
별도 문서로 설계안 작성해줘." - `SP-7CC5693A`("VFS 커널 서브시스템 —
설계 제안", approved)가 커널 측 VFS 라우팅(`MountTable`/경로 해석/
livefs/유저랜드 준비 신호)과 유저랜드 `fs` 서비스 자신의 설계(드라이버
우선순위/라이브러리 분리/공통 인터페이스/메인 시퀀스)를 한 문서에
같이 담고 있었다 - `SP-B26CDBDD`/`SP-6A563A8F`/`SP-CC1CF30E`가
확립한 것과 같은 패턴으로, **유저랜드 `fs` 서비스 자신의 설계**만
독립 문서로 분리한다. **새로 결정하는 내용은 없다** - `SP-7CC5693A`
§3/§3.1/§3.1a/§3.2/§3.3/§6이 이미 설계자 지시로 확정해 둔 내용을
그대로 옮긴다(CLAUDE.md 규칙 4).

**남는 것과 옮기는 것의 경계**: VFS 트리 구조 자체(루트, 경로 해석,
`MountTable`, syscall `Mount`/`ResolvePath`/`SignalUserlandReady`/
`WaitForUserlandReady`, livefs)는 **커널 코드**이므로 `SP-7CC5693A`가
계속 소유한다(§2-A "커널이 모든 통제권을 쥔 유저랜드 서비스" 원칙 -
라우팅/통제는 커널, 실질 처리는 fs). 이 문서는 그 마운트 지점
**안에서** 실제로 파일을 읽고 쓰는 `fs` 프로세스 자신의 내부 설계만
다룬다.

## 1. 드라이버 우선순위 (설계자 확정, 2026-09-14)

1. **ext4** - 주 데이터/루트 파일시스템. 저널링·익스텐트 기반 블록
   매핑·htree 디렉터리 등 실사용에 필요한 현대적 기능을 갖춘 표준
   리눅스 파일시스템이라 최우선.
2. **swapfs** - 이미 설계된 유저 프로세스 폴트 처리 정책이 이것을
   전제로 한다: `Process::FaultInfo`/`QU-0C4D25EF` 답변("폴트 정보를
   읽고 그걸 처리해서 프로세스를 죽이거나 swapfs에서 읽어서 페이지를
   갈아 끼워 줘야해")이 이미 "swapfs가 존재한다"고 가정 - ext4 다음
   으로 필요한 이유가 이미 코드/설계에 박혀 있다.
3. **FAT32/16** - `SP-8B6B8D25` §4의 `/boot/uefi`(ESP, UEFI 부팅
   경로용) 마운트, USB/외부 매체와의 상호운용성(대부분 FAT로 포맷돼
   배포됨) 두 용도.

## 2. 라이브러리 분리 - `libext4`/`libswapfs`/`libvfat`

설계자 지시(2026-09-15): "fs 서비스에서 ext4와 swapfs를
`minicore/libs/libext4`와 `minicore/libs/libswapfs`로 분리하여
구현하라... FAT32/16도 `minicore/libs/libvfat`으로 분리 구현하라.
이 라이브러리들은 커널과 유저랜드 양쪽에서 컴파일/참조가 가능해야
해." - 세 드라이버를 `fs` 서비스 소스 트리 안에 직접 구현하지 않고,
`libelf`/`libcpio`와 같은 패턴(커널/유저 공용, `RM-7C249618`)의
독립 라이브러리로 분리한다:

| 라이브러리 | 대상 파일시스템 |
|---|---|
| `minicore/libs/libext4` | ext4 |
| `minicore/libs/libswapfs` | swapfs |
| `minicore/libs/libvfat` | FAT32/16 |

`fs` 서비스(유저랜드)는 이 라이브러리들을 링크해 §3(공통 인터페이스)
구현을 얹는다. **"커널/유저 공용"으로 만드는 이유**(`QU-4B1B6DDE`
답변): "추후에 최적화 옵션을 만들려고" - 지금 커널이 이 파서들을
직접 호출하는 구체적 경로는 없지만, 나중에(예: swapfs 핫패스를 IPC
없이 커널에서 직접 처리) 최적화 옵션을 열어 두기 위한 선제 조치다.
v1은 `libelf`처럼 매크로로 컴파일 모드만 구분해 두면 되고, 실제로는
유저랜드 쪽만 쓰인다.

## 3. 공통 드라이버 인터페이스 - `FileSystemDriver`

**[전면 정정, 2026-09-22, `QU-08ACD701` 설계자 답변("양쪽 모두를
수정하면서 구현해")]** §3.1 원안은 평범한 동기 가상함수 인터페이스
(open/close/read/write/stat/mkdir/rmdir/unlink/readdir 9개)였으나,
`PN-22784AD4`(libext4 구현)가 실제 코드와 대조하며 이게 한 번도
코드로 존재한 적이 없다는 걸 발견했다 - fs가 `SP-43331889`로 순수
커널 `KernelThread`로 흡수된 뒤 `LiveFs`/`ProcFs`/`ResourceGroupFs`
전부 이미 `minicore/kernel/mount_table.h`의 `kernel::KernelFsDriver`
(`AsyncTaskHandler` 상속, `KernelFsOpCode` 태그 기반 비동기 op 제출)
패턴으로 구현돼 있었다. 설계자가 "양쪽 모두 수정"(`FileSystemDriver`
쪽도, 각 드라이버 쪽도)을 지시해 아래로 전면 재작성한다 - **`FileSystemDriver`는
독자적인 인터페이스가 아니라 `kernel::KernelFsDriver`를 확장하는
것으로 재정의되고**, `Ext4Driver`/`Fat32Driver`/`ExfatDriver`/
`NtfsDriver`는 옛 동기 시그니처 대신 `onExec`/`onFailure`/`onCancel`
(`AsyncTaskHandler` 프로토콜)을 구현한다.

### 3.0 공통 블록 장치 인터페이스 - `BlockDevice` — [확정, 2026-09-18, 설계자 지시]

`FileSystemDriver::mount`이 받는 `BlockDevice*`가 실제로 무엇을
노출하는지는 지금까지 "AHCI 등에서 넘어온 블록 장치(AhciBlockDevice류)"
로만 지칭되고 구체 시그니처가 없었다(`SP-C2670F69` §4 항목2가 이
공백을 명시적으로 남겨 뒀음) - 이 절이 그 인터페이스를 확정한다.
AHCI/USB/향후 NVMe 등 어떤 저장장치 드라이버든 파일시스템 드라이버
(§3.1)에게는 동일한 모양으로 보여야 하므로, `FileSystemDriver`와
마찬가지로 하드웨어 종류와 무관한 추상 인터페이스로 둔다:

```cpp
// fs 서비스 내부(유저랜드) - 파일시스템 드라이버(§3.1)가 소비하는 쪽.
// LBA(논리 블록 주소) 단위로만 이야기하고, 그 아래 AHCI/USB/NVMe 등
// 실제 전송 프로토콜은 전혀 알지 못한다.
class BlockDevice {
public:
    virtual uint32_t blockSize() const = 0;   // 보통 512 또는 4096
    virtual uint64_t blockCount() const = 0;  // 용량 = blockSize() * blockCount()
    virtual bool readBlocks(uint64_t lba, uint32_t count, void* buf) = 0;
    virtual bool writeBlocks(uint64_t lba, uint32_t count, const void* buf) = 0;
    // 컨트롤러/장치 자체 쓰기 캐시를 안정 매체까지 밀어낸다(AHCI의
    // FLUSH CACHE류 ATA 명령에 대응) - 저널링 파일시스템(ext4)의
    // 배리어/커밋 지점에서 필수.
    virtual bool flush() = 0;
    // 최적화용 힌트 - 미지원 장치/드라이버는 항상 true(아무것도 안
    // 하고 성공 처리)를 반환해도 무방하다(SSD TRIM처럼 데이터 정확성엔
    // 영향 없음).
    virtual bool trim(uint64_t lba, uint32_t count) = 0;
};

// SP-C2670F69 §3.1의 AhciPort 하나를 감싸는 첫 번째(그리고 현재
// 유일한) 실제 구현.
class AhciBlockDevice : public BlockDevice { /* SP-C2670F69 §3.1 참고, 후속 */ };
```

§3.1의 `FileSystemDriver::mount`와 §4의 `SwapBackend::mount`는 둘 다
이 인터페이스를 받는다 - 파일시스템 드라이버/swapfs 백엔드 어느 쪽도
device가 AHCI인지 USB인지 알 필요가 없다.

### 3.1 파일시스템 드라이버 인터페이스 — `kernel::KernelFsDriver` 확장

**타입/오퍼레이션은 새로 정의하지 않는다** - `minicore/kernel/
mount_table.h`가 이미 실제 코드로 확정해 둔 `kernel::VfsError`/
`FileHandle`/`OpenResult`/`ReadResult`/`VfsDirEntry`와, `KernelFsOpCode`
(Open/Close/Read/Write/Stat/Mkdir/Rmdir/Unlink/Readdir) + 그 9개에
대응하는 `KernelFsOpenArgs`/`KernelFsCloseArgs`/`KernelFsReadArgs`/
`KernelFsWriteArgs`/`KernelFsStatArgs`/`KernelFsMkdirArgs`/
`KernelFsRmdirArgs`/`KernelFsUnlinkArgs`/`KernelFsReaddirArgs`를
그대로 재사용한다(각 Args 구조체의 첫 필드가 `op`라 `onExec()`가
그 태그만으로 실제 타입을 재캐스팅해 분기 - `LiveFs`/`ProcFs`/
`ResourceGroupFs`와 완전히 동일한 관례).

`kernel::KernelFsDriver` 자신은 `mount(BlockDevice*)`가 없다(그
드라이버들은 블록 장치가 필요 없는 순수 인메모리 뷰라서) - 이
문서가 다루는 ext4/FAT류는 블록 장치가 반드시 필요하므로,
`KernelFsDriver`를 그대로 확장하는 별도 서브타입으로 그 차이를
표현한다:

```cpp
// minicore/kernel/mount_table.h의 kernel::KernelFsDriver를 그대로
// 확장 - 새 인터페이스가 아니라 기존 인터페이스에 "마운트" 개념 하나만
// 얹는다. mount()/remount()는 AsyncTaskHandler 프로토콜(onExec 등)과
// 별개로, 그 드라이버가 MountTable::mountKernel()에 등록되기 전/후에
// 동기적으로 한 번 호출되는 준비 단계일 뿐이다(파일 op 하나하나처럼
// 매번 제출되는 요청이 아님 - 그래서 일반 가상함수로 남겨 둔다).
class FileSystemDriver : public kernel::KernelFsDriver {
public:
    // readOnly: 부팅 초기 임시 읽기전용 마운트 지원(§5.1).
    virtual bool mount(BlockDevice* device, bool readOnly) = 0;   // BlockDevice 정의는 위 §3.0 참고
    // readOnly=true로 마운트된 대상을 쓰기 가능으로 전환(§5 - init이
    // /sys/etc/mtab을 읽은 뒤 호출) - 이미 writable이면 아무 효과 없이 true.
    virtual bool remount(bool writable) = 0;

    // onExec/onFailure/onCancel(AsyncTaskHandler, kernel::KernelFsDriver
    // 경유)를 각 드라이버가 구현 - args의 KernelFsOpCode 태그로 9개
    // 오퍼레이션에 분기한다(mount_table.h 관례 그대로). 이 클래스
    // 자신은 새 가상함수를 추가하지 않는다 - mount/remount 둘뿐.
};

class Ext4Driver : public FileSystemDriver {
public:
    bool mount(BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;
    void onExec(AsyncTask* task, void* args) override;   // KernelFsOpCode 분기
    void onFailure(AsyncTask* task) override;
    void onCancel(AsyncTask* task, void* args) override;
    /* 내부 상태는 각 라이브러리 문서(SP-7A9CED3E 등) 참고 */
};
// swapfs는 이 인터페이스를 구현하지 않는다 - §4의 별도 SwapBackend 참고.
class Fat32Driver : public FileSystemDriver { /* SP-A658A124 참고, 동일 패턴 */ };
```

**`read`/`write`가 명시적 `offset`을 받는 것**(POSIX `pread`/`pwrite`
스타일, `KernelFsReadArgs::offset`/`KernelFsWriteArgs::offset`)은
그대로 유효하다 - "현재 커서 위치"라는 상태는 이 인터페이스가 아니라
**커널의 파일 디스크립터 테이블**(`SP-2AAD7C8D` §9.2)이 갖는다,
드라이버 자신은 무상태(stateless) 오퍼레이션만 구현한다(이 원칙은
바뀌지 않음 - 호출 규약만 동기 반환값에서 `AsyncTask`의 out 필드로
바뀌었을 뿐).

**각 라이브러리 문서(`SP-D02C4A73`/`SP-7A9CED3E`/`SP-A658A124`/
`SP-F1987EF8`/`SP-AA6DF406`)의 §4가 이 새 패턴에 맞춰 갱신 필요** -
개별 문서에서 처리(CLAUDE.md 규칙11, "같은 설명이 다른 문서에도
복제돼 있는지 의심한다").

`fs` 서비스는 `SP-7CC5693A` §2.2의 `Mount` syscall로 커널에 마운트를
등록하면서, 동시에 내부적으로 해당 마운트 경로에 어떤
`FileSystemDriver` 인스턴스(어떤 블록 장치 + 어떤 포맷)를 연결할지
자신의 설정/슈퍼블록 감지 로직으로 결정한다 - 이 결정 로직은
**`SP-7CC5693A` §2.5의 `WaitForUserlandReady` 반환 이후에만 시작**
해야 한다(설계자 지시) - 구체 판별 절차 자체는 각 드라이버 착수
시점의 후속 설계 과제(§6 참고).

## 4. swapfs의 특수성 - `FileSystemDriver`와 분리된 `SwapBackend`

swapfs는 ext4/FAT32처럼 "디렉터리 트리를 가진 범용 파일시스템"이
아니라 **스왑 슬롯(고정 크기 블록) 배열에 가까운 단순 구조**다 -
`open`/디렉터리 개념 없이 "슬롯 번호 ↔ 물리 페이지 내용"만 오가는
훨씬 좁은 인터페이스로 충분하다.

**[확정, 2026-09-18, 설계자 opinion("swapfs는 기존의 FileSystemDriver와
별도로 분리해야 할것 같은데")]** §3의 `FileSystemDriver`를 그대로
구현하되 대부분의 메서드를 무의미(에러 반환)하게 두는 방식은 기각됐다
- swapfs는 **별도의 `SwapBackend` 인터페이스**로 분리한다. 아홉 개
파일 API 메서드(open/close/read/write/stat/mkdir/rmdir/unlink/readdir)
중 실제로 의미 있는 건 사실상 슬롯 단위 읽기/쓰기뿐이라, 억지로
`FileSystemDriver`를 구현하면 나머지 대부분이 죽은 코드(항상 에러
반환)로 남는다 - 애초에 다른 인터페이스로 분리하는 편이 정직하다:

```cpp
// fs 서비스 내부(유저랜드) - FileSystemDriver와 완전히 별개.
// "슬롯 번호 -> 물리 페이지 크기 블록" 매핑만 다룬다. 디렉터리/이름/
// 열기 상태 같은 파일시스템 개념이 전혀 없다.
using SwapSlot = uint64_t;

class SwapBackend {
public:
    // [정정, 2026-09-18, 설계자 지시] readOnly 파라미터 없음 - 스왑은
    // 절대 읽기 전용으로 마운트되면 안 된다(아래 §5.1 참고), 그래서
    // FileSystemDriver::mount(§3.1)와 달리 이 구분 자체가 없다.
    virtual bool mount(BlockDevice* device) = 0;
    // 물리 페이지 하나(PL-2D3184BC/커널 페이지 크기, 4KiB 가정)를
    // 슬롯에 기록/조회 - 파일 오프셋이 아니라 슬롯 번호로 직접 색인.
    virtual bool writeSlot(SwapSlot slot, const void* page) = 0;
    virtual bool readSlot(SwapSlot slot, void* page) = 0;
    // 빈 슬롯 할당/반납 - ext4의 mkdir/unlink에 대응하는 "공간 관리"
    // 축이지만 이름/경로 개념이 없어 훨씬 단순하다.
    virtual bool allocateSlot(SwapSlot* out) = 0;
    virtual void freeSlot(SwapSlot slot) = 0;
};

class SwapfsBackend : public SwapBackend { /* 후속 */ };
```

`Process::FaultInfo`/`QU-0C4D25EF`가 이미 확정한 소비 지점("폴트
정보를 읽고... swapfs에서 읽어서 페이지를 갈아 끼워 줘야해")은 이
`readSlot`/`writeSlot`을 그대로 호출하면 된다 - 커널의 폴트 처리
경로가 표준 파일 API(`Open`/`Read`)를 거칠 필요가 없다는 뜻이기도
하다(이 부분은 §1-A "커널/유저 공용 라이브러리" 배경과도 맞물린다 -
커널이 나중에 `libswapfs`를 직접 링크해 IPC 없이 `SwapBackend`를
호출하는 최적화 경로를 열어 두려는 것이 §2의 원래 취지였다).

## 5. `fs` 서비스 메인 시퀀스

`SP-9DD4F3EA` §6(devmgr 메인 서비스 시퀀스)과 같은 목적/형식 - 위
각 절이 다룬 조각(드라이버 우선순위/라이브러리/공통 인터페이스)을
`fs` 프로세스 하나의 실제 시작 시퀀스로 엮는다:

1. **스폰**: `kmain.cpp`의 부팅 매니페스트(`SP-EAB162FC` §2.2 -
   initrd 안의 `fs`라는 이름과 정확히 일치하는 실행 파일을 커널이
   직접 스폰, `ProcessRole::KernelService` 부여)로 `init`/`devmgr`과
   별개로 기동된다.
2. **마운트 지점 사전 등록**: `SP-7CC5693A` §2.3에 따라
   `SP-8B6B8D25` §4가 정의한 자신의 담당 경로들(`/sys/etc`,
   `/sys/bin`, `/sys/mnt`, `/sys/dev`, `/sys/tmp`, `/apps`, `/run`,
   `/usr/*`, `/home` 등 - `/sys/live`는 제외, livefs가 커널 자신의
   `mountKernel()`로 별도 등록)를 `Mount` syscall로 순서대로
   등록한다 - 이 시점엔 아직 어떤 드라이버도 연결되지 않은 "빈
   마운트 지점" 예약뿐이다.
3. **유저랜드 준비 신호 대기**: `SP-7CC5693A` §2.5의
   `WaitForUserlandReady`를 호출해 블로킹 - `init`이
   `SignalUserlandReady`를 호출(커널 전역 단 1회)할 때까지 실제
   슈퍼블록 감지를 시작하지 않는다.
4. **신호 수신 후 블록 장치 발견**: **fs가 블록 장치를 찾는 경로는
   pubreg와 무관하다**(`SP-B071E628` §5-A/§5-B 확정 - "devmgr/fs는
   둘 다 커널 서비스라 Tier B로 직접 통신하도록 이미 설계돼 있으므로
   pubreg를 거칠 필요 자체가 없었다") - 기존 PnP "장치 열거 → IO
   권한 요청" 패턴(devmgr의 `EnumerateDevices`/`RequestIoPermission`,
   `SP-9DD4F3EA` §3.1/§6)을 그대로 쓴다.
5. **슈퍼블록 감지 및 드라이버 연결**: 각 블록 장치 채널에
   `connectChannel()`로 연결한 뒤, §1의 우선순위(ext4 → swapfs →
   FAT32/16)로 §2의 라이브러리(`libext4`/`libswapfs`/`libvfat`)를
   차례로 시도해 슈퍼블록을 판별한다 - 판별에 성공한
   `FileSystemDriver` 구현(§3)을 해당 블록 장치와 함께 2단계에서
   미리 예약해 둔 마운트 지점 중 알맞은 곳에 연결한다(어떤 블록
   장치를 어떤 마운트 지점에 붙일지 판단하는 구체 규칙은 §6에 열린
   질문으로 남아 있음 - 이 절은 "언제/어떤 순서로"만 다룬다).
6. **서비스 개시**: 이제 `ResolvePathArgs`(`SP-7CC5693A` §2.2)/표준
   파일 API syscall(`SP-2AAD7C8D` §9)이 이 `fs` 서비스의 마운트들을
   실제로 라우팅하기 시작한다 - 이 시점부터 다른 유저 프로세스의
   `Open`/`Read`/`Write` 등이 정상 응답을 받는다.

### 5.1 부팅 필수 마운트와 `/sys/etc/mtab` — [확정, 2026-09-18, 설계자 지시]

위 1~6단계는 "장치가 발견된 뒤 슈퍼블록을 판별해 마운트"하는 일반
절차를 다룬다. 그런데 루트 파티션(`/`)과 `/sys` 파티션(기본적으로
루트에 통합될 수 있음, 필요하면 별도 파티션으로 분리 가능) 자체는
이 일반 절차보다 먼저, 그리고 `/sys/etc/mtab`(설정 파일)조차 아직
읽을 수 없는 시점에 마운트돼 있어야 한다 - `/sys/etc/mtab` 자체가 그
마운트 지점들 **안**에 있기 때문이다(닭이 먼저냐 달걀이 먼저냐 문제).
이를 풀기 위해 부팅을 두 단계로 나눈다:

**1단계 - 부팅 필수 마운트 (initrd 내장 설정)**: 루트 파티션(`/`),
스왑 파티션, `/sys` 파티션 이 세 곳만 initrd 안에 함께 동봉된 최소
설정 파일을 근거로 위 2단계보다 먼저 연결한다. **[확정, 2026-09-18,
설계자 지시]** 이 파일은 `/sys/etc/mtab`(2단계)과 같은 포맷(리눅스
mtab 포맷)을 쓰고, initrd는 경로 구분을 하지 않으므로(§2.2의
`devmgr`/`fs`/`net`/`tty`/`pubreg`처럼 고정 이름과 정확히 일치하는
엔트리를 찾는 방식과 동일한 평면 구조 - `SP-EAB162FC` §2.2) initrd
안에 그냥 `mtab`이라는 이름으로 최소 구성만 동봉한다 - 별도 경로
규칙이나 새 파일명은 두지 않는다. 이 중 루트/`/sys`(둘 다 `FileSystemDriver`
대상, §3.1)는 **읽기 전용**으로 마운트한다 - 이 시점엔 아직
`/sys/etc/mtab`의 실제 설정을 몰라 "쓰기 가능해야 하는지" 판단할
근거가 없기 때문에, 안전한 기본값으로 시작한다는 점에서 SP-8D206F11
§1의 캐시 정책과 같은 "정확성 우선" 원칙을 따른다.

**[정정, 2026-09-18, 설계자 지시] 스왑 파티션은 읽기 전용으로 마운트하면
안 된다** - `SwapBackend`(§4)는 스왑아웃(페이지 내용을 슬롯에 기록)이
본질적인 동작이라 읽기 전용이면 그 자체로 기능하지 않는다(파일시스템의
"나중에 쓰기 가능으로 전환"과 달리, 스왑은 처음부터 쓰기가 가능해야
의미가 있다). 그래서 `SwapBackend::mount`(§4)엔 애초에 `readOnly`
파라미터를 두지 않았다 - 스왑은 1단계에서 `SwapBackend::mount`
한 번으로 곧장 완전히 사용 가능한 상태가 되고, 2단계의 remount 절차
(아래) 대상이 아니다.

**2단계 - `/sys/etc/mtab` 기반 기본 마운팅 (init 주도)**: `/sys`가
읽기 전용으로라도 마운트되면 `/sys/etc/mtab`을 읽을 수 있게 된다 -
`init`이 이 파일을 읽어 전체 시스템의 마운트 설정(리눅스 `/etc/fstab`/
`mtab`과 같은 역할 - 장치/파티션 ↔ 마운트 지점 ↔ 옵션 매핑)을 확인하고
"기본 마운팅 작업"을 완료한다. 여기엔 두 가지가 포함된다:

1. 1단계에서 읽기 전용으로 올려 둔 루트/`/sys`(스왑 제외 - 위 §5.1
   정정 참고, 스왑은 이미 1단계에서 완전히 쓰기 가능함)를, `mtab`
   설정에 쓰기 가능으로 지정돼 있으면 `FileSystemDriver::remount(true)`
   (§3.1)로 전환한다.
2. `mtab`에 나열된 나머지 마운트 지점(위 2단계가 미리 예약해 둔
   `/sys/etc`/`/sys/bin`/`/home` 등)에 대해, 위 5~6단계의 일반
   슈퍼블록 감지/드라이버 연결 절차를 진행한다.

이로써 §6 항목3("블록 장치 ↔ 마운트 지점 매칭 규칙")이 답을 얻는다 -
**설정 파일**(`/sys/etc/mtab`, 리눅스 fstab/mtab과 같은 역할) 기준이며,
파티션 레이블이나 고정 순서가 아니다.

**커널↔커널서비스 고속 채널 결정(참고, 이미 해소됨)**: 위 2/3단계가
쓰는 `Mount`/`SignalUserlandReady`/`WaitForUserlandReady`는 `fs`가
커널 자신과 직접 주고받는 제어 트래픽이라 `DC-6E2500A6`(커널↔커널
서비스 고속 채널)의 잠재적 적용 대상이었으나, 그 결정이 "재설계
불필요 - Channel IPC에 `exclusivePreemptive` 플래그만 얹으면 충분
(Tier B)"으로 해소돼 위 시퀀스 형태 그대로 유효하다. 4단계(pubreg
무관)는 애초에 이 결정과 무관.

## 6. 아직 열려 있는 설계 영역 (fs 서비스 고유)

1. ~~**swapfs의 인터페이스 형태**~~ - **[해소, 2026-09-18, 설계자
   opinion]** 별도 `SwapBackend`로 분리 확정(§4).
2. **각 드라이버의 슈퍼블록 감지/자동 마운트 로직**: `fs` 서비스가
   블록 장치를 보고 "이건 ext4다/FAT32다"를 판별하는 절차 - 각
   드라이버 착수 시점의 구현 세부(단, 시작 시점은 `SP-7CC5693A` §2.5
   로 이미 확정됨).
3. ~~**블록 장치 ↔ 마운트 지점 매칭 규칙**(§5 5단계)~~ - **[해소,
   2026-09-18, 설계자 지시]** §5.1 참고 - 설정 파일(`/sys/etc/mtab`)
   기준으로 확정.
4. ~~**§5.1의 initrd 내장 부팅 필수 마운트 설정 파일의 경로/포맷**~~ -
   **[해소, 2026-09-18, 설계자 지시]** 리눅스 mtab과 동일 포맷,
   initrd 안에 경로 없이 `mtab`이라는 이름으로 최소 구성 동봉(§5.1).
5. ~~**§5.1에서 스왑 파티션의 "읽기 전용" 의미**~~ - **[해소,
   2026-09-18, 설계자 지시]** "스왑 파티션은 읽기 전용 마운트되면
   안 된다" - §5.1/§4를 정정해 `SwapBackend::mount`에서 `readOnly`
   파라미터 자체를 제거하고, 스왑은 1단계에서 곧장 완전히 쓰기
   가능한 상태로 연결되도록 확정했다.

## 7. 착수 조건

- `SP-7CC5693A`(VFS 커널 서브시스템, approved) - `MountTable`/
  `Mount`/`ResolvePath`/`SignalUserlandReady`/`WaitForUserlandReady`
  syscall 전부 이 문서의 전제.
- Channel IPC(`PL-C8648D4D`, 완료) - 블록 장치 채널 연결, 표준 파일
  API의 기반.
- Syscall 서브시스템(`PL-21344323`, 완료).
- 프로세스 모델(`PN-16CA347D`, 완료) + `fs` 서비스 스폰 체계
  (`PN-D3C05C0B`, 완료, commit `6fae6c1`) - 다만 `fs` 자신의 실행
  파일 내용(`minicore/fs` 실코드, `PN-452FF696`)이 아직 없어 `fs`
  서비스가 실제로 존재하는 것과는 별개 문제.
- AHCI(`SP-C2670F69`)/USB(`SP-E35FD36C`) - ext4/FAT32 드라이버가
  실제로 블록 장치를 얻는 경로.
- devmgr PnP "장치 열거 → IO 권한 요청"(`SP-9DD4F3EA` §3.1/§6) -
  §5 4단계의 블록 장치 발견 경로.
- `Process::FaultInfo`/`QU-0C4D25EF` 답변(확정) - swapfs가 실제로
  소비되는 지점(유저 폴트 처리).

## 8. 검증 계획

1. `fs` 프로세스 기동 - §5 1~3단계(스폰/마운트 사전 등록/준비 신호
   대기)가 순서대로 진행되는지, `init`의 `SignalUserlandReady` 호출
   전에는 슈퍼블록 감지가 시작되지 않는지.
2. 블록 장치 발견 - devmgr PnP 경로로 실제 AHCI 블록 장치를 찾아
   연결하는지(pubreg 경유 없이).
3. 슈퍼블록 판별 - ext4/swapfs/FAT32 각각의 알려진 이미지로 올바른
   드라이버가 선택되는지, 우선순위(ext4 → swapfs → FAT32/16)가
   지켜지는지.
4. 표준 파일 API 왕복 - 다른 유저 프로세스의 `Open`/`Read`/`Write`/
   `Stat`/`Readdir` 등이 `fs`가 마운트한 경로에서 정상 동작하는지.
5. QEMU 4개 표준 시나리오 무회귀 - `fs` 서비스 추가로 부팅 순서/
   다른 서비스에 영향 없는지.
