#ifndef MINICORE_LIBNTFS_NTFS_H
#define MINICORE_LIBNTFS_NTFS_H

#include "libkenv/types.h"

namespace fs {
class BlockDevice;
}

// SP-AA6DF406 - NTFS 온디스크 포맷 읽기 라이브러리(1차 증분, 읽기
// 전용, PN-49F24FD7). 이 프로젝트가 새로 고안한 포맷이 아니다 -
// 공개 리버스엔지니어링 문헌 수준인 SP-AA6DF406 원안을 그대로 믿지
// 않고, Linux 커널 소스(fs/ntfs3/ntfs.h)와 1바이트 단위로 재대조해
// 확정했다(SP-AA6DF406 §1이 다른 세 문서보다 한 단계 더 강하게 요구한
// 재확인 - ext4/swapfs 때와 달리 이번엔 부트섹터/MFT 레코드 헤더/
// 속성 헤더 자체는 정확했으나, `$FILE_NAME`/`$STANDARD_INFORMATION`
// 내용과 `IndexHeader` 내부 필드는 원래 설계 문서가 §5에서 "확정하지
// 않음"으로 열어 둔 부분이라 이번 구현에서 Linux 커널 소스로 새로
// 확정했고, `NtfsNonResidentAttrTail::compressionUnit`은 실제로는
// 1바이트(`c_unit`)+5바이트 예약(원안은 2바이트+4바이트로 잘못
// 나눴었음 - 전체 6바이트 폭은 같아 실제 값 오염은 없었지만 정확한
// 타입으로 정정).
//
// [범위, 2026-09-22, PN-49F24FD7] §3(온디스크 포맷)과 무상태 읽기
// API(`NtfsVolume`)만 구현한다 - `NtfsDriver`(§4, VFS 통합 계층)는
// `libext4`/`libvfat`/`libexfat`과 동일한 이유로 별도 후속 계획으로
// 등록한다. **읽기 전용** - 쓰기(mkdir/unlink/write)는 이 라이브러리
// 자체가 아직 없다(SP-AA6DF406 §1 - 손상 위험이 높아 의도적으로
// 뒤로 미룸). `$INDEX_ALLOCATION`(대용량 디렉터리 B+ 트리 확장)/
// 압축/스파스/암호화/리파스포인트/ADS도 전부 이번 범위 밖 - 그런
// 파일/디렉터리를 만나면 명시적으로 "미지원"만 반환한다(크래시 아님).
namespace ntfs {

using kernel::uint8_t;
using kernel::uint16_t;
using kernel::uint32_t;
using kernel::uint64_t;
using kernel::int8_t;
using kernel::int64_t;

// ---------------------------------------------------------------------
// 3.1 부트 섹터(VBR) - LBA 0, 512바이트. SP-AA6DF406 §3.1 그대로 -
// 실제 mkfs.ntfs(ntfs-3g) 이미지 바이트 및 Linux 커널 소스로 재대조
// 완료(오프셋 전부 일치, 정정 없음).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct NtfsBootSector {
    uint8_t  jmpBoot[3];
    char     oemId[8];              // "NTFS    "
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;       // 항상 0
    uint8_t  unused1[5];
    uint8_t  mediaDescriptor;
    uint16_t unused2;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t unused3;
    uint32_t unused4;
    uint64_t totalSectors;
    uint64_t mftClusterNumber;       // $MFT 시작 LCN(§3.2)
    uint64_t mftMirrClusterNumber;
    int8_t   clustersPerMftRecord;   // 양수=클러스터 수, 음수=2^|값| 바이트
    uint8_t  reserved1[3];
    int8_t   clustersPerIndexBuffer; // 위와 동일한 인코딩
    uint8_t  reserved2[3];
    uint64_t volumeSerialNumber;
    uint32_t checksum;
    uint8_t  bootCode[426];
    uint16_t bootSignature;          // 0xAA55
};
static_assert(sizeof(NtfsBootSector) == 512, "NtfsBootSector는 512바이트여야 함");
#pragma pack(pop)

constexpr char kNtfsOemId[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};
constexpr uint32_t kBootSectorSignatureOffset = 510;
constexpr uint16_t kBootSectorSignature = 0xAA55;

inline uint32_t kNtfsClustersOrPow2Size(int8_t rawValue, uint32_t clusterSize) {
    return rawValue >= 0 ? static_cast<uint32_t>(rawValue) * clusterSize
                         : 1u << static_cast<uint32_t>(-rawValue);
}

// ---------------------------------------------------------------------
// 3.2 MFT 레코드 헤더 - Linux 커널 fs/ntfs3/ntfs.h의 NTFS_RECORD_HEADER
// + MFT_REC와 1바이트 단위로 대조 완료(SP-AA6DF406이 이미 이 두
// 구조체를 평탄화해 옮긴 그대로 정확했음 - 정정 없음). fixup(수정
// 시퀀스) 검증은 모든 레코드 읽기의 필수 전제 조건(§3.2 문서 주석,
// ntfs.cpp의 kApplyFixup 참고).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct NtfsFileRecordHeader {
    char     magic[4];              // "FILE" - 손상/미사용은 다른 값("BAAD" 등)
    uint16_t updateSequenceOffset;  // fixup 배열 오프셋(레코드 시작 기준)
    uint16_t updateSequenceSize;    // fixup 배열의 u16 원소 개수(첫 원소=원본 USN 자신)
    uint64_t logFileSequenceNumber; // $LogFile LSN - 1차 증분은 무시(저널 리플레이 없음)
    uint16_t sequenceNumber;
    uint16_t hardLinkCount;
    uint16_t firstAttributeOffset;
    uint16_t flags;                 // bit0=InUse bit1=IsDirectory
    uint32_t usedSize;
    uint32_t allocatedSize;
    uint64_t baseFileRecord;        // 확장 레코드면 기본 레코드 참조, 0이면 자신이 기본
    uint16_t nextAttributeId;
    uint16_t reserved;
    uint32_t mftRecordNumber;       // 자기 자신의 레코드 번호
};
static_assert(sizeof(NtfsFileRecordHeader) == 48, "NtfsFileRecordHeader는 48바이트여야 함");
#pragma pack(pop)

constexpr uint16_t kNtfsFlagInUse = 0x1;
constexpr uint16_t kNtfsFlagIsDirectory = 0x2;

constexpr uint32_t kNtfsRootDirectoryRecord = 5;  // 예약 레코드 번호(§3.2) - 1차 증분이 여는 유일한 예약 레코드

// ---------------------------------------------------------------------
// 3.3 속성(Attribute) 공통 헤더 - Linux 커널 ATTRIB/ATTR_RESIDENT/
// ATTR_NONRESIDENT와 대조 완료. NtfsNonResidentAttrTail만 정정
// (compressionUnit이 실제로는 1바이트 - 위 파일 문서 주석 참고).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct NtfsAttributeHeader {
    uint32_t type;             // 0x10/0x30/0x80/0x90/0xA0/0xB0, 0xFFFFFFFF=끝
    uint32_t length;           // 이 속성 전체 크기(8바이트 정렬)
    uint8_t  nonResident;      // 0=상주 1=비상주
    uint8_t  nameLength;       // UTF-16 코드유닛 수(0=이름 없음, ADS는 >0)
    uint16_t nameOffset;
    uint16_t flags;            // COMPRESSED/ENCRYPTED/SPARSE 등(1차 증분은 미지원 처리)
    uint16_t attributeId;
};
static_assert(sizeof(NtfsAttributeHeader) == 16, "NtfsAttributeHeader는 16바이트여야 함");

// nonResident==0일 때 헤더 직후.
struct NtfsResidentAttrTail {
    uint32_t contentLength;
    uint16_t contentOffset;
    uint8_t  indexedFlag;
    uint8_t  padding;
};
static_assert(sizeof(NtfsResidentAttrTail) == 8, "NtfsResidentAttrTail은 8바이트여야 함");

// nonResident==1일 때 헤더 직후.
struct NtfsNonResidentAttrTail {
    uint64_t startingVcn;
    uint64_t lastVcn;
    uint16_t dataRunsOffset;
    uint8_t  compressionUnit;  // 압축 단위 log값, 0이면 비압축(Linux c_unit과 대조해 정정)
    uint8_t  reserved1[5];
    uint64_t allocatedSize;
    uint64_t realSize;
    uint64_t initializedSize;
};
static_assert(sizeof(NtfsNonResidentAttrTail) == 48, "NtfsNonResidentAttrTail은 48바이트여야 함");
#pragma pack(pop)

constexpr uint32_t kNtfsAttrTypeStandardInformation = 0x10;
constexpr uint32_t kNtfsAttrTypeFileName = 0x30;
constexpr uint32_t kNtfsAttrTypeData = 0x80;
constexpr uint32_t kNtfsAttrTypeIndexRoot = 0x90;
constexpr uint32_t kNtfsAttrTypeIndexAllocation = 0xA0;
constexpr uint32_t kNtfsAttrTypeBitmap = 0xB0;
constexpr uint32_t kNtfsAttrTypeEnd = 0xFFFFFFFFu;

// ---------------------------------------------------------------------
// 3.5 핵심 속성 내용 - SP-AA6DF406 §3.5는 산문으로만 필드를 나열했고
// (§5 "확정하지 않는 것"에도 정확한 구조체는 없었음), 이번 구현에서
// Linux 커널 ATTR_STD_INFO/ATTR_FILE_NAME(+NTFS_DUP_INFO)와 1바이트
// 단위로 대조해 구체적인 구조체로 확정했다.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
// $STANDARD_INFORMATION(0x10, 항상 상주) - 1차 증분이 실제로 쓰는
// 앞부분만(뒤에 NTFS 3.0+ 확장 필드가 더 있으나 §3.5가 "참고용,
// 필수 아님"으로 명시).
struct NtfsStandardInfoContent {
    uint64_t creationTime;
    uint64_t lastModificationTime;
    uint64_t lastMftChangeTime;
    uint64_t lastAccessTime;
    uint32_t fileAttributes;
};
static_assert(sizeof(NtfsStandardInfoContent) == 36, "NtfsStandardInfoContent 앞부분 크기 확인용");

// $FILE_NAME(0x30, 보통 상주) 고정부 - name[nameLength]가 바로 뒤에 옴.
struct NtfsFileNameContent {
    uint64_t parentDirectory;   // 하위 48비트=부모 MFT 레코드 번호, 상위 16비트=부모 시퀀스 번호
    uint64_t creationTime;
    uint64_t lastModificationTime;
    uint64_t lastMftChangeTime;
    uint64_t lastAccessTime;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint32_t fileAttributes;
    uint32_t reserved;           // Linux extend_data - 1차 증분은 안 씀
    uint8_t  nameLength;         // UTF-16 코드유닛 수
    uint8_t  nameNamespace;      // 0=POSIX 1=Win32 2=DOS 3=Win32&DOS
};
static_assert(sizeof(NtfsFileNameContent) == 66, "NtfsFileNameContent 고정부는 66바이트여야 함");
#pragma pack(pop)

constexpr uint8_t kNtfsNamespaceWin32 = 1;
constexpr uint8_t kNtfsNamespaceWin32AndDos = 3;
// [정정, 2026-09-22, PN-52C577F3 실측 발견] SP-AA6DF406 §3.5는 이
// 비트가 "FAT류 attr 비트와 값 호환"이라며 0x10(Win32
// FILE_ATTRIBUTE_DIRECTORY)으로 명시했으나, 실제 mkfs.ntfs(ntfs-3g)
// 이미지의 $FILE_NAME 중복 정보를 바이트 단위로 디코딩해 보면 진짜
// 디렉터리 엔트리의 fileAttributes는 0x10 비트가 전혀 서 있지 않고
// (예: 파일 hello.txt=0x20/ARCHIVE만, 디렉터리 nested=0x10000020)
// 대신 0x10000000 비트가 서 있다 - 이건 Win32 GetFileAttributes의
// FILE_ATTRIBUTE_DIRECTORY가 아니라 NTFS 온디스크 $FILE_NAME 전용
// 센티널(공개 문헌에서 "FILE_ATTR_DUP_FILENAME_INDEX_PRESENT"로
// 불리는 값 - 자식 MFT 레코드를 열지 않고도 디렉터리 목록에서
// 디렉터리 여부를 알 수 있게 부모 인덱스 엔트리에 복제해 두는
// 필드다). 리눅스 커널 소스 재대조 없이 SP-AA6DF406 원문을 그대로
// 믿었다면 놓쳤을 실제 기능 결함(Readdir/Open이 모든 하위 디렉터리를
// 파일로 오판해 그 안으로 못 들어감) - 실측 바이트로 확인 후 정정.
constexpr uint32_t kFileAttrDirectory = 0x10000000;

inline uint64_t kNtfsMftReferenceRecordNumber(uint64_t ref) { return ref & 0xFFFFFFFFFFFFull; }

// ---------------------------------------------------------------------
// 3.6 디렉터리 - $INDEX_ROOT(작은 디렉터리는 이걸로 완결). IndexHeader
// 내부 필드는 SP-AA6DF406 §5가 "확정하지 않음"으로 열어 둔 부분 -
// Linux 커널 INDEX_HDR/NTFS_DE와 대조해 이번 구현에서 확정했다.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct NtfsIndexRootHeader {  // NtfsResidentAttrTail의 content 시작
    uint32_t attributeType;   // 보통 0x30($FILE_NAME) - 디렉터리는 이걸로 정렬
    uint32_t collationRule;
    uint32_t indexAllocEntrySize;
    uint8_t  clustersPerIndexRecord;
    uint8_t  reserved[3];
};
static_assert(sizeof(NtfsIndexRootHeader) == 16, "NtfsIndexRootHeader는 16바이트여야 함");

// NtfsIndexRootHeader 바로 뒤에 옴.
struct NtfsIndexHeader {
    uint32_t entriesOffset;  // 첫 NtfsIndexEntry까지의 오프셋(이 헤더 시작 기준)
    uint32_t usedSize;
    uint32_t totalSize;
    uint32_t flags;          // 0x00=small 0x01=large(1차 증분 미지원)
};
static_assert(sizeof(NtfsIndexHeader) == 16, "NtfsIndexHeader는 16바이트여야 함");

struct NtfsIndexEntry {
    uint64_t mftReference;   // 이 엔트리가 가리키는 자식의 MFT 참조
    uint16_t entryLength;
    uint16_t keyLength;      // 뒤따르는 $FILE_NAME 형식 키의 길이
    uint16_t flags;          // bit0=하위 노드 있음(1차 증분 미지원) bit1=마지막 엔트리(키 없음)
    uint16_t reserved;
};
static_assert(sizeof(NtfsIndexEntry) == 16, "NtfsIndexEntry는 16바이트여야 함");
#pragma pack(pop)

constexpr uint16_t kIndexEntryHasSubnode = 0x1;
constexpr uint16_t kIndexEntryLast = 0x2;

// ---------------------------------------------------------------------
// 4(부분) - NtfsVolume: 읽기 전용 마운트 + mount()가 캐싱한 상태의
// 읽기 전용 노출.
//
// [범위 변경, 2026-09-22, PN-49F24FD7 -> PN-52C577F3으로 갱신,
// libext4/libvfat/libexfat이 먼저 겪은 것과 동일한 실측 제약] 원래
// 있던 무상태 동기 메서드(resolvePath/readData/readdirAt + private
// 헬퍼 findAttribute/scanIndexRoot/clusterToSector/readClusters)는
// 전부 제거했다 - 전부 `fs::BlockDevice::readBlocks()`(Task 레벨
// 블로킹 동기 래퍼)를 쓰는데, 이건 `kernel::KernelFsDriver::onExec()`
// (코루틴)에서 호출하면 실측 확인된 무한 대기가 난다. `NtfsDriver`
// (ntfs_driver.h/.cpp, PN-52C577F3)가 `kernel::AsyncTaskCoroAwaiter`
// (PN-6EDED542) 기반으로 `onExec` 자신의 코루틴 몸체 안에 이 로직을
// 평탄화해 다시 구현한다(`ext4_driver.cpp`와 가장 가까운 템플릿 -
// NTFS도 "레코드를 열고 -> 속성을 찾고 -> 데이터 런을 읽는" 다단계
// 구조이기 때문). `mount()`만 여전히 실제로 쓰인다 - 진짜
// `kernel::Task` 컨텍스트(`NtfsDriver::mount()`)에서 한 번 호출되는
// 준비 단계라 내부적으로 동기 헬퍼(readMftRecord)를 계속 쓰는 게
// 안전하다(mount() 자신이 루트 레코드가 실제로 열리는지 확인해야
// 해서 - 이 하나는 mount() 전용 구현 세부로 private에 남긴다).
// ---------------------------------------------------------------------
class NtfsVolume {
public:
    bool mount(fs::BlockDevice* device);

    // [PN-52C577F3] mount()가 이미 파싱해 둔 상태를 읽기 전용으로
    // 노출 - `NtfsDriver::onExec()`(코루틴 컨텍스트)가 이 상태를
    // 그대로 재사용해 자신만의 평탄화된 순회 로직을 구현하는 데 쓴다.
    fs::BlockDevice* device() const { return device_; }
    uint32_t bytesPerSectorValue() const { return bs_.bytesPerSector; }
    uint32_t sectorsPerClusterValue() const { return bs_.sectorsPerCluster; }
    uint32_t clusterSizeValue() const { return clusterSize_; }
    uint32_t mftRecordSizeValue() const { return mftRecordSize_; }
    uint64_t mftStartLcnValue() const { return mftStartLcn_; }

private:
    // recordNumber의 MFT 레코드를 읽어 fixup을 적용하고 magic=="FILE"
    // 인지 확인한다 - outBuf는 mftRecordSize_ 바이트 이상이어야 한다.
    // mount()가 루트(레코드 5)를 실제로 열어 볼 때만 쓴다.
    bool readMftRecord(uint64_t recordNumber, uint8_t* outBuf);

    fs::BlockDevice* device_ = nullptr;
    NtfsBootSector bs_{};
    uint32_t clusterSize_ = 0;
    uint32_t mftRecordSize_ = 0;
    uint64_t mftStartLcn_ = 0;
};

}  // namespace ntfs

#endif  // MINICORE_LIBNTFS_NTFS_H
