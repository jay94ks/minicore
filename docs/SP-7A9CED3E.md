# libext4 — ext4 온디스크 포맷 읽기/쓰기 라이브러리 설계

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-7A9CED3E
  status: approved
  updatedAt: 2026-09-22T04:05:42.974Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# libext4 — ext4 온디스크 포맷 읽기/쓰기 라이브러리 설계

## 1. 배경

`SP-2BCE5D60` §1이 ext4를 "주 데이터/루트 파일시스템... 최우선"으로
지목하고 §2가 `minicore/libs/libext4` 경로를 이미 지정해 뒀지만
(`RM-7C249618` "예정, 미구현") 실제 온디스크 포맷 설계는 없었다 -
`PN-452FF696`(fs 서비스)의 남은 세 블로커 중 하나. 설계자 답변
(`QU-CE388254`)이 "libext4/libswapfs/libvfat 모두 설계해야 하고"라고
명시한 데 따라 `SP-A658A124`(libvfat)에 이어 등록한다.

**libswapfs/libvfat와 같은 원칙**: ext4는 리눅스의 기존 외부 표준
온디스크 포맷이다 - 이 프로젝트가 새로 고안할 내용이 없다(CLAUDE.md
규칙4). ext4는 셋 중 스펙 자체가 가장 크고(저널링, 익스텐트,
htree, 체크섬, 64비트 확장 등 다수의 선택적 기능 플래그 조합)
**이 문서 혼자서 스펙 전체를 재현하지 않는다** - v1이 실제로
지원할 기능 부분집합을 명확히 긋고(§2), 그 부분집합의 온디스크
구조만 정확히 설계한다. **이 문서가 옮기는 필드 레이아웃은 공개된
ext4 문헌에 근거하지만, 구현 착수 시 반드시 실제 스펙(리눅스 커널
`Documentation/filesystems/ext4/` 트리 또는 `e2fsprogs`/`fs/ext4/
ext4.h` 소스)과 1바이트 단위로 대조해 재확인할 것** - 특히 슈퍼블록
확장 필드 영역(§3.1 후반부)은 필드 수가 많아 이 문서 하나만으로
정확성을 단언하지 않는다.

## 2. 스코프 — 단계적 로드맵, 목표는 ext4 전체 지원

**[정정, 2026-09-22, 설계자 opinion(`PN-22784AD4` 대상)]** 이 절의
원안은 "v1이 영구히 거부하는 기능" 목록으로 구성돼 있었으나 반려됐다
- 설계자 답변 원문: **"v1이 거부하는 기능이 없어야해. 목표는 전체
지원이야."** 즉 아래 §2.1의 항목들은 "영구 미지원"이 아니라
**"1차 증분(이 문서가 지금 구조를 확정하는 범위) 다음에 순서대로
이어지는 증분 목록"**으로 재구성한다 - 최종 목표는 ext4가 지원하는
기능 전체(64비트/메타데이터 체크섬/저널 리플레이/쿼터/htree 쓰기
유지/레거시 간접 블록 읽기 호환까지)이고, 그 전부를 하나의 문서·
계획으로 한 번에 설계하는 대신 `SP-D02C4A73`/`SP-A658A124`처럼
"검증 가능한 단위로 쪼개 순서대로 완성"하는 이 프로젝트의 표준
관례(스케줄러/AsyncTask 등 다른 대형 서브시스템도 전부 이 방식으로
진행됨)를 그대로 따른다.

ext4는 다수의 선택적 기능을 `feature_compat`/`feature_incompat`/
`feature_ro_compat` 비트마스크(§3.1)로 조합한다. `mount()`는 **이
문서(1차 증분)가 아직 구현하지 않은 필수(incompat) 기능을 만나면
"아직 지원 안 함"으로 마운트을 거부**하되, 이는 §2.2의 각 후속
증분이 완료될 때마다 순서대로 사라지는 **일시적** 제약이다(§2의
원안처럼 "이 구현의 영구적 한계"로 문서화하지 않는다).

### 2.1 1차 증분(이 문서가 지금 구조를 확정하는 범위)

- `INCOMPAT_EXTENTS`(익스텐트 기반 블록 매핑, §3.4) + `INCOMPAT_
  FILETYPE`(§3.5) - `mkfs.ext4` 기본값이라 이 프로젝트가 만드는
  이미지는 자동으로 충족.
- `inode` 크기(`s_inode_size`, 보통 256) 그대로 존중, 확장 필드
  (`i_extra_isize` 이후)는 최소한만 다룸(§3.3).
- htree 읽기(§3.5 참고 - 인덱스를 무시하고 선형 스캔해도 올바른
  전체 목록을 얻는다는 것이 htree 설계 자체의 하위호환 보장, 구현
  착수 시 실제 동작으로 재확인).

### 2.2 후속 증분 — 순서대로 이어져 "ext4 전체 지원"에 도달

각 항목은 이 문서와 별도의 후속 SP/PN으로 등록해 CLAUDE.md 규칙7을
지킨다(§2.3 참고) - 이 문서는 1차 증분의 온디스크 구조만 정확히
설계하고, 아래는 그 이후 순서와 이유만 기록한다:

1. **레거시 간접 블록 포인터 매핑(ext2/ext3 호환, `EXTENTS_FL`
   꺼진 inode)** - 오래된 이미지 읽기 호환용. 1차 증분 이후 가장
   낮은 리스크(순수 읽기 경로 추가, 기존 익스텐트 경로와 상호
   배타적이라 회귀 위험 적음).
2. **저널(jbd2) 리플레이** - 비정상 언마운트 이미지도 안전하게
   마운트하기 위해 필수. 1차 증분은 "항상 정상 언마운트"만 받아
   들이는데, 이 증분이 그 제약을 없앤다. **쓰기 시 저널을 실제로
   기록하는 것**(현재 계획은 쓰기를 저널 없이 직접 갱신 - 이것도
   "전체 지원"의 일부이므로 이 증분 또는 별도 후속에서 함께
   다룰지는 착수 세션이 판단).
3. **`RO_COMPAT_METADATA_CSUM`(CRC32C 메타데이터 체크섬)** - 읽을
   때 검증, 쓸 때 갱신 둘 다 필요. `Ext4GroupDesc32::checksum`(§3.2)
   등 이미 필드는 문서에 있으나 1차 증분은 계산/검증을 안 함 -
   이 증분이 실제 체크섬 로직을 채운다.
4. **`INCOMPAT_64BIT`(64비트 블록/그룹 카운트 확장)** - §3.1/§3.2의
   `*Lo` 필드들이 `*Hi` 대응 필드와 짝을 이루는 확장 - 큰 볼륨
   지원. 이 프로젝트의 현재 QEMU 개발 규모(32비트 블록 카운트
   상한 이내)에서는 급하지 않아 순서상 뒤로 미룸(우선순위 판단,
   영구 배제 아님).
5. **`RO_COMPAT_QUOTA`** - 사용자/그룹 디스크 쿼터. `SP-30FCC8AE`
   (uid/gid 체계)와 함께 다룰 가능성이 높음 - 그 문서와의 연계는
   착수 세션이 확인.
6. **htree 쓰기(인덱스 유지·갱신)** - 새 엔트리 추가 시 해시
   기반 구조를 실제로 갱신. 1차 증분은 읽기만 지원(§2.1) - 이
   증분이 "만들기"까지 완성한다.

### 2.3 이 문서가 지금 등록하는 것

CLAUDE.md 규칙7에 따라 위 여섯 항목 각각을 `PN-` 계획으로 등록한다
(§7 참고, 이 문서 승인과 함께 등록) - "설계 미착수" 상태로 시작해
1차 증분(`PN-22784AD4`) 완료 후 순서대로 착수 조건을 재검토한다.

## 3. 온디스크 포맷 — ext4 표준 준수

### 3.1 슈퍼블록 — 블록 장치 오프셋 1024바이트(블록 크기와 무관하게 고정)

핵심 필드만 옮긴다(전체는 확장 필드가 매우 많음 - §1의 재확인
경고 그대로):

```cpp
#pragma pack(push, 1)
struct Ext4SuperblockCore {
    uint32_t inodesCount;
    uint32_t blocksCountLo;
    uint32_t rBlocksCountLo;      // 예약 블록(root 전용) 수
    uint32_t freeBlocksCountLo;
    uint32_t freeInodesCount;
    uint32_t firstDataBlock;      // 블록 크기 1024면 1, 그 외 0
    uint32_t logBlockSize;        // 블록 크기 = 1024 << logBlockSize
    uint32_t logClusterSize;
    uint32_t blocksPerGroup;
    uint32_t clustersPerGroup;
    uint32_t inodesPerGroup;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mntCount;
    uint16_t maxMntCount;
    uint16_t magic;                // 0xEF53 - 오프셋 56(0x38)
    uint16_t state;                // EXT4_VALID_FS=1, EXT4_ERROR_FS=2
    uint16_t errors;
    uint16_t minorRevLevel;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creatorOs;
    uint32_t revLevel;             // EXT4_DYNAMIC_REV=1이어야 아래 확장 필드 유효
    uint16_t defResuid;
    uint16_t defResgid;
    // -- EXT4_DYNAMIC_REV 확장 시작 --
    uint32_t firstIno;             // 보통 11(첫 비예약 inode 번호)
    uint16_t inodeSize;            // 보통 256
    uint16_t blockGroupNr;         // 이 슈퍼블록 백업 사본이 속한 그룹 번호
    uint32_t featureCompat;
    uint32_t featureIncompat;      // §2가 검사하는 비트마스크
    uint32_t featureRoCompat;      // §2가 검사하는 비트마스크
    uint8_t  uuid[16];
    char     volumeName[16];
    char     lastMounted[64];
    uint32_t algorithmUsageBitmap;
    // -- 이후 저널/htree/64bit/checksum 등 확장 필드 다수 생략 --
    // (총 슈퍼블록 크기는 1024바이트 - 나머지는 v1이 안 읽는
    // 필드라 이 struct에 옮기지 않음, 실제 구현 시 전체 1024바이트를
    // 통째로 읽고 위 core 필드만 오프셋으로 파싱하는 방식을 권장)
};
static_assert(offsetof(Ext4SuperblockCore, magic) == 56);
#pragma pack(pop)

constexpr uint16_t kExt4Magic = 0xEF53;
constexpr uint32_t kExt4IncompatExtents = 0x40;
constexpr uint32_t kExt4IncompatFiletype = 0x2;
constexpr uint32_t kExt4Incompat64Bit = 0x80;
constexpr uint32_t kExt4IncompatJournalDev = 0x8;
constexpr uint32_t kExt4RoCompatMetadataCsum = 0x400;
constexpr uint32_t kExt4RoCompatQuota = 0x100;
constexpr uint16_t kExt4StateValidFs = 0x1;
```

**`mount()` 판별**: 오프셋 1024부터 슈퍼블록을 읽어 `magic ==
kExt4Magic` 확인(불일치 시 "이 포맷 아님", 다음 드라이버 시도).
일치하면 §2의 기능 플래그 검사(지원하지 않는 incompat/ro_compat
비트가 서 있으면 마운트 거부 - 단, "이 포맷이 아님"과는 구분되는
"이 포맷이지만 이 구현이 지원 못 함"이라 별도 에러 코드 권장) +
`state & kExt4StateValidFs` 확인(§2 저널 정책).

### 3.2 블록 그룹 디스크립터

슈퍼블록이 있는 블록 바로 다음 블록부터(블록 크기 1024면 블록 2,
그 외 블록 1) `ceil(그룹 수 * 디스크립터크기 / 블록크기)`만큼 연속
배치. v1(64bit 미지원, §2)은 32바이트 디스크립터:

```cpp
#pragma pack(push, 1)
struct Ext4GroupDesc32 {
    uint32_t blockBitmapLo;
    uint32_t inodeBitmapLo;
    uint32_t inodeTableLo;
    uint16_t freeBlocksCountLo;
    uint16_t freeInodesCountLo;
    uint16_t usedDirsCountLo;
    uint16_t flags;
    uint32_t reserved[3];
    uint16_t itableUnusedLo;
    uint16_t checksum;             // v1은 metadata_csum 미지원(§2)이라
                                    // 검증하지 않음 - 필드만 무시
};
static_assert(sizeof(Ext4GroupDesc32) == 32);
#pragma pack(pop)
```

**그룹 수** = `ceil(blocksCountLo / blocksPerGroup)`. 그룹 i의
디스크립터는 이 배열의 i번째 원소.

### 3.3 블록/inode 비트맵, inode 테이블

각 그룹 디스크립터가 가리키는 세 영역:
- `blockBitmapLo`가 가리키는 블록 1개 - 그 그룹 안 블록들의 사용
  여부(1비트씩, `libswapfs`의 런타임 비트맵과 달리 **이건 디스크에
  영구 저장되는 진짜 파일시스템 메타데이터**).
- `inodeBitmapLo`가 가리키는 블록 1개 - 그 그룹 안 inode 사용 여부.
- `inodeTableLo`부터 `ceil(inodesPerGroup * inodeSize / blockSize)`
  블록 - 고정 크기 inode 구조체(`inodeSize`, 보통 256바이트) 배열.

**inode 번호 ↔ 위치**: inode N(1-based)은 그룹
`(N-1) / inodesPerGroup`, 그 그룹 안 인덱스 `(N-1) % inodesPerGroup`
- inode 테이블 시작 + `인덱스 * inodeSize` 바이트.

### 3.4 inode 구조체 + 익스텐트 기반 블록 매핑

핵심 필드(inodeSize가 256이면 뒤쪽 확장 영역은 v1이 최소한만 사용):

```cpp
#pragma pack(push, 1)
struct Ext4InodeCore {
    uint16_t mode;          // 파일 타입(상위 비트) + POSIX 권한 비트
    uint16_t uid;
    uint32_t sizeLo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t linksCount;
    uint32_t blocksLo;       // 512바이트 섹터 단위(블록 단위 아님 - 주의)
    uint32_t flags;          // EXT4_EXTENTS_FL(0x80000) - 반드시 세팅
                              // 돼 있어야 함(§2), 아니면 레거시 간접
                              // 블록 포맷이라 v1이 거부.
    uint32_t osd1;
    uint8_t  block[60];      // flags에 EXTENTS_FL 있으면 §3.4하단
                              // 익스텐트 트리 헤더+엔트리가 인라인.
    uint32_t generation;
    uint32_t fileAclLo;
    uint32_t sizeHigh;       // sizeLo와 합쳐 64비트 파일 크기
    uint32_t obsoFaddr;
    uint8_t  osd2[12];
    uint16_t extraIsize;     // inodeSize > 128일 때만 유효
    uint16_t checksumHi;     // v1 미사용(§2)
    // extraIsize만큼 더 확장 필드(ctime_extra 등) - v1은 안 읽음
};
static_assert(offsetof(Ext4InodeCore, block) == 40);

// EXTENTS_FL이 켜진 inode의 block[60]을 해석하는 뷰
struct Ext4ExtentHeader {
    uint16_t magic;      // 0xF30A
    uint16_t entries;    // 실제 엔트리 수
    uint16_t max;        // 이 노드가 담을 수 있는 최대 엔트리 수(고정 4)
    uint16_t depth;      // 0이면 리프(아래 Ext4Extent 배열), >0이면
                          // 내부 노드(Ext4ExtentIdx 배열)
    uint32_t generation;
};
struct Ext4Extent {               // depth==0일 때(리프)
    uint32_t block;      // 이 익스텐트가 매핑하는 첫 논리 블록 번호
    uint16_t len;        // 연속 블록 수(상위 비트=uninitialized 플래그)
    uint16_t startHi;    // 물리 블록 번호 상위 16비트
    uint32_t startLo;    // 물리 블록 번호 하위 32비트
};
struct Ext4ExtentIdx {            // depth>0일 때(내부 노드)
    uint32_t block;      // 이 서브트리가 커버하는 첫 논리 블록 번호
    uint32_t leafLo;     // 자식 노드가 있는 물리 블록(하위 32비트)
    uint16_t leafHi;
    uint16_t unused;
};
static_assert(sizeof(Ext4ExtentHeader) == 12);
static_assert(sizeof(Ext4Extent) == 12);
static_assert(sizeof(Ext4ExtentIdx) == 12);
#pragma pack(pop)

constexpr uint16_t kExt4ExtentMagic = 0xF30A;
constexpr uint32_t kExt4ExtentsFl = 0x80000;
```

`inode.block[60]`은 `Ext4ExtentHeader`(12바이트) + 최대 4개의
`Ext4Extent`(리프, `depth==0`) 또는 `Ext4ExtentIdx`(내부 노드,
`depth>0`, 이 경우 해당 물리 블록을 읽으면 그 블록 시작이 다시
`Ext4ExtentHeader`+더 많은 엔트리) - 트리가 4개보다 많은 익스텐트를
필요로 하면(파일이 심하게 조각남) `depth>0`으로 외부 블록에
위임된다. **논리 블록 오프셋 → 물리 블록** 변환: `depth==0`이면
엔트리들을 순회해 `block <= 논리오프셋 < block+len`인 엔트리를
찾고 `startHi/startLo + (논리오프셋-block)`이 물리 블록. `depth>0`
이면 해당 범위를 담당하는 자식 노드로 재귀.

### 3.5 디렉터리 엔트리

디렉터리의 데이터(§3.4의 익스텐트로 매핑된 블록들)는
`ext4_dir_entry_2`(`INCOMPAT_FILETYPE`, §2 필수) 레코드의 연속:

```cpp
#pragma pack(push, 1)
struct Ext4DirEntry2 {
    uint32_t inode;
    uint16_t recLen;     // 이 엔트리가 차지하는 바이트 수(다음 엔트리
                          // 위치 = 이 필드로 계산 - 이름 뒤 패딩 포함)
    uint8_t  nameLen;
    uint8_t  fileType;    // 1=Regular 2=Dir 7=Symlink 등
    char     name[];      // nameLen바이트, 널 종단 없음
};
constexpr uint8_t kExt4FtRegFile = 1;
constexpr uint8_t kExt4FtDir = 2;
#pragma pack(pop)
```

`inode == 0`이면 "삭제됨"(그 자리의 `recLen`만큼 공간이 비어있다는
뜻 - unlink는 인접 엔트리의 `recLen`을 늘려 흡수하거나 이 엔트리
자체의 `inode`만 0으로 마킹, v1은 후자의 단순한 방식 채택). 한
블록의 엔트리들은 `recLen`을 누적해 블록 끝까지 순회(htree
디렉터리도 각 리프 블록 내부는 이 포맷 그대로 - §2 참고).

## 4. `Ext4Driver` 구현 — `FileSystemDriver` 인터페이스

`SP-2BCE5D60` §3.1 인터페이스를 그대로 상속(mount/remount/open/
close/read/write/stat/mkdir/rmdir/unlink/readdir).

```cpp
// minicore/libs/libext4/ext4.h
class Ext4Driver : public FileSystemDriver {
public:
    bool mount(BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;
    OpenResult open(const char* relPath, uint32_t relPathLen, uint32_t flags) override;
    void close(FileHandle handle) override;
    ReadResult read(FileHandle handle, uint64_t offset, void* buf, uint32_t len) override;
    WriteResult write(FileHandle handle, uint64_t offset, const void* buf, uint32_t len) override;
    bool stat(const char* relPath, uint32_t relPathLen, StatBuf* out) override;
    bool mkdir(const char* relPath, uint32_t relPathLen) override;
    bool rmdir(const char* relPath, uint32_t relPathLen) override;
    bool unlink(const char* relPath, uint32_t relPathLen) override;
    bool readdir(FileHandle dirHandle, DirEntry* out) override;

private:
    BlockDevice* device_ = nullptr;
    Ext4SuperblockCore sb_{};
    uint32_t blockSize_ = 0;       // 1024 << sb_.logBlockSize
    uint32_t groupCount_ = 0;
    bool readOnly_ = true;
    // 그룹 디스크립터 배열 - mount() 시 통째로 읽어 메모리에 유지
    // (그룹 수가 현실적 볼륨 크기에서 작으므로 항상 작음 -
    // libswapfs의 badPages 처리와 동일한 "작으니 전체 캐시" 전략).
    Ext4GroupDesc32* groupDescs_ = nullptr;
};
```

### 4.1 경로 해석

루트 inode는 **항상 inode 번호 2**(ext4 고정 관례 - `firstIno`는
"첫 비예약 사용자 inode"일 뿐 루트 자체는 예약 inode 2). 경로
세그먼트마다 현재 디렉터리 inode를 §3.4로 열어(익스텐트 순회)
그 데이터 블록들을 §3.5로 스캔해 이름이 일치하는 엔트리를 찾고,
그 `inode` 필드로 다음 세그먼트의 시작 inode 삼는다.

### 4.2 `open`/`read`/`write`

`FileHandle`에 inode 번호 + 파싱된 익스텐트 정보를 캐싱. `read`는
논리 오프셋을 §3.4로 물리 블록으로 변환해 `device_->readBlocks()`.
`write`가 파일 끝을 넘으면 새 블록을 그룹 비트맵(§3.3)에서 할당해
익스텐트 트리에 추가(단순 케이스는 마지막 익스텐트의 `len` 증가로
끝나지만, 물리적으로 연속이 아니면 새 익스텐트 엔트리 추가 - 4개
슬롯이 차면 외부 익스텐트 블록으로 트리를 확장해야 함, 이 확장
로직의 정확한 알고리즘은 구현 세션이 스펙과 대조해 확정).

### 4.3 `mkdir`/`unlink`/`rmdir`

`mkdir`: 새 inode 할당(inode 비트맵) + 새 블록 1개 할당(디렉터리
데이터, `"."`/`".."` `Ext4DirEntry2` 기록) + 부모 디렉터리에 새
엔트리 추가(§3.5, 빈 공간 없으면 부모 디렉터리에도 블록 추가) +
그룹 디스크립터의 `usedDirsCountLo` 증가. `unlink`/`rmdir`은
libvfat와 동일한 원칙(대상 엔트리 inode=0 마킹, 그 inode가 가리키던
블록들을 그룹 비트맵에서 해제, `linksCount`가 0에 도달하면 inode
자체도 비트맵에서 해제) - `rmdir`은 빈 디렉터리(`"."`/`".."`만)
확인 후 진행.

## 5. 이 문서가 확정하지 않는 것

- htree 디렉터리에 새 엔트리를 추가할 때의 정확한 정책(§2에서 이미
  "구현 세션이 확인" 단계로 미룸) - 데이터 정확성에는 영향 없으나
  다른 (e2fsck 등) 도구와의 상호운용성에 영향 줄 수 있는 부분.
- 익스텐트 트리가 4개 슬롯을 넘어 외부 블록으로 확장되는 정확한
  분할 알고리즘(§4.2) - 표준 B-tree류 분할과 유사할 것으로 예상되나
  ext4 고유의 세부 규칙은 구현 시 스펙 재확인 필요.
- 예약 블록(`rBlocksCountLo`, root 전용 여유분) 강제 정책 - v1이
  이 제약을 실제로 지킬지는 구현 세션 판단(안 지켜도 데이터
  손상은 아님 - 단지 표준이 보장하는 관리자 여유 공간 정책을
  안 따르는 것뿐).
- 타임스탬프(`atime`/`ctime`/`mtime`) 갱신 정책 - `libvfat`와 같은
  이유로 이 커널의 wall-clock 소스 확정 여부에 달려 있음.

## 6. 다른 설계와의 관계

- `SP-2BCE5D60` §3.1/§3.0 — 이 문서가 구현하는 `FileSystemDriver`/
  `BlockDevice` 인터페이스의 출처.
- `SP-D02C4A73`/`SP-A658A124` — 같은 블로커(`PN-452FF696`)를 푸는
  자매 설계(libswapfs/libvfat), 외부 표준 준수 원칙의 선례.
- `RM-7C249618` — 라이브러리 목록, 이 문서 승인 후 `libext4` 행
  갱신 필요.

