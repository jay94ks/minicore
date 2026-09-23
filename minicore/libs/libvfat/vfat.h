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

// [신규, 2026-09-23, PN-5481287C 준비 작업 2단계] FAT12/FAT16 공용
// 확장 BPB - Fat32Extended에서 FAT32 전용 필드(fatSize32/extFlags/
// fsVersion/rootCluster/fsInfoSector/backupBootSector/reserved[12],
// 도합 28바이트)를 뺀 나머지가 오프셋만 36(BpbCommon 바로 뒤, FAT32는
// 대신 그 28바이트가 먼저 옴)으로 당겨져 그대로 반복된다 - Linux
// `msdos_fs.h`와 실측(`mkfs.fat -F 12`/`-F 16`으로 만든 실제 이미지의
// 바이트 36~61을 직접 덤프) 둘 다로 확인: driveNumber=0x80/
// bootSig=0x29/volumeId가 리틀엔디안 4바이트/volumeLabel="MYFAT12    "
// (11바이트, 공백 패딩)/fileSystemType="FAT12   "(8바이트) 전부 이
// 오프셋 그대로 일치.
struct Fat16Extended {
    uint8_t  driveNumber;
    uint8_t  reserved1;
    uint8_t  bootSig;             // 0x28/0x29면 volumeId/volumeLabel 유효(Fat32Extended와 동일 관례)
    uint32_t volumeId;
    char     volumeLabel[11];
    char     fileSystemType[8];   // "FAT12   " 또는 "FAT16   " - 진단용, 실제 포맷 판별에 안 씀(Fat32Extended와 동일 주의)
};
static_assert(sizeof(Fat16Extended) == 26, "Fat16Extended 레이아웃이 msdos_fs.h와 어긋남");
#pragma pack(pop)

constexpr uint32_t kBootSectorSignatureOffset = 510;
constexpr uint16_t kBootSectorSignature = 0xAA55;
constexpr uint32_t kFat32ExtendedOffset = 36;  // sizeof(BpbCommon)과 항상 같아야 함
constexpr uint32_t kFat16ExtendedOffset = 36;  // FAT12/16은 FAT32 전용 28바이트가 없어 BpbCommon 바로 뒤

// ---------------------------------------------------------------------
// 3.4 FAT32 엔트리(32비트, 하위 28비트만 유효) - 클러스터 체인.
// ---------------------------------------------------------------------
constexpr uint32_t kFatEntryMask = 0x0FFFFFFFu;
constexpr uint32_t kFatEocMin = 0x0FFFFFF8u;   // 이상이면 체인 끝
constexpr uint32_t kFatBadCluster = 0x0FFFFFF7u;
constexpr uint32_t kFirstDataCluster = 2;      // 클러스터 번호는 2부터 시작(0/1 예약)
constexpr uint32_t kReservedFatEntryIndex = 1; // 클러스터 0/1은 실제 체인이 아니라 예약(볼륨 dirty 비트는 FAT[1]에)

// ---------------------------------------------------------------------
// [신규, 2026-09-23, PN-5481287C 준비 작업] FAT12 엔트리 - 12비트씩
// 패킹돼(1.5바이트/엔트리) 바이트 경계와 어긋난다. 이 프로젝트가 새로
// 고안한 값이 아니다 - Linux 커널 `fs/fat/fatent.c`의
// `fat12_ent_get`/`fat12_ent_put`과 동일한 알고리즘(fatgen103 문서
// 관례 그대로). 순수 계산 함수만 여기 있고, 아직 어디서도 호출되지
// 않는다 - 실제 마운트/읽기 경로(§3.3의 FAT12/16 전용 고정 루트
// 디렉터리 영역 처리 포함)는 훨씬 큰 후속 작업(별도 Volume/Driver
// 클래스 필요)으로 남아 있다. 순수 pack/unpack 로직만 먼저 호스트
// 사이드 단위 테스트로 검증해 둔 것(8개 케이스: 짝/홀 클러스터
// get, round-trip set 후 이웃 엔트리 훼손 여부까지 확인 - 전부
// 통과).
// ---------------------------------------------------------------------
// cluster번째 FAT12 엔트리가 시작하는 바이트 오프셋 - floor(cluster*1.5).
// 엔트리 하나가 이 오프셋과 그 다음 바이트에 걸쳐 있다(짝수 클러스터는
// 하위 12비트, 홀수 클러스터는 상위 12비트).
inline uint32_t kFat12EntryByteOffset(uint32_t cluster) {
    return cluster + cluster / 2;
}

inline uint16_t kFat12EntryGet(const uint8_t* fat, uint32_t cluster) {
    const uint32_t off = kFat12EntryByteOffset(cluster);
    if (cluster & 1) {
        return static_cast<uint16_t>((fat[off] >> 4) | (fat[off + 1] << 4));
    }
    return static_cast<uint16_t>(fat[off] | ((fat[off + 1] & 0x0F) << 8));
}

inline void kFat12EntryPut(uint8_t* fat, uint32_t cluster, uint16_t value) {
    const uint32_t off = kFat12EntryByteOffset(cluster);
    if (cluster & 1) {
        fat[off] = static_cast<uint8_t>((value << 4) | (fat[off] & 0x0F));
        fat[off + 1] = static_cast<uint8_t>(value >> 4);
    } else {
        fat[off] = static_cast<uint8_t>(value & 0xFF);
        fat[off + 1] = static_cast<uint8_t>((fat[off + 1] & 0xF0) | (value >> 8));
    }
}

// [신규, 2026-09-23, PN-547EF839, SP-A658A124 §2 후속 증분 항목6] FAT[1]
// (예약 엔트리) 상위 비트의 볼륨 dirty 관례 - 이 프로젝트가 새로
// 고안한 값이 아니다. Microsoft "FAT: General Overview of On-Disk
// Format"(fatgen103) 및 Linux 커널 fs/fat/fat.h의 FAT32 정의와 동일:
// 비트27=1이면 마지막으로 정상 언마운트됨("clean shutdown"), 비트26=1
// 이면 마지막 마운트 중 하드웨어 오류 없었음("no hw error"). v1은
// "정상 언마운트 여부"만 다룬다(SP-A658A124 §2 문구 그대로) - 이
// 커널이 아직 디스크 I/O 오류를 추적하는 메커니즘이 없어 hw-error
// 비트는 건드리지 않는다(RM-23F4B687 §4, 실제로 겪어본 뒤 재검토).
constexpr uint32_t kFat32DirtyBitCleanShutdown = 0x08000000u;  // bit 27

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

// [신규, 2026-09-23, PN-8CACD042, SP-A658A124 §2 후속 증분 항목5] NT/VFAT
// 확장 - `ntReserved` 바이트의 이 두 비트가 서면 온디스크는 대문자로
// 유지한 채 표시만 소문자로 한다(다른 OS가 만든 이미지와의 표시
// 호환성). 이 프로젝트가 새로 고안한 값이 아니다 - Linux 커널
// `include/uapi/linux/msdos_fs.h`의 `CASE_LOWER_BASE`(8)/
// `CASE_LOWER_EXT`(16)와 동일(libvfat의 다른 모든 상수와 같은 원칙,
// RM-23F4B687 §4 - 실제 스펙/리눅스 소스와 대조).
constexpr uint8_t kNtCaseLowerBase = 0x08;  // 8.3 이름의 "이름" 부분이 소문자로 표시돼야 함
constexpr uint8_t kNtCaseLowerExt = 0x10;   // 8.3 이름의 "확장자" 부분이 소문자로 표시돼야 함

// ---------------------------------------------------------------------
// 3.6 LFN(Long File Name) 슬롯 - attr==kAttrLongName(0x0F)인 디렉터리
// 엔트리를 이 레이아웃으로 재해석한다(DirEntry와 크기만 같은 별개
// 오버레이, Linux msdos_fs.h의 struct msdos_dir_slot과 1바이트 단위로
// 대조). 짧은 이름 엔트리 바로 앞에 시퀀스 번호 역순(높은 번호가 먼저)
// 으로 나열되고, 최상위 비트(kLfnLastEntryFlag)가 선 그 체인의
// "마지막 논리 조각"(=이름의 끝부분)임을 표시한다 - PN-1A224EC2.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct LfnSlot {
    uint8_t  id;
    uint16_t name0_4[5];
    uint8_t  attr;       // 항상 kAttrLongName(0x0F)
    uint8_t  slotType;   // 항상 0
    uint8_t  checksum;   // 뒤따르는 8.3 짧은 이름의 체크섬(kLfnChecksum 참고)
    uint16_t name5_10[6];
    uint16_t startCluster;  // 항상 0(레거시 필드, LFN 슬롯엔 의미 없음)
    uint16_t name11_12[2];
};
static_assert(sizeof(LfnSlot) == 32, "LfnSlot은 DirEntry와 같은 32바이트여야 함");
#pragma pack(pop)

constexpr uint8_t kLfnLastEntryFlag = 0x40;
constexpr uint8_t kLfnSeqMask = 0x1F;
constexpr uint32_t kLfnMaxSlots = 20;          // 20*13=260자 >= NAME_MAX(255)
constexpr uint32_t kLfnCharsPerSlot = 13;       // 5+6+2

// 8.3 짧은 이름(11바이트, 대문자 정규화된 그대로)의 체크섬 - 스펙
// 알고리즘 그대로(FAT: General Overview of On-Disk Format 문서).
inline uint8_t kLfnChecksum(const char name11[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; ++i) {
        sum = static_cast<uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + static_cast<uint8_t>(name11[i]));
    }
    return sum;
}

inline uint32_t kFatFirstCluster(const DirEntry& e) {
    return (static_cast<uint32_t>(e.fstClusHi) << 16) | e.fstClusLo;
}

// Fat32Driver::onExec()이 `FileHandle::value`에 인코딩해 들고
// 다니는, 디렉터리 엔트리 하나의 요약 - FAT엔 ext4의 inode 같은 별도
// 메타데이터 테이블이 없어서(크기/디렉터리 여부가 전부 부모 디렉터리
// 엔트리 자신에 있음) 이 세 값을 통째로 들고 다녀야 한다.
struct ResolvedEntry {
    uint32_t firstCluster = 0;
    uint64_t fileSize = 0;   // 디렉터리는 항상 0(스펙 - 크기는 클러스터 체인 길이로만 앎)
    bool isDir = false;
    // [신규, 2026-09-23, PN-9D6FE4B6, QU-E4E83A9A 답변("(A) 별도
    // open-handle 테이블 도입")] 이 엔트리 자신의 디렉터리 엔트리가
    // 어느 클러스터의 몇 번째 바이트에 있는지 - write가 파일 끝을
    // 넘을 때 fileSize/firstCluster를 다시 써넣거나 unlink가 name[0]을
    // 지우려면 이 위치가 필요하다(vfat_driver.h의 OpenHandleEntry
    // 문서 주석 참고). 루트 디렉터리는 부모 디렉터리 엔트리 자체가
    // 없으므로 이 값들은 정의되지 않는다.
    uint32_t entryCluster = 0;
    uint32_t entryByteOffset = 0;
};

// ---------------------------------------------------------------------
// 4. Fat32Volume - 읽기 전용 마운트 + 온디스크 레이아웃 상수 접근자.
//
// [범위 변경, 2026-09-22, PN-EBAEA67B, PN-9AE5BFE4(Ext4Driver)가 먼저
// 겪은 것과 동일한 실측 제약] 원래 있던 무상태 동기 메서드
// (resolvePath/readData/readdirAt + private 헬퍼 clusterToSector/
// nextCluster/findDirEntry)는 전부 제거했다 - 전부 내부적으로
// `fs::BlockDevice::readBlocks()`(Task 레벨 블로킹 동기 래퍼)를
// 쓰는데, 이건 `kernel::KernelFsDriver::onExec()`(코루틴,
// `AsyncReactor::drainOnce()` 안에서 실행)에서 호출하면 실측 확인된
// 무한 대기가 난다(`QU-FF7044DA` 설계자 답변 - "커널 내의 모든 동작은
// 비동기 프레임워크 기반으로"). `Fat32Driver`(vfat_driver.h/.cpp)가
// `kernel::AsyncTaskCoroAwaiter`(PN-6EDED542) 기반으로 `onExec` 자신의
// 코루틴 몸체 안에 이 로직을 평탄화해 다시 구현한다
// (`minicore/libs/libext4/ext4_driver.cpp`와 동일한 패턴 - 자세한
// 이유는 그 파일 상단 문서 주석 참고). 이 클래스엔 `mount()`와, 그
// 평탄화된 onExec가 스스로 I/O를 조립하는 데 필요한 읽기 전용
// 접근자만 남긴다.
// ---------------------------------------------------------------------
class Fat32Volume {
public:
    // 부트 섹터(LBA 0)를 읽어 시그니처/BPB를 확인하고, §3.2 클러스터
    // 수 계산으로 실제 FAT32인지 재확인한다(fileSystemType 문자열은
    // 신뢰하지 않는다 - 스펙 원문의 경고 그대로). 진짜 kernel::Task
    // 컨텍스트(Fat32Driver::mount(), MountTable::mountKernel() 등록
    // 이전 1회 준비 단계)에서만 호출하는 게 안전 - onExec 코루틴
    // 안에서는 호출하지 않는다(위 클래스 문서 주석).
    bool mount(fs::BlockDevice* device);

    // 이 볼륨의 루트 디렉터리 첫 클러스터(BPB의 rootCluster) - 최상위
    // 탐색의 시작점.
    uint32_t rootFirstCluster() const { return ext32_.rootCluster; }

    fs::BlockDevice* device() const { return device_; }
    uint32_t bytesPerSectorValue() const { return bpb_.bytesPerSector; }
    uint32_t sectorsPerClusterValue() const { return bpb_.sectorsPerCluster; }
    uint32_t bytesPerClusterValue() const { return bytesPerCluster_; }
    uint32_t fatStartSectorValue() const { return fatStartSector_; }
    uint32_t dataStartSectorValue() const { return dataStartSector_; }
    // [신규, 2026-09-23, PN-9D6FE4B6 준비 작업 - §3.4] free 클러스터
    // 스캔의 종료 조건(클러스터 번호는 kFirstDataCluster(2)부터
    // clusterCount_+1까지 유효)과 다중 FAT 사본 동기화에 필요.
    uint32_t clusterCountValue() const { return clusterCount_; }
    uint32_t numFatsValue() const { return numFats_; }
    uint32_t fatSize32Value() const { return fatSize32_; }

private:
    fs::BlockDevice* device_ = nullptr;
    BpbCommon bpb_{};
    Fat32Extended ext32_{};
    uint32_t bytesPerCluster_ = 0;
    uint32_t fatStartSector_ = 0;
    uint32_t dataStartSector_ = 0;
    uint32_t clusterCount_ = 0;
    uint32_t numFats_ = 0;
    uint32_t fatSize32_ = 0;
};

}  // namespace vfat

#endif  // MINICORE_LIBVFAT_VFAT_H
