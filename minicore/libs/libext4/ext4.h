#ifndef MINICORE_LIBEXT4_EXT4_H
#define MINICORE_LIBEXT4_EXT4_H

#include "libkenv/types.h"

namespace fs {
class BlockDevice;
}

// SP-7A9CED3E - ext4 온디스크 포맷 읽기 라이브러리(1차 증분, PN-22784AD4).
// 이 프로젝트가 새로 고안한 포맷이 아니다 - 실제 Linux ext4를 그대로
// 읽는다(CLAUDE.md 규칙4). 아래 struct 레이아웃은 리눅스 커널 소스
// (fs/ext4/ext4.h, fs/ext4/ext4_extents.h, torvalds/linux master)와
// 1바이트 단위로 대조해 확정했다(PN-22784AD4/SP-7A9CED3E §1이 요구한
// 재확인 - libswapfs의 mkswap 대조와 동일한 절차).
namespace ext4 {

using kernel::uint8_t;
using kernel::uint16_t;
using kernel::uint32_t;
using kernel::uint64_t;

// ---------------------------------------------------------------------
// 3.1 슈퍼블록 - 블록 장치 오프셋 1024바이트(블록 크기와 무관하게 고정).
// 확장 필드(저널/htree/64bit/체크섬 등, algorithmUsageBitmap 이후)는
// v1이 읽지 않는다 - SP-7A9CED3E §1이 이미 "이 문서 하나로 정확성을
// 단언하지 않는다"고 명시한 부분, 실제로 읽는 core 필드만 옮긴다.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct SuperblockCore {
    uint32_t inodesCount;
    uint32_t blocksCountLo;
    uint32_t rBlocksCountLo;
    uint32_t freeBlocksCountLo;
    uint32_t freeInodesCount;
    uint32_t firstDataBlock;
    uint32_t logBlockSize;
    uint32_t logClusterSize;
    uint32_t blocksPerGroup;
    uint32_t clustersPerGroup;
    uint32_t inodesPerGroup;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mntCount;
    uint16_t maxMntCount;
    uint16_t magic;              // 0xEF53 - 오프셋 56(리눅스 소스로 재확인 완료)
    uint16_t state;
    uint16_t errors;
    uint16_t minorRevLevel;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creatorOs;
    uint32_t revLevel;           // EXT4_DYNAMIC_REV=1이어야 아래 확장 필드 유효
    uint16_t defResuid;
    uint16_t defResgid;
    // -- EXT4_DYNAMIC_REV 확장 시작 (오프셋 84 = 0x54) --
    uint32_t firstIno;
    uint16_t inodeSize;
    uint16_t blockGroupNr;
    uint32_t featureCompat;
    uint32_t featureIncompat;    // mount() 판별에 씀 - kExt4Incompat* 참고
    uint32_t featureRoCompat;
    uint8_t  uuid[16];
    char     volumeName[16];
    char     lastMounted[64];
    uint32_t algorithmUsageBitmap;
    // 이후 저널/64bit/checksum 등 확장 필드는 총 1024바이트 중 나머지 -
    // v1이 안 읽는 필드라 이 struct에 옮기지 않는다(실제 읽기는 항상
    // 1024바이트를 통째로 읽어 이 struct 크기만큼만 해석하는 방식).
};
static_assert(sizeof(SuperblockCore) == 204, "SuperblockCore 레이아웃이 리눅스 소스와 어긋남");
#pragma pack(pop)

constexpr uint32_t kSuperblockOffset = 1024;
constexpr uint16_t kMagic = 0xEF53;
constexpr uint32_t kStateValidFs = 0x1;

constexpr uint32_t kIncompatFiletype = 0x2;
constexpr uint32_t kIncompatExtents = 0x40;
constexpr uint32_t kIncompatFlexBg = 0x200;
constexpr uint32_t kIncompat64Bit = 0x80;
// [실측 확인, 2026-09-22] SP-7A9CED3E §2.1은 EXTENTS+FILETYPE만
// 필수로 검사하도록 설계했으나, 실제 `mkfs.ext4`(util-linux/e2fsprogs
// 기본값)가 만드는 이미지는 FLEX_BG incompat 비트도 항상 함께
// 세운다(그룹별 비트맵/inode 테이블을 flex group 첫 그룹에 몰아
// 배치하는 최적화) - 이 비트를 허용 목록에서 빼면 mkfs.ext4 기본
// 이미지를 전부 거부하게 된다. 다행히 flex_bg는 각 그룹 디스크립터의
// blockBitmapLo/inodeBitmapLo/inodeTableLo가 여전히 정확한 절대
// 블록 번호를 담고 있어(그 값이 어느 그룹에 물리적으로 있든) v1의
// "그룹 디스크립터가 가리키는 곳을 그대로 따라간다" 읽기 경로에
// 아무 영향이 없다 - 그래서 목적어 없이 허용 목록에 추가한다(실제
// 파서 코드 변경 불필요, 검사만 통과시키면 됨).
constexpr uint32_t kSupportedIncompatMask = kIncompatFiletype | kIncompatExtents | kIncompatFlexBg;

// ---------------------------------------------------------------------
// 3.2 블록 그룹 디스크립터(32바이트, INCOMPAT_64BIT 미지원 - §2.2 후속).
// [정정, 2026-09-22] SP-7A9CED3E §3.2 원안은 reserved[3](12바이트) 뒤에
// itableUnusedLo/checksum 필드를 추가로 뒀으나, 그러면 struct 전체가
// 36바이트가 돼 실제 32바이트 레이아웃과 어긋난다(리눅스 소스 대조로
// 확인 - bg_exclude_bitmap_lo(4)+bg_block_bitmap_csum_lo(2)+
// bg_inode_bitmap_csum_lo(2)+bg_itable_unused_lo(2)+bg_checksum(2)=12
// 바이트가 정확히 reserved[3]의 20~31 오프셋 전체를 채운다 - v1은 이
// 값들 중 아무것도 안 읽으므로 reserved로 전부 묶는 편이 맞다). 다음
// 그룹 디스크립터를 4바이트 밀려서 잘못 읽는 실제 버그였다.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct GroupDesc32 {
    uint32_t blockBitmapLo;
    uint32_t inodeBitmapLo;
    uint32_t inodeTableLo;
    uint16_t freeBlocksCountLo;
    uint16_t freeInodesCountLo;
    uint16_t usedDirsCountLo;
    uint16_t flags;
    uint8_t  reserved[12];  // excludeBitmapLo+두 csum+itableUnusedLo+checksum, v1 미사용
};
static_assert(sizeof(GroupDesc32) == 32, "GroupDesc32는 정확히 32바이트여야 함");
#pragma pack(pop)

// ---------------------------------------------------------------------
// 3.4 inode 구조체(core 128바이트, inodeSize>128이면 나머지는 확장
// 필드 - v1은 읽지 않음) + 익스텐트 트리.
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct InodeCore {
    uint16_t mode;
    uint16_t uid;
    uint32_t sizeLo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t linksCount;
    uint32_t blocksLo;   // 512바이트 섹터 단위 - 주의, 블록 단위 아님
    uint32_t flags;      // kExtentsFl이 반드시 서 있어야 함(레거시 간접 블록은 §2.2 후속)
    uint32_t osd1;
    uint8_t  block[60];  // kExtentsFl 켜짐 - ExtentHeader+엔트리 인라인
    uint32_t generation;
    uint32_t fileAclLo;
    uint32_t sizeHigh;
    uint32_t obsoFaddr;
    uint8_t  osd2[12];
    uint16_t extraIsize;
    uint16_t checksumHi;
};
// 128(EXT4_GOOD_OLD_INODE_SIZE, extraIsize 이전까지)이 아니라 132다 -
// extraIsize/checksumHi 두 필드(오프셋 128/130, 리눅스 소스로 재확인)
// 까지 이 struct에 포함시켰기 때문에 정확한 값.
static_assert(sizeof(InodeCore) == 132, "InodeCore 레이아웃이 리눅스 소스와 어긋남");
#pragma pack(pop)

constexpr uint32_t kExtentsFl = 0x80000;
constexpr uint16_t kExtentMagic = 0xF30A;
constexpr uint32_t kExtentUninitLenBit = 0x8000;  // ee_len 최상위 비트 - uninitialized 익스텐트

#pragma pack(push, 1)
struct ExtentHeader {
    uint16_t magic;
    uint16_t entries;
    uint16_t max;
    uint16_t depth;   // 0=리프(Extent 배열), >0=내부노드(ExtentIdx 배열)
    uint32_t generation;
};
static_assert(sizeof(ExtentHeader) == 12, "ExtentHeader는 12바이트");

struct Extent {           // depth==0
    uint32_t block;      // 이 익스텐트의 첫 논리 블록 번호
    uint16_t len;         // 상위 비트=uninitialized, 하위 15비트=길이(<=32768)
    uint16_t startHi;
    uint32_t startLo;
};
static_assert(sizeof(Extent) == 12, "Extent는 12바이트");

struct ExtentIdx {        // depth>0
    uint32_t block;
    uint32_t leafLo;
    uint16_t leafHi;
    uint16_t unused;
};
static_assert(sizeof(ExtentIdx) == 12, "ExtentIdx는 12바이트");
#pragma pack(pop)

// ---------------------------------------------------------------------
// 3.5 디렉터리 엔트리(INCOMPAT_FILETYPE 필수 - §2.1).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct DirEntry2Header {
    uint32_t inode;
    uint16_t recLen;
    uint8_t  nameLen;
    uint8_t  fileType;
    // char name[nameLen] - 가변 길이라 이 struct 뒤에 바로 이어짐(널 종단 없음)
};
static_assert(sizeof(DirEntry2Header) == 8, "DirEntry2Header는 8바이트");
#pragma pack(pop)

constexpr uint8_t kFtRegFile = 1;
constexpr uint8_t kFtDir = 2;

constexpr uint32_t kMaxNameLen = 255;   // ext4 NAME_LEN

// ---------------------------------------------------------------------
// 4. Ext4Volume - 읽기 전용 마운트/경로 탐색/읽기/디렉터리 열거.
//
// [범위, 2026-09-22, PN-22784AD4] SP-7A9CED3E §4는 이 클래스를
// `FileSystemDriver`(SP-2BCE5D60 §3.1, open/close/read/write/stat/
// mkdir/rmdir/unlink/readdir 9개 가상함수)를 구현하는 `Ext4Driver`로
// 스케치했다 - 하지만 실제 코드베이스는 이미 그 인터페이스를 쓰지
// 않는다: `fs`가 SP-43331889로 순수 커널 KernelThread에 흡수된 뒤
// LiveFs/ProcFs/ResourceGroupFs 전부 `mount_table.h`의
// `KernelFsDriver`(AsyncTaskHandler 상속, subjectCode 기반 비동기
// op 제출)로 이미 구현돼 있다 - SP-2BCE5D60 §3.1의 동기 가상함수
// 인터페이스는 그 문서가 여전히 "fs 서비스(유저랜드)"를 전제하던
// 시절의 설계라 지금 구조와 어긋난다(libswapfs의 SwapBackend가
// 겪은 것과 같은 종류의 설계 공백, 다만 그때보다 훨씬 근본적 -
// 매크로 하나가 아니라 호출 규약 자체가 다르다). 이 불일치는
// `QU-...`(이 커밋과 함께 등록)로 설계자에게 확인을 요청했다 -
// Ext4Driver를 KernelFsDriver 패턴으로 새로 만들지, 아니면
// FileSystemDriver 쪽을 그 패턴에 맞게 재정의할지는 설계자 결정
// 사항(CLAUDE.md 규칙4 - 임의로 정하지 않음).
//
// 그래서 이번 증분은 §3(온디스크 포맷)만 완전히 구현하고, §4의 VFS
// 통합 계층(FileSystemDriver든 KernelFsDriver든)은 만들지 않는다 -
// 대신 어느 쪽으로 결정되든 그대로 얹을 수 있는 무상태(stateless)
// 메서드 집합(mount 이후는 전부 inode 번호로 직접 오퍼레이션)만
// 제공한다. 쓰기 경로(mkdir/rmdir/unlink/write)도 이번 증분에
// 포함하지 않는다 - SP-7A9CED3E §5가 이미 "구현 세션이 스펙과
// 대조해 확정"으로 열어 둔 익스텐트 트리 분할 알고리즘 등 위험도
// 높은 미결 사항이 남아 있어, 검증 가능한 읽기 경로부터 확정하는
// 편이 libswapfs와 같은 "작은 단위로 쪼개 순서대로 완성" 원칙에
// 맞는다.
// ---------------------------------------------------------------------
class Ext4Volume {
public:
    // 오프셋 1024의 슈퍼블록을 읽어 매직/필수 incompat 기능을 확인하고
    // 그룹 디스크립터 테이블을 전부 메모리에 캐싱한다(그룹 수가 현실적
    // 볼륨 크기에서 항상 작다는 libswapfs의 badPages와 같은 전제).
    bool mount(fs::BlockDevice* device);

    // "/a/b/c" 형태의 절대 경로를 루트(inode 2)부터 세그먼트별로
    // 탐색한다. 성공 시 outInode/outIsDir을 채운다.
    bool resolvePath(const char* path, uint32_t pathLen, uint32_t* outInode, bool* outIsDir);

    // inode 하나의 크기/디렉터리 여부를 읽는다(디스크에서 inode를
    // 다시 읽음 - v1은 캐싱하지 않는다).
    bool statInode(uint32_t inodeNum, uint64_t* outSize, bool* outIsDir);

    // 논리 오프셋 기준 읽기(POSIX pread 스타일, 상태 없음) - 파일
    // 끝을 넘는 길이는 파일 끝까지만 읽고 실제 읽은 바이트 수를
    // 반환한다. 실패(예: 손상된 익스텐트 트리)는 UINT32_MAX 아님,
    // outOk로 구분.
    uint32_t readInode(uint32_t inodeNum, uint64_t offset, void* buf, uint32_t len, bool* outOk);

    // 디렉터리 inode의 0-based 인덱스 순회 - readdir()의 커서는
    // 호출부(향후 KernelFsDriver의 FileHandle)가 관리, 이 클래스는
    // 매 호출마다 처음부터 다시 스캔한다(디렉터리 크기가 작다는
    // 전제 - htree 인덱스를 무시하고 선형 스캔해도 올바른 전체
    // 목록을 얻는다는 htree 자체의 하위호환 보장, SP-7A9CED3E §2.1).
    bool readdirAt(uint32_t dirInodeNum, uint64_t index, char* nameOut, uint32_t nameOutCap,
                   uint32_t* outNameLen, bool* outIsDir, uint32_t* outEntryInode);

private:
    bool readInodeStruct(uint32_t inodeNum, InodeCore* out);
    // 논리 블록 번호 -> 물리 블록 번호. inode.block[60]을 ExtentHeader로
    // 해석해 depth==0(리프)/depth>0(내부 노드, 재귀) 양쪽을 처리한다.
    bool resolveExtent(const InodeCore& inode, uint32_t logicalBlock, uint64_t* outPhysicalBlock);
    bool resolveExtentNode(const uint8_t* nodeBytes, uint32_t logicalBlock, uint32_t depthLimit,
                           uint64_t* outPhysicalBlock);
    // dirInodeNum의 데이터 블록들을 스캔해 name과 일치하는 엔트리를 찾는다.
    bool findDirEntry(uint32_t dirInodeNum, const char* name, uint32_t nameLen,
                       uint32_t* outInode, uint8_t* outFileType);

    fs::BlockDevice* device_ = nullptr;
    SuperblockCore sb_{};
    uint32_t blockSize_ = 0;
    uint32_t groupCount_ = 0;
    GroupDesc32* groupDescs_ = nullptr;
};

constexpr uint32_t kRootInodeNumber = 2;

}  // namespace ext4

#endif  // MINICORE_LIBEXT4_EXT4_H
