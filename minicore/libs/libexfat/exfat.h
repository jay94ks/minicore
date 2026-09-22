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

// ---------------------------------------------------------------------
// 4(부분) - ExfatVolume: 읽기 전용 마운트 + mount()가 캐싱한 상태의
// 읽기 전용 노출.
//
// [범위 변경, 2026-09-22, PN-C93A4E9E -> PN-09970F05로 갱신,
// libext4/libvfat이 먼저 겪은 것과 동일한 실측 제약] 원래 있던
// 무상태 동기 메서드(resolvePath/readdirAt + public readData +
// private 헬퍼 scanDirectory/DirSlotCursor/upcaseInPlace)는 전부
// 제거했다 - 전부 `fs::BlockDevice::readBlocks()`(Task 레벨 블로킹
// 동기 래퍼)를 쓰는데, 이건 `kernel::KernelFsDriver::onExec()`(코루틴,
// `AsyncReactor::drainOnce()` 안에서 실행)에서 호출하면 실측 확인된
// 무한 대기가 난다. `ExfatDriver`(exfat_driver.h/.cpp, PN-09970F05)가
// `kernel::AsyncTaskCoroAwaiter`(PN-6EDED542) 기반으로 `onExec` 자신의
// 코루틴 몸체 안에 이 로직을 평탄화해 다시 구현한다(`ext4_driver.cpp`/
// `vfat_driver.cpp`와 동일한 패턴). `mount()`만 여전히 실제로 쓰인다 -
// 진짜 `kernel::Task` 컨텍스트(`ExfatDriver::mount()`)에서 한 번
// 호출되는 준비 단계라 내부적으로 동기 헬퍼(clusterToSector/
// nextCluster/readData)를 계속 쓰는 게 안전하다(루트 디렉터리를 스캔해
// 할당 비트맵/Up-case 테이블을 로드해야 해서 ext4/FAT32의 mount()보다
// 스스로 할 일이 많다 - 이 셋은 mount() 전용 구현 세부로 private에
// 남긴다).
// ---------------------------------------------------------------------
class ExfatVolume {
public:
    // 부트 섹터(LBA 0)를 읽어 시그니처를 확인하고, 루트 디렉터리를
    // 스캔해 할당 비트맵(0x81)/Up-case 테이블(0x82) 특수 엔트리를
    // 찾아 그 내용 전체를 메모리에 캐싱한다.
    bool mount(fs::BlockDevice* device);

    // [PN-09970F05] mount()가 이미 파싱/캐싱해 둔 상태를 읽기 전용으로
    // 노출 - `ExfatDriver::onExec()`(코루틴 컨텍스트)가 이 상태를 그대로
    // 재사용해 자신만의 평탄화된 순회 로직을 구현하는 데 쓴다.
    uint32_t rootFirstCluster() const { return bs_.firstClusterOfRootDirectory; }
    fs::BlockDevice* device() const { return device_; }
    uint32_t sectorSizeValue() const { return sectorSize_; }
    uint32_t clusterSizeValue() const { return clusterSize_; }
    uint32_t sectorsPerClusterValue() const { return sectorsPerCluster_; }
    uint32_t clusterHeapOffsetValue() const { return bs_.clusterHeapOffset; }
    uint32_t fatOffsetValue() const { return bs_.fatOffset; }
    const uint16_t* upcaseTablePtr() const { return upcaseTable_; }
    uint32_t upcaseTableEntriesValue() const { return upcaseTableEntries_; }

private:
    bool clusterToSector(uint32_t cluster, uint32_t* outSector) const;
    // FAT 테이블에서 cluster의 다음 클러스터를 읽는다 - EOC/BAD/손상은
    // false(호출부가 "체인 끝"으로 처리). noFatChain 파일에는 호출되지
    // 않는다(호출부가 §3.2 산술로 대체).
    bool nextCluster(uint32_t cluster, uint32_t* outNext);
    // 논리 오프셋 기준 읽기(mount()가 할당 비트맵/Up-case 테이블
    // 본문을 로드하는 데만 쓴다 - onExec은 이 메서드를 재사용하지
    // 않는다, 위 클래스 문서 주석 참고).
    uint32_t readData(uint32_t firstCluster, uint64_t fileSize, bool noFatChain, uint64_t offset, void* buf,
                       uint32_t len, bool* outOk);

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
