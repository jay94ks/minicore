#ifndef MINICORE_LIBVFAT_VFAT_H
#define MINICORE_LIBVFAT_VFAT_H

#include "libkenv/types.h"

namespace fs {
class BlockDevice;
}

// SP-A658A124 - FAT32 온디스크 포맷 읽기 라이브러리(1차 증분,
// PN-F32F55A8). 이 프로젝트가 새로 고안한 포맷이 아니다 - 실제
// Microsoft FAT32 스펙/Linux 커널 소스(include/uapi/linux/msdos_fs.h)
// 와 1바이트 단위로 대조해 확정했다(libext4/libswapfs와 동일한 절차).
//
// [범위, 2026-09-22, PN-F32F55A8] FAT32만 구현한다 - 계획 본문이
// "FAT16도 지원한다면 구현 세션 재량"으로 열어 둔 부분을, libext4/
// libswapfs와 같은 이유(검증 가능한 단위로 쪼개기)로 이번 증분은
// FAT32 하나에 집중하고 FAT16은 후속으로 미룬다(SP-2BCE5D60 §1의
// 우선순위 - ext4가 최우선, FAT는 ESP/USB 상호운용 목적이라 FAT32가
// 실질적으로 더 흔함). LFN(긴 파일명)도 계획이 이미 "1차 증분 범위
// 밖"으로 명시한 대로 읽기 스캔 중 건너뛴다(스펙 자체의 하위호환
// 보장 - LFN 엔트리를 모르는 구현은 무시하고 짧은 이름만 봐도 된다).
namespace vfat {

using kernel::uint8_t;
using kernel::uint16_t;
using kernel::uint32_t;
using kernel::uint64_t;

// ---------------------------------------------------------------------
// 3.1 부트 섹터/BPB - LBA 0, 512바이트. 오프셋 0~35는 FAT12/16/32
// 공통(Linux include/uapi/linux/msdos_fs.h의 struct fat_boot_sector
// 앞부분과 1바이트 단위로 대조), 오프셋 36부터 FAT32 전용 확장.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct BpbCommon {
    uint8_t  jmpBoot[3];
    char     oemName[8];
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectorCount;
    uint8_t  numFats;
    uint16_t rootEntryCount;      // FAT32는 반드시 0
    uint16_t totalSectors16;      // FAT32는 반드시 0(totalSectors32 사용)
    uint8_t  media;
    uint16_t fatSize16;           // FAT32는 반드시 0(fatSize32 사용)
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
};
static_assert(sizeof(BpbCommon) == 36, "BpbCommon 레이아웃이 msdos_fs.h와 어긋남");

struct Fat32Extended {
    uint32_t fatSize32;
    uint16_t extFlags;
    uint16_t fsVersion;    // 0이어야 함
    uint32_t rootCluster;
    uint16_t fsInfoSector;
    uint16_t backupBootSector;
    uint8_t  reserved[12];
    uint8_t  driveNumber;
    uint8_t  reserved1;
    uint8_t  bootSig;      // 0x28/0x29면 volumeId/volumeLabel 유효(스펙상 참고용, mount() 판별엔 안 씀)
    uint32_t volumeId;
    char     volumeLabel[11];
    char     fileSystemType[8];  // 진단용 - 실제 포맷 판별에 안 씀(§3.2, 스펙이 이 필드를 신뢰하지 말라고 명시)
};
static_assert(sizeof(Fat32Extended) == 54, "Fat32Extended 레이아웃이 msdos_fs.h와 어긋남");
#pragma pack(pop)

constexpr uint32_t kBootSectorSignatureOffset = 510;
constexpr uint16_t kBootSectorSignature = 0xAA55;
constexpr uint32_t kFat32ExtendedOffset = 36;  // sizeof(BpbCommon)과 항상 같아야 함

// ---------------------------------------------------------------------
// 3.4 FAT32 엔트리(32비트, 하위 28비트만 유효) - 클러스터 체인.
// ---------------------------------------------------------------------
constexpr uint32_t kFatEntryMask = 0x0FFFFFFFu;
constexpr uint32_t kFatEocMin = 0x0FFFFFF8u;   // 이상이면 체인 끝
constexpr uint32_t kFatBadCluster = 0x0FFFFFF7u;
constexpr uint32_t kFirstDataCluster = 2;      // 클러스터 번호는 2부터 시작(0/1 예약)

// ---------------------------------------------------------------------
// 3.5 디렉터리 엔트리(32바이트, 8.3 짧은 이름).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct DirEntry {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  ntReserved;
    uint8_t  crtTimeTenth;
    uint16_t crtTime;
    uint16_t crtDate;
    uint16_t lastAccessDate;
    uint16_t fstClusHi;
    uint16_t wrtTime;
    uint16_t wrtDate;
    uint16_t fstClusLo;
    uint32_t fileSize;
};
static_assert(sizeof(DirEntry) == 32, "DirEntry는 32바이트여야 함");
#pragma pack(pop)

constexpr uint8_t kAttrReadOnly = 0x01;
constexpr uint8_t kAttrHidden = 0x02;
constexpr uint8_t kAttrSystem = 0x04;
constexpr uint8_t kAttrVolumeId = 0x08;
constexpr uint8_t kAttrDirectory = 0x10;
constexpr uint8_t kAttrArchive = 0x20;
constexpr uint8_t kAttrLongName = kAttrReadOnly | kAttrHidden | kAttrSystem | kAttrVolumeId;  // 0x0F

constexpr uint8_t kNameFreeRestMarker = 0x00;   // 이 엔트리부터 디렉터리 끝(더 이상 유효 엔트리 없음)
constexpr uint8_t kNameDeletedMarker = 0xE5;
constexpr uint8_t kNameEscapedE5 = 0x05;        // 실제 파일명 첫 글자가 0xE5인 경우의 이스케이프

inline uint32_t kFatFirstCluster(const DirEntry& e) {
    return (static_cast<uint32_t>(e.fstClusHi) << 16) | e.fstClusLo;
}

// resolvePath()/findDirEntry()가 돌려주는, 디렉터리 엔트리 하나의
// 요약 - FAT엔 ext4의 inode 같은 별도 메타데이터 테이블이 없어서
// (크기/디렉터리 여부가 전부 부모 디렉터리 엔트리 자신에 있음) 매번
// 이 세 값을 통째로 들고 다닌다.
struct ResolvedEntry {
    uint32_t firstCluster = 0;
    uint64_t fileSize = 0;   // 디렉터리는 항상 0(스펙 - 크기는 클러스터 체인 길이로만 앎)
    bool isDir = false;
};

// ---------------------------------------------------------------------
// 4. Fat32Volume - 읽기 전용 마운트/경로 탐색/읽기/디렉터리 열거.
//
// [범위, 2026-09-22, PN-F32F55A8] SP-A658A124 §4는 이 클래스를
// `FileSystemDriver`(SP-2BCE5D60 §3.1)를 구현하는 `Fat32Driver`로
// 스케치했다 - `PN-22784AD4`(libext4)에서 이미 발견한 것과 동일한
// 이유(`QU-08ACD701`, 그 인터페이스가 실제 코드에 없고 fs가
// `KernelFsDriver` 패턴으로 옮겨간 것과 어긋남)로 이번 증분은 §3
// (온디스크 포맷)과 무상태 읽기 API만 구현하고 §4(VFS 통합 계층)는
// 만들지 않는다 - `QU-08ACD701` 답변을 기다린다. 쓰기 경로(mkdir/
// unlink/write)도 이번 증분에 포함하지 않는다(libext4와 동일한
// "작은 단위로 쪼개 순서대로 완성" 원칙).
// ---------------------------------------------------------------------
class Fat32Volume {
public:
    // 부트 섹터(LBA 0)를 읽어 시그니처/BPB를 확인하고, §3.2 클러스터
    // 수 계산으로 실제 FAT32인지 재확인한다(fileSystemType 문자열은
    // 신뢰하지 않는다 - 스펙 원문의 경고 그대로).
    bool mount(fs::BlockDevice* device);

    // 이 볼륨의 루트 디렉터리 첫 클러스터(BPB의 rootCluster) - 최상위
    // 탐색의 시작점.
    uint32_t rootFirstCluster() const { return ext32_.rootCluster; }

    // "/a/b/c" 형태의 절대 경로를 루트부터 세그먼트별로 탐색한다 -
    // 각 세그먼트를 8.3 형식으로 정규화해 비교(대소문자 무시, 스펙
    // 자체가 8.3 이름을 항상 대문자로 저장).
    bool resolvePath(const char* path, uint32_t pathLen, ResolvedEntry* out);

    // 논리 오프셋 기준 읽기(POSIX pread 스타일, 상태 없음) - FAT엔
    // 재조회 가능한 inode 번호가 없어 firstCluster/fileSize를 호출부가
    // (resolvePath 결과 그대로) 매번 넘긴다.
    uint32_t readData(uint32_t firstCluster, uint64_t fileSize, uint64_t offset, void* buf, uint32_t len,
                       bool* outOk);

    // 디렉터리(첫 클러스터로 식별)의 0-based 인덱스 순회 - LFN(0x0F)/
    // 삭제됨(0xE5)/볼륨 라벨(VOLUME_ID) 엔트리는 자동으로 건너뛴다.
    bool readdirAt(uint32_t dirFirstCluster, uint64_t index, char* nameOut, uint32_t nameOutCap,
                   uint32_t* outNameLen, bool* outIsDir, uint32_t* outFirstCluster);

private:
    bool clusterToSector(uint32_t cluster, uint32_t* outSector) const;
    // FAT 테이블에서 cluster의 다음 클러스터를 읽는다 - EOC/BAD/손상은
    // false(호출부가 "체인 끝"으로 처리).
    bool nextCluster(uint32_t cluster, uint32_t* outNext);
    bool findDirEntry(uint32_t dirFirstCluster, const char* name, uint32_t nameLen, ResolvedEntry* out);

    fs::BlockDevice* device_ = nullptr;
    BpbCommon bpb_{};
    Fat32Extended ext32_{};
    uint32_t bytesPerCluster_ = 0;
    uint32_t fatStartSector_ = 0;
    uint32_t dataStartSector_ = 0;
};

}  // namespace vfat

#endif  // MINICORE_LIBVFAT_VFAT_H
