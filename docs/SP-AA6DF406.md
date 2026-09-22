# libntfs — NTFS 온디스크 포맷 읽기 라이브러리 설계 (1차 증분: 읽기 전용)

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-AA6DF406
  status: approved
  updatedAt: 2026-09-22T04:43:50.443Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# libntfs — NTFS 온디스크 포맷 읽기 라이브러리 설계 (1차 증분: 읽기 전용)

## 1. 배경

설계자 opinion(`SP-A658A124`/libvfat 대상, 2026-09-22): "exfat과
ntfs도 지원해야해." - `SP-F1987EF8`(libexfat)에 이어 NTFS를 지원하는
새 라이브러리를 등록한다. NTFS는 Microsoft의 기존 외부 표준(공개
문서화는 스펙 원본만큼 완전하지 않지만 리눅스 `ntfs-3g`/`ntfs3`
드라이버로 수십 년간 리버스엔지니어링·검증된 사실상 표준 레이아웃이
존재) - 이 프로젝트가 새로 고안할 내용이 없다(CLAUDE.md 규칙4).

**이 문서는 셋 중(ext4/FAT/exFAT) 가장 복잡하고 이 문서 하나의
신뢰도가 가장 낮은 영역이다** - MFT(Master File Table) 레코드
구조, 속성(attribute) 인코딩, 데이터 런(data run) 압축 인코딩,
B+ 트리 인덱스 등 필드 수/중첩 구조가 훨씬 많다. **이 문서가 옮기는
필드 레이아웃은 공개적으로 잘 알려진 리버스엔지니어링 문헌(리눅스
`ntfs-3g`/`ntfs3` 소스, `linux-ntfs` 프로젝트 문서 등) 수준의
신뢰도이지, 1바이트 단위로 검증된 공식 스펙이 아니다 - 구현 착수
시 반드시 그 소스들과 실제 대조·실측 검증할 것**(다른 세 문서보다
한 단계 더 강한 재확인 경고 - CLAUDE.md 규칙4).

**이런 이유로 1차 증분을 의도적으로 읽기 전용으로 좁힌다**(§2) -
쓰기는 MFT 비트맵 할당/속성 확장(attribute list)/B+ 트리 재조정
등 실수 시 실제 손상 위험이 있는 영역이 많아, 구조를 실측으로 먼저
검증한 뒤 쓰기로 나아가는 것이 안전하다. 이것은 "영구 미지원"이
아니라 `SP-7A9CED3E`/`SP-A658A124`가 이미 확립한 "단계적 증분"
원칙을 위험도가 더 높은 포맷에 맞게 적용한 것뿐이다.

**신규 라이브러리 등록**(CLAUDE.md 규칙8): `minicore/libs/libntfs`
- `RM-7C249618`에 이 문서 승인과 함께 행 추가.

## 2. 스코프 — 단계적 로드맵

**1차 증분(이 문서가 지금 구조를 확정하는 범위, 읽기 전용)**:
- 부트 섹터(§3.1), MFT 레코드 헤더 + fixup(수정 시퀀스) 검증(§3.2),
  상주(resident)/비상주(non-resident) 속성 공통 헤더 파싱(§3.3),
  데이터 런 디코딩(§3.4), `$STANDARD_INFORMATION`/`$FILE_NAME`/
  `$DATA` 속성 읽기(§3.5), `$INDEX_ROOT`만으로 완결되는 **작은
  디렉터리** 읽기(§3.6) - 대용량 디렉터리(`$INDEX_ALLOCATION` B+
  트리 확장)는 후속 증분.

**후속 증분**:
1. **쓰기 지원 전체** - MFT 레코드 할당(`$MFT`/`$Bitmap` 갱신),
   속성 상주→비상주 전환, 새 데이터 런 할당, `$FILE_NAME` 인덱스
   엔트리 삽입/삭제. 가장 큰 후속 작업 - 여러 하위 증분으로 더
   쪼갤 가능성이 높음(착수 세션 판단).
2. **`$INDEX_ALLOCATION` B+ 트리 확장** - `$INDEX_ROOT`에 안 들어가는
   대용량 디렉터리 읽기(+ 후속 1과 함께 쓰기).
3. **`$LogFile` 저널 리플레이** - 비정상 언마운트 볼륨 안전 마운트
   (`libext4`의 jbd2 리플레이 증분과 대응하는 개념).
4. **압축(`COMPRESSION_FL`)/스파스 파일** - `$DATA` 속성 플래그가
   이미 이 가능성을 구조에 담고 있으나(§3.3) 1차 증분은 압축 해제
   로직 자체가 없어 그런 파일을 만나면 명시적으로 거부.
5. **암호화(EFS)** - Windows 전용 키 관리에 의존해 이 프로젝트
   범위에서 완전한 지원이 가능한지 자체가 불확실 - 착수 세션이
   재검토(최소 "암호화된 파일은 읽기 거부"까지는 1차 증분이 이미
   구조적으로 자연스럽게 처리 가능).
6. **리파스 포인트(심볼릭 링크 등)** - `$REPARSE_POINT` 속성(타입
   0xC0) 해석.
7. **보안 서술자(`$Secure`)/ACL, 볼륨 전역 메타파일 나머지**
   (`$Quota`/`$UsnJrnl`/`$ObjId`) - 접근 제어/쿼터/USN 저널.
8. **대체 데이터 스트림(ADS) 완전 지원** - §3.3의 `nameLength`
   필드가 이미 구조적으로 지원하지만, 1차 증분은 이름 없는(기본)
   `$DATA`만 실제로 노출 - 이름 있는 스트림에 대한 API 경로(파일
   경로에 `:스트림이름` 접미사 등)는 후속.

각 항목은 별도 `PN-` 계획으로 등록한다(CLAUDE.md 규칙7, 문서 승인과
함께).

## 3. 온디스크 포맷 — NTFS

### 3.1 부트 섹터(VBR) — LBA 0, 512바이트

```cpp
#pragma pack(push, 1)
struct NtfsBootSector {
    uint8_t  jmpBoot[3];
    char     oemId[8];              // "NTFS    "
    uint16_t bytesPerSector;        // 보통 512
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;       // 항상 0
    uint8_t  unused1[5];            // 옛 FAT 필드 자리, 전부 0
    uint8_t  mediaDescriptor;
    uint16_t unused2;                // 옛 FAT16 필드 자리, 0
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t unused3;                // 옛 FAT32 TotalSectors32 자리, 0
    uint32_t unused4;                // 항상 0(정확한 의미는 구현 시 재확인)
    uint64_t totalSectors;
    uint64_t mftClusterNumber;       // $MFT 시작 LCN(§3.2)
    uint64_t mftMirrClusterNumber;   // $MFTMirr(백업) 시작 LCN
    int8_t   clustersPerMftRecord;   // 양수면 클러스터 수, 음수면
                                       // 레코드 크기 = 2^|값| 바이트
                                       // (예: 0xF6=-10 → 1024바이트,
                                       // 가장 흔한 값)
    uint8_t  reserved1[3];
    int8_t   clustersPerIndexBuffer; // 위와 동일한 인코딩 규칙
    uint8_t  reserved2[3];
    uint64_t volumeSerialNumber;
    uint32_t checksum;                // 미사용(0) - 실사용 안 함
    uint8_t  bootCode[426];
    uint16_t bootSignature;           // 0xAA55
};
static_assert(sizeof(NtfsBootSector) == 512);
#pragma pack(pop)
constexpr char kNtfsOemId[8] = {'N','T','F','S',' ',' ',' ',' '};

inline uint32_t kNtfsMftRecordSize(const NtfsBootSector& bs) {
    return bs.clustersPerMftRecord >= 0
        ? bs.clustersPerMftRecord * bs.sectorsPerCluster * bs.bytesPerSector
        : 1u << (-bs.clustersPerMftRecord);
}
```

**`mount()` 판별**: `oemId == kNtfsOemId` + `bootSignature == 0xAA55`
확인 - 불일치 시 "이 포맷 아님".

### 3.2 MFT(Master File Table) 레코드 — fixup(수정 시퀀스) 필수

`mftClusterNumber`부터 시작하는 클러스터 영역에 고정 크기(§3.1
`kNtfsMftRecordSize`, 보통 1024바이트) 레코드가 연속 배치. MFT 자신도
레코드 0(`$MFT`)으로 자기 자신을 기술하는 자기 참조 구조 - 1차 증분은
부트 섹터의 `mftClusterNumber`/`totalSectors`만으로 MFT 영역 물리
위치를 이미 알 수 있으므로(자기 참조를 실제로 순회할 필요 없음),
레코드 0 자체를 별도로 특별 취급하지 않고 나머지 레코드와 동일하게
다룬다.

**예약 레코드 번호**(관례 - MFT 첫 16개는 항상 시스템 메타파일):
0=`$MFT` 1=`$MFTMirr` 2=`$LogFile` 3=`$Volume` 4=`$AttrDef`
5=루트 디렉터리 6=`$Bitmap`(볼륨 클러스터 할당 비트맵) 7=`$Boot`
8=`$BadClus` 9=`$Secure` 10=`$UpCase` 11=`$Extend` 12~15=예약.
1차 증분이 실제로 여는 건 5(루트)뿐 - 나머지는 §2 후속 증분(쓰기,
`$Secure` 등)이 필요로 할 때 다룬다.

```cpp
#pragma pack(push, 1)
struct NtfsFileRecordHeader {
    char     magic[4];              // "FILE" - 손상/미사용 레코드는
                                     // 다른 값("BAAD" 등)일 수 있음
    uint16_t updateSequenceOffset;  // 아래 fixup 배열 오프셋
    uint16_t updateSequenceSize;    // fixup 배열의 u16 원소 개수
                                     // (첫 원소 = 원본 USN 값 자신 포함)
    uint64_t logFileSequenceNumber; // $LogFile LSN - 1차 증분은 무시
                                     // (저널 리플레이 없음, §2 후속3)
    uint16_t sequenceNumber;
    uint16_t hardLinkCount;
    uint16_t firstAttributeOffset;
    uint16_t flags;                 // bit0=InUse bit1=IsDirectory
    uint32_t usedSize;
    uint32_t allocatedSize;         // = kNtfsMftRecordSize()
    uint64_t baseFileRecord;        // 확장 레코드면 기본 레코드 참조,
                                     // 0이면 이 레코드 자신이 기본
    uint16_t nextAttributeId;
    uint16_t reserved;
    uint32_t mftRecordNumber;       // 자기 자신의 레코드 번호(자체 검증용)
};
#pragma pack(pop)
constexpr uint32_t kNtfsFileMagic = 0x454C4946;  // "FILE" 리틀엔디안
constexpr uint16_t kNtfsFlagInUse = 0x1;
constexpr uint16_t kNtfsFlagIsDirectory = 0x2;
```

**fixup 검증(모든 레코드 읽기의 필수 전제 조건)**: NTFS는 섹터
(512바이트) 단위 쓰기 중 전원 손실로 일부만 쓰인 손상을 감지하기
위해, 레코드를 디스크에 쓸 때 각 섹터의 **마지막 2바이트**를 공통
"Update Sequence Number"로 덮어쓰고, 원래 있던 마지막 2바이트
값들을 `updateSequenceOffset` 위치의 배열에 순서대로 보존해 둔다.
**읽을 때**: 레코드를 통째로 읽은 뒤, 각 섹터의 마지막 2바이트가
전부 그 USN과 일치하는지 확인(불일치면 손상 - 레코드 거부) → 그
2바이트들을 fixup 배열에 저장된 원래 값으로 되돌려 놓는다(이 복원
없이 레코드 내용을 그대로 해석하면 각 섹터 끝 2바이트가 USN 쓰레기로
오염된 채 읽힌다 - **이 단계를 빠뜨리면 모든 파싱이 조용히 틀어진다**,
1차 증분 구현 시 반드시 첫 번째로 검증할 지점).

### 3.3 속성(Attribute) 공통 헤더

레코드의 `firstAttributeOffset`부터 속성들이 연속 배치, 각 속성의
`length`로 다음 속성 위치 계산, `type == 0xFFFFFFFF`를 만나면 끝.

```cpp
#pragma pack(push, 1)
struct NtfsAttributeHeader {
    uint32_t type;            // 0x10=$STANDARD_INFORMATION 0x30=$FILE_NAME
                               // 0x80=$DATA 0x90=$INDEX_ROOT
                               // 0xA0=$INDEX_ALLOCATION 0xB0=$BITMAP
                               // 0xFFFFFFFF=끝
    uint32_t length;          // 이 속성 전체 크기(8바이트 정렬)
    uint8_t  nonResident;     // 0=상주(resident) 1=비상주(non-resident)
    uint8_t  nameLength;      // UTF-16 코드유닛 수(0=이름 없음, ADS는 >0)
    uint16_t nameOffset;
    uint16_t flags;           // COMPRESSED/ENCRYPTED/SPARSE 등(§2 후속4/5)
    uint16_t attributeId;
};
// 상주(nonResident==0)일 때 헤더 직후:
struct NtfsResidentAttrTail {
    uint32_t contentLength;
    uint16_t contentOffset;
    uint8_t  indexedFlag;
    uint8_t  padding;
    // <실제 내용이 contentOffset 위치부터 contentLength바이트>
};
// 비상주(nonResident==1)일 때 헤더 직후:
struct NtfsNonResidentAttrTail {
    uint64_t startingVcn;
    uint64_t lastVcn;
    uint16_t dataRunsOffset;
    uint16_t compressionUnit;   // 0이면 비압축
    uint32_t padding;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint64_t initializedSize;
    // compressionUnit != 0이면 추가로 uint64_t compressedSize
    // <데이터 런이 dataRunsOffset 위치부터, §3.4>
};
#pragma pack(pop)
```

### 3.4 데이터 런(Data Run) 디코딩 — 논리→물리 클러스터 매핑

비상주 속성의 데이터 런은 가변 길이 압축 인코딩이다 - 각 런:

```
[헤더바이트 1개] [길이 필드 N바이트] [오프셋 필드 M바이트]
헤더바이트: 하위 4비트 = N(길이 필드 바이트 수), 상위 4비트 = M(오프셋 필드 바이트 수)
길이 필드: 리틀엔디안 부호 없는 정수 - 이 런이 담당하는 클러스터 수
오프셋 필드: 리틀엔디안 부호 있는 정수(2의 보수, M바이트 폭) - 직전
             런의 시작 LCN 대비 상대 오프셋(첫 런은 0 기준). M==0이면
             "sparse 런"(실제 할당 없음 - 읽으면 0으로 채워진 것으로
             간주).
헤더바이트 0x00을 만나면 데이터 런 시퀀스 종료.
```

논리 클러스터 번호(VCN) → 물리 클러스터 번호(LCN) 변환: 런들을
순서대로 누적하며 VCN이 속하는 런을 찾고(그 런의 시작 VCN + 런
길이 범위), 그 런의 시작 LCN(직전 런들의 오프셋을 누적한 절대값)
+ (VCN - 그 런의 시작 VCN)이 물리 클러스터.

### 3.5 핵심 속성 내용

- **`$STANDARD_INFORMATION`(0x10, 항상 상주)**: `creationTime`/
  `lastModificationTime`/`lastMftChangeTime`/`lastAccessTime`(전부
  `uint64_t`, Windows FILETIME - 1601-01-01 UTC 기준 100나노초 단위,
  이 커널의 wall-clock 변환 필요), `fileAttributes`(`uint32_t`,
  READONLY/HIDDEN/SYSTEM/DIRECTORY 등 - FAT 계열과 값 자체는 유사한
  비트 의미), 이후 NTFS 3.0+ 확장 필드(OwnerId/SecurityId/
  QuotaCharged/USN) - 1차 증분은 필요 시에만 참고, 필수 파싱 대상
  아님.
- **`$FILE_NAME`(0x30, 보통 상주)**: `parentDirectory`(`uint64_t`,
  하위 48비트=부모 MFT 레코드 번호, 상위 16비트=부모 시퀀스 번호),
  타임스탬프 4종(위와 동일 형식), `allocatedSize`/`realSize`,
  `flags`, `nameLength`(`uint8_t`, UTF-16 코드유닛 수), `nameNamespace`
  (`uint8_t`: 0=POSIX 1=Win32 2=DOS 3=Win32&DOS), `name[nameLength]`
  (UTF-16). **한 파일이 여러 개 가질 수 있다**(하드링크마다 하나,
  8.3 별칭용 DOS 이름 포함) - 1차 증분은 `nameNamespace==1(Win32)`
  또는 `3(Win32&DOS)`인 것을 "그 파일의 대표 이름"으로 우선 채택
  (일반적인 리버스엔지니어링 문헌의 관례 - 구현 시 재확인).
- **`$DATA`(0x80)**: 이름 없으면(§3.3 `nameLength==0`) 기본 파일
  내용 - 상주면 §3.3 `NtfsResidentAttrTail` 뒤에 바로 원본 바이트,
  비상주면 §3.4 데이터 런으로 매핑된 클러스터들.

### 3.6 디렉터리 — `$INDEX_ROOT`(작은 디렉터리는 이걸로 완결)

`$INDEX_ROOT`(0x90, 항상 상주)는 B+ 트리의 루트를 담는다 -
루트 노드에 인덱스 엔트리가 다 들어가면(작은 디렉터리) 그걸로 끝,
안 들어가면 인덱스 엔트리가 `$INDEX_ALLOCATION`(§2 후속2, 1차 증분
범위 밖)의 하위 노드를 가리킨다.

```cpp
#pragma pack(push, 1)
struct NtfsIndexRootHeader {   // NtfsResidentAttrTail의 content 시작
    uint32_t attributeType;   // 보통 0x30($FILE_NAME) - 디렉터리는 이걸로 정렬
    uint32_t collationRule;
    uint32_t indexAllocEntrySize; // 인덱스 버퍼 크기(바이트)
    uint8_t  clustersPerIndexRecord;
    uint8_t  reserved[3];
    // 이어서 IndexHeader(오프셋/크기 필드) + IndexEntry 배열
};
struct NtfsIndexEntry {
    uint64_t mftReference;    // 이 엔트리가 가리키는 자식의 MFT 참조
                               // (하위 48비트=레코드번호, 상위 16비트=시퀀스)
    uint16_t entryLength;
    uint16_t keyLength;        // 뒤따르는 $FILE_NAME 키의 길이
    uint16_t flags;            // bit0=하위 노드 있음($INDEX_ALLOCATION로
                                // 내려가야 함, 1차 증분은 이 경우 미지원
                                // 처리) bit1=마지막 엔트리(키 없음)
    uint16_t reserved;
    // <flags bit1이 꺼져 있으면 여기부터 keyLength바이트의 $FILE_NAME
    // 형식 키(§3.5)> - flags bit0이 세팅됐으면 이 엔트리 끝에 자식
    // 인덱스 버퍼의 VCN(uint64_t)도 붙지만 1차 증분은 그 경로를 안 탐
};
#pragma pack(pop)
```

1차 증분은 `IndexEntry` 순회 중 `flags` bit0(하위 노드 있음)이 세팅된
엔트리를 만나면 "이 디렉터리는 1차 증분이 처리 못 하는 대용량
디렉터리"로 판단해 명시적으로 실패 반환(크래시/데이터 손상이 아니라
"아직 지원 안 함" - `SP-7A9CED3E` §2가 이미 확립한 관례와 동일한
정직한 에러 처리).

## 4. `NtfsDriver` 구현 — `FileSystemDriver` 인터페이스(1차 증분은 읽기 전용)

```cpp
// minicore/libs/libntfs/ntfs.h
class NtfsDriver : public FileSystemDriver {
public:
    bool mount(BlockDevice* device, bool readOnly) override;  // 1차 증분은
                                                                 // readOnly=false로
                                                                 // 호출되면 실패
                                                                 // 반환(쓰기 미지원
                                                                 // 명시)
    bool remount(bool writable) override;   // 1차 증분은 writable=true 거부
    OpenResult open(const char* relPath, uint32_t relPathLen, uint32_t flags) override;
    void close(FileHandle handle) override;
    ReadResult read(FileHandle handle, uint64_t offset, void* buf, uint32_t len) override;
    WriteResult write(FileHandle handle, uint64_t offset, const void* buf, uint32_t len) override;  // 항상 실패(§2 후속1)
    bool stat(const char* relPath, uint32_t relPathLen, StatBuf* out) override;
    bool mkdir(const char* relPath, uint32_t relPathLen) override;   // 항상 실패(§2 후속1)
    bool rmdir(const char* relPath, uint32_t relPathLen) override;   // 항상 실패(§2 후속1)
    bool unlink(const char* relPath, uint32_t relPathLen) override;  // 항상 실패(§2 후속1)
    bool readdir(FileHandle dirHandle, DirEntry* out) override;

private:
    BlockDevice* device_ = nullptr;
    NtfsBootSector bs_{};
    uint32_t clusterSize_ = 0;
    uint32_t mftRecordSize_ = 0;
    uint64_t mftStartLcn_ = 0;
};
```

`FileSystemDriver` 인터페이스 자체는 쓰기 메서드도 요구하지만(§4
서명 그대로 유지 - 인터페이스를 쪼개지 않음, `SwapBackend`가 읽기
전용 마운트 개념 자체를 아예 안 가진 것과 달리 이쪽은 인터페이스
차원에서 "거부"로 표현 가능하므로 새 인터페이스가 필요 없음)
쓰기 계열 메서드는 1차 증분에서 전부 명시적으로 실패를 반환한다.

### 4.1 경로 해석

루트(MFT 레코드 5)의 `$INDEX_ROOT`(§3.6)에서 이름이 일치하는
`NtfsIndexEntry`를 찾아 그 `mftReference`의 레코드 번호로 다음
세그먼트의 시작 레코드 삼는다 - 그 레코드를 §3.2로 열고(fixup 검증
필수) `$FILE_NAME`(§3.5)으로 이름 재확인, 디렉터리면 다시 그
레코드의 `$INDEX_ROOT`로 하강.

### 4.2 `open`/`read`

`FileHandle`에 MFT 레코드 번호 + 파싱된 `$DATA` 속성(상주 내용
포인터 또는 비상주 데이터 런)을 캐싱. `read`는 상주면 그대로 복사,
비상주면 §3.4로 논리 오프셋을 물리 클러스터로 변환해
`device_->readBlocks()`(클러스터 크기 단위 정렬 후 필요한 만큼
잘라 반환).

## 5. 이 문서가 확정하지 않는 것

- `NtfsIndexRootHeader`/`NtfsIndexEntry`의 `collationRule`/`flags`
  정확한 값 목록과 `IndexHeader` 내부 필드(엔트리 배열 시작 오프셋
  계산에 필요) - §1 재확인 경고 그대로, 이 문서 하나로 단언하지
  않음.
- `$FILE_NAME` 다중 인스턴스 중 "대표 이름" 선택 정책(§3.5) - 구현
  세션이 실제 Windows/`ntfs-3g` 동작과 대조해 재확인.
- 압축/스파스/암호화 플래그(§3.3 `flags`)를 만났을 때 정확히 어떤
  에러 코드로 거부할지 - `SP-2BCE5D60` §3.1의 기존 에러 체계(§4
  후속 증분 범위)와 맞출지는 구현 세션 판단.
- wall-clock 변환(Windows FILETIME ↔ 이 커널의 시각 표현) - 다른
  세 라이브러리(FAT 계열 타임스탬프)와 공유 가능한 유틸리티로 묶을
  지 각자 구현할지는 구현 세션 재량.

## 6. 다른 설계와의 관계

- `SP-2BCE5D60` §3.1/§3.0 — 이 문서가 구현하는 `FileSystemDriver`/
  `BlockDevice` 인터페이스의 출처.
- `SP-A658A124` — 이 opinion이 원래 달렸던 문서(libvfat).
- `SP-F1987EF8` — 같은 opinion으로 함께 등록된 자매 설계(libexfat).
- `SP-D02C4A73`/`SP-7A9CED3E` — "외부 표준 준수 + 단계적 로드맵"
  원칙의 선례 - 이 문서는 그 원칙을 가장 위험도 높은 포맷에 적용한
  사례(1차 증분을 읽기 전용으로 더 좁힘).
- `RM-7C249618` — 이 문서 승인 후 `libntfs` 신규 행 추가 필요
  (CLAUDE.md 규칙8).

