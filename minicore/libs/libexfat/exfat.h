#ifndef MINICORE_LIBEXFAT_EXFAT_H
#define MINICORE_LIBEXFAT_EXFAT_H

#include "libkenv/types.h"

namespace fs {
class BlockDevice;
}

// SP-F1987EF8 - exFAT 온디스크 포맷 읽기/쓰기 라이브러리(1차 증분,
// PN-C93A4E9E). 이 프로젝트가 새로 고안한 포맷이 아니다 - 실제
// Microsoft exFAT File System Specification과 Linux 커널 소스
// (fs/exfat/exfat_raw.h)를 1바이트 단위로 대조해 확정했다(libext4/
// libswapfs/libvfat와 동일한 절차) - 이번 증분은 대조 결과 설계
// 문서(SP-F1987EF8) 자체에 레이아웃 오류가 없었다(libvfat의 FAT32와
// 같은 경우 - libext4/libswapfs와 달리 이번엔 정정이 필요 없었음).
//
// [범위, 2026-09-22, PN-C93A4E9E] §3(온디스크 포맷)과 무상태 읽기
// API(`ExfatVolume`)만 구현한다 - `ExfatDriver`(§4, VFS 통합 계층)는
// `libext4`(`PN-22784AD4`→`PN-9AE5BFE4`)/`libvfat`(`PN-F32F55A8`→
// `PN-EBAEA67B`)와 동일한 이유로 별도 후속 계획으로 등록한다(그
// 계획의 착수 세션이 이 클래스의 무상태 메서드를 재사용하지 않고
// `onExec` 자신의 코루틴 몸체 안에 평탄화해 다시 구현할 것 - 이미
// 두 번 확인된 실측 제약, `ext4_driver.cpp`/`vfat_driver.cpp` 참고).
// 쓰기 경로(mkdir/unlink)와 TexFAT/dirty비트/PercentInUse 후속
// 증분(SP-F1987EF8 §2)도 이번 범위 밖.
namespace exfat {

using kernel::uint8_t;
using kernel::uint16_t;
using kernel::uint32_t;
using kernel::uint64_t;

// ---------------------------------------------------------------------
// 3.1 메인 부트 섹터 - LBA 0, 512바이트. Microsoft 스펙 §3.1 및 실제
// exfatprogs(mkfs.exfat) 이미지로 필드 오프셋 실측 대조 완료
// (partitionOffset@64 ~ percentInUse@112 전부 일치).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct ExfatBootSector {
    uint8_t  jumpBoot[3];
    char     fileSystemName[8];   // "EXFAT   "(8바이트, 공백 패딩)
    uint8_t  mustBeZero[53];      // 바이트 11~63 - FAT12/16/32 BPB와 겹치는 자리를 일부러 0으로
    uint64_t partitionOffset;
    uint64_t volumeLength;
    uint32_t fatOffset;
    uint32_t fatLength;
    uint32_t clusterHeapOffset;
    uint32_t clusterCount;
    uint32_t firstClusterOfRootDirectory;
    uint32_t volumeSerialNumber;
    uint16_t fileSystemRevision;  // 0x0100 = 1.00
    uint16_t volumeFlags;         // bit0=ActiveFat bit1=VolumeDirty bit2=MediaFailure bit3=ClearToZero
    uint8_t  bytesPerSectorShift;     // 섹터 크기 = 1 << 이 값
    uint8_t  sectorsPerClusterShift;  // 클러스터 크기(섹터) = 1 << 이 값
    uint8_t  numberOfFats;        // 1(일반) 또는 2(TexFAT, 후속) - v1은 1만 지원
    uint8_t  driveSelect;
    uint8_t  percentInUse;
    uint8_t  reserved[7];
    uint8_t  bootCode[390];
    uint16_t bootSignature;       // 0xAA55
};
static_assert(sizeof(ExfatBootSector) == 512, "ExfatBootSector는 512바이트여야 함");
#pragma pack(pop)

constexpr char kExfatFsName[8] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
constexpr uint32_t kBootSectorSignatureOffset = 510;
constexpr uint16_t kBootSectorSignature = 0xAA55;

// ---------------------------------------------------------------------
// 3.2 FAT 영역 - FAT32와 동일한 32비트 체계(Linux 커널 exfat_raw.h
// EXFAT_EOF_CLUSTER/EXFAT_BAD_CLUSTER 대조 완료). NoFatChain 최적화
// (ExfatStreamExtEntry::generalSecondaryFlags bit1)는 아래 3.5 참고.
// ---------------------------------------------------------------------
constexpr uint32_t kExfatFatFree = 0;
constexpr uint32_t kExfatEoc = 0xFFFFFFFFu;
constexpr uint32_t kExfatBadCluster = 0xFFFFFFF7u;
constexpr uint32_t kExfatFirstCluster = 2;  // 클러스터 힙의 첫 클러스터 번호(EXFAT_FIRST_CLUSTER)

constexpr uint8_t kAllocPossibleBit = 0x01;  // GeneralSecondaryFlags bit0
constexpr uint8_t kNoFatChainBit = 0x02;     // GeneralSecondaryFlags bit1 - 세팅되면 FAT 참조 없이 산술만

// ---------------------------------------------------------------------
// 3.5 디렉터리 엔트리 - 32바이트, 여러 개가 묶여 파일 하나를 표현.
// 전부 Linux 커널 exfat_raw.h의 struct exfat_dentry(union)와 1바이트
// 단위로 대조 완료 - SP-F1987EF8이 옮긴 필드 오프셋 그대로 정확했다.
// ---------------------------------------------------------------------
#pragma pack(push, 1)

// EntryType 0x85(Primary) - "File Directory Entry": 파일 하나의 시작.
struct ExfatFileDirEntry {
    uint8_t  entryType;      // 0x85
    uint8_t  secondaryCount; // 뒤따르는 보조 엔트리 수(스트림+이름 합)
    uint16_t setChecksum;    // 이 엔트리 집합 전체(자신+보조, 이 필드 제외)의 체크섬
    uint16_t fileAttributes; // READ_ONLY/HIDDEN/SYSTEM/DIRECTORY/ARCHIVE - FAT32 attr 비트와 값 호환
    uint16_t reserved1;
    uint32_t createTimestamp;
    uint32_t lastModifiedTimestamp;
    uint32_t lastAccessedTimestamp;
    uint8_t  create10msIncrement;
    uint8_t  lastModified10msIncrement;
    uint8_t  createUtcOffset;
    uint8_t  lastModifiedUtcOffset;
    uint8_t  lastAccessedUtcOffset;
    uint8_t  reserved2[7];
};
static_assert(sizeof(ExfatFileDirEntry) == 32, "ExfatFileDirEntry는 32바이트여야 함");

// EntryType 0xC0(Secondary) - "Stream Extension": 데이터 위치/이름 길이.
struct ExfatStreamExtEntry {
    uint8_t  entryType;             // 0xC0
    uint8_t  generalSecondaryFlags; // bit0=AllocationPossible bit1=NoFatChain
    uint8_t  reserved1;
    uint8_t  nameLength;             // UTF-16 코드유닛 수
    uint16_t nameHash;                // Up-case 정규화된 이름의 해시(빠른 조회용 - v1은 안 씀, 3.6 참고)
    uint16_t reserved2;
    uint64_t validDataLength;         // 실제 쓰여진 데이터 길이(<=dataLength)
    uint32_t reserved3;
    uint32_t firstCluster;
    uint64_t dataLength;              // 파일 전체 크기
};
static_assert(sizeof(ExfatStreamExtEntry) == 32, "ExfatStreamExtEntry는 32바이트여야 함");

// EntryType 0xC1(Secondary) - "File Name": 이름 조각 15자(UTF-16)씩.
struct ExfatFileNameEntry {
    uint8_t  entryType;   // 0xC1
    uint8_t  generalSecondaryFlags;
    uint16_t fileName[15]; // UTF-16LE, 15자씩(nameLength로 실제 길이 판단)
};
static_assert(sizeof(ExfatFileNameEntry) == 32, "ExfatFileNameEntry는 32바이트여야 함");

// EntryType 0x81(Primary) - "Allocation Bitmap": 클러스터 힙 사용
// 비트맵 자신이 저장된 클러스터 체인(§3.3). Linux 커널 exfat_raw.h의
// bitmap 변형과 대조 완료(SP-F1987EF8엔 구조체가 없어 이번 구현에서
// 새로 옮김 - 스펙 자체의 고정 레이아웃이라 CLAUDE.md 규칙4의
// "확인 후 구현" 대상, 설계자 결정 사항 아님).
struct ExfatBitmapEntry {
    uint8_t  entryType;    // 0x81
    uint8_t  flags;
    uint8_t  reserved[18];
    uint32_t firstCluster;
    uint64_t dataLength;
};
static_assert(sizeof(ExfatBitmapEntry) == 32, "ExfatBitmapEntry는 32바이트여야 함");

// EntryType 0x82(Primary) - "Up-case Table": 대소문자 정규화 테이블
// 자신이 저장된 클러스터 체인(§3.4). 위와 동일한 이유로 이번 구현에서
// 새로 옮김.
struct ExfatUpcaseEntry {
    uint8_t  entryType;      // 0x82
    uint8_t  reserved1[3];
    uint32_t tableChecksum;
    uint8_t  reserved2[12];
    uint32_t firstCluster;
    uint64_t dataLength;
};
static_assert(sizeof(ExfatUpcaseEntry) == 32, "ExfatUpcaseEntry는 32바이트여야 함");

#pragma pack(pop)

constexpr uint8_t kExfatEntryInUseBit = 0x80;
constexpr uint8_t kExfatEntryTypeFile = 0x85;
constexpr uint8_t kExfatEntryTypeStreamExt = 0xC0;
constexpr uint8_t kExfatEntryTypeFileName = 0xC1;
constexpr uint8_t kExfatEntryTypeAllocBitmap = 0x81;
constexpr uint8_t kExfatEntryTypeUpcaseTable = 0x82;
constexpr uint8_t kExfatEntryTypeVolumeLabel = 0x83;
constexpr uint8_t kExfatEntryTypeEndOfDirectory = 0x00;  // 더 이상 유효 엔트리 없음

constexpr uint16_t kFileAttrDirectory = 0x10;  // FAT32 attr 비트와 값 호환(SP-F1987EF8 §3.5)

// resolvePath()/readdirAt()이 돌려주는, 파일 하나(0x85+0xC0 엔트리
// 집합)의 요약 - FAT류와 마찬가지로 재조회 가능한 inode 번호가 없어
// 이 값들을 통째로 들고 다닌다.
struct ResolvedEntry {
    uint32_t firstCluster = 0;
    uint64_t fileSize = 0;   // 디렉터리는 0x85 엔트리 자체엔 크기가 없음 - 항상 0으로 채움(크기는 클러스터 체인 길이로만 앎)
    bool isDir = false;
    bool noFatChain = false;  // true면 firstCluster+fileSize만으로 전체 범위 계산(FAT 미참조, §3.2)
};

// ---------------------------------------------------------------------
// 4(부분) - ExfatVolume: 읽기 전용 마운트 + 무상태 경로 탐색/읽기/
// 디렉터리 열거. `ExfatDriver`(VFS 통합 계층)는 이 계획 범위 밖 -
// libext4/libvfat 선례대로 이 클래스의 무상태 메서드는 실제 driver의
// onExec 코루틴 안에서는 재사용되지 않고(코루틴 합성 불가 제약,
// ext4_driver.cpp 상단 문서 주석 참고) 그 후속 계획이 평탄화해
// 다시 구현할 것이다 - 그래도 이 증분 자체의 독립적인 정확성 검증
// 가치가 있어(실제 mkfs.exfat 이미지 대조) libext4/libvfat과 동일한
// "먼저 무상태로, 나중에 평탄화" 순서를 그대로 따른다.
// ---------------------------------------------------------------------
class ExfatVolume {
public:
    // 부트 섹터(LBA 0)를 읽어 시그니처를 확인하고, 루트 디렉터리를
    // 스캔해 할당 비트맵(0x81)/Up-case 테이블(0x82) 특수 엔트리를
    // 찾아 그 내용 전체를 메모리에 캐싱한다.
    bool mount(fs::BlockDevice* device);

    uint32_t rootFirstCluster() const { return bs_.firstClusterOfRootDirectory; }

    // "/a/b/c" 형태의 절대 경로를 루트부터 세그먼트별로 탐색한다 -
    // 각 세그먼트를 Up-case 테이블로 정규화해 비교(대소문자 무시,
    // 원래 대소문자는 보존 - §3.6). v1은 ASCII 경로만 지원(세그먼트
    // 바이트를 그대로 UTF-16 코드유닛으로 폭 확장 후 비교 - 이
    // 프로젝트의 다른 파일시스템 API와 동일하게 커널 내부 경로가
    // 전부 ASCII라는 전제, 비ASCII 파일명은 후속).
    bool resolvePath(const char* path, uint32_t pathLen, ResolvedEntry* out);

    // 논리 오프셋 기준 읽기(POSIX pread 스타일, 상태 없음) - noFatChain
    // 이면 FAT를 참조하지 않고 firstCluster+오프셋 산술만으로 클러스터를
    // 찾는다(§3.2 핵심 최적화).
    uint32_t readData(uint32_t firstCluster, uint64_t fileSize, bool noFatChain, uint64_t offset, void* buf,
                       uint32_t len, bool* outOk);

    // 디렉터리(첫 클러스터로 식별)의 0-based 인덱스 순회 - 삭제된
    // 엔트리 집합(InUse 비트 꺼짐)과 0x81/0x82/0x83 특수 엔트리는
    // 자동으로 건너뛴다.
    bool readdirAt(uint32_t dirFirstCluster, uint64_t index, char* nameOut, uint32_t nameOutCap, uint32_t* outNameLen,
                   bool* outIsDir, uint32_t* outFirstCluster, uint64_t* outFileSize, bool* outNoFatChain);

private:
    // 32바이트 슬롯을 클러스터 체인에서 순서대로 읽어 주는 커서 -
    // 클러스터 경계를 넘어가는 엔트리 집합(0x85+0xC0+0xC1×N)도
    // 투명하게 처리한다. 중첩 클래스라 ExfatVolume의 private 멤버
    // (clusterToSector/nextCluster/device_ 등)에 그대로 접근한다 -
    // 정의는 exfat.cpp.
    class DirSlotCursor;

    bool clusterToSector(uint32_t cluster, uint32_t* outSector) const;
    // FAT 테이블에서 cluster의 다음 클러스터를 읽는다 - EOC/BAD/손상은
    // false(호출부가 "체인 끝"으로 처리). noFatChain 파일에는 호출되지
    // 않는다(호출부가 §3.2 산술로 대체).
    bool nextCluster(uint32_t cluster, uint32_t* outNext);
    // dirFirstCluster의 클러스터 체인(dirNoFatChain/dirDataLength로
    // §3.2 순회 방식 결정)을 순회하며 0x85로 시작하는 엔트리 집합을
    // 하나씩 파싱한다. nameUtf16Upper/nameLen이 null이 아니면 "이름
    // 일치 탐색"(찾으면 즉시 중단), null이면 "index-th 유효 엔트리
    // 찾기"(targetIndex 사용) 모드.
    bool scanDirectory(uint32_t dirFirstCluster, bool dirNoFatChain, uint64_t dirDataLength,
                        const uint16_t* nameUtf16Upper, uint32_t nameLen, uint64_t targetIndex, ResolvedEntry* out,
                        char* nameOut, uint32_t nameOutCap, uint32_t* outNameLen);
    void upcaseInPlace(uint16_t* codeUnits, uint32_t count) const;

    fs::BlockDevice* device_ = nullptr;
    ExfatBootSector bs_{};
    uint32_t sectorSize_ = 0;
    uint32_t clusterSize_ = 0;
    uint32_t sectorsPerCluster_ = 0;

    uint8_t* allocBitmap_ = nullptr;      // §3.3 - GenericSlabAllocator 소유, ceil(clusterCount/8)바이트
    uint32_t allocBitmapBytes_ = 0;
    uint16_t* upcaseTable_ = nullptr;     // §3.4 - GenericSlabAllocator 소유, upcaseTableEntries_개 uint16
    uint32_t upcaseTableEntries_ = 0;
};

}  // namespace exfat

#endif  // MINICORE_LIBEXFAT_EXFAT_H
