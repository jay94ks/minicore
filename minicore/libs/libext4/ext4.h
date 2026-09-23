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

// RO_COMPAT_METADATA_CSUM(0x400) - PN-1750A32F(SP-7A9CED3E §2.2 항목3)가
// 다루는 대상. v1 마운트 허용 여부와는 무관(read-only compat 비트라
// 몰라도 마운트 자체는 안전) - kExt4ComputeGroupDescChecksum()을 실제로
// 쓸지 판단하는 호출자 쪽 조건으로만 쓰인다.
constexpr uint32_t kRoCompatMetadataCsum = 0x400;

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
// [증분, PN-1750A32F] 위 12바이트를 실제 이름 있는 필드로 쪼갰다 -
// checksum(bg_checksum)을 kExt4ComputeGroupDescChecksum()으로 계산/
// 검증(읽기)/갱신(쓰기 시 대입)하려면 그 필드의 정확한 오프셋(30)이
// 필요하기 때문. 나머지 4개 필드는 v1이 여전히 안 읽지만, 체크섬
// 계산 시 "필드 자체를 0으로 채운 상태로 해시"하는 범위에 포함되므로
// 이름을 붙여 두는 편이 코드를 읽을 때 명확하다.
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
    uint32_t excludeBitmapLo;
    uint16_t blockBitmapCsumLo;
    uint16_t inodeBitmapCsumLo;
    uint16_t itableUnusedLo;
    uint16_t checksum;  // bg_checksum(crc16 또는 crc32c&0xFFFF, feature에 따라 갈림)
};
static_assert(sizeof(GroupDesc32) == 32, "GroupDesc32는 정확히 32바이트여야 함");
#pragma pack(pop)

// crc32c(Castagnoli, 다항식 0x82F63B78 reflected) 순수 계산 함수 - ext4
// metadata_csum 계열(그룹 디스크립터/비트맵/inode 체크섬)이 공유하는
// 원시 연속(raw continuation) 형태다. 리눅스 커널 crc32c()/e2fsprogs와
// 동일하게 함수 자체는 초기/최종 보수(~0) 처리를 하지 않는다 - 호출자가
// 첫 seed로 0xFFFFFFFF를 넘기고, ext4 체크섬 관례상 최종 값도 보수 없이
// 그대로 쓴다(표준 단독 CRC32C 체크섬과 다른 점 - PN-1750A32F 조사로
// 실제 mkfs.ext4 -O metadata_csum 이미지 3개 그룹 전부와 대조 확인).
uint32_t kCrc32c(uint32_t seed, const void* data, uint32_t len);

// ext4 그룹 디스크립터 체크섬(bg_checksum, RO_COMPAT_METADATA_CSUM 방식)
// 계산 - 읽은 값과 비교하면 검증, desc.checksum에 대입하면 갱신(쓰기
// 전 준비)에 그대로 쓸 수 있는 순수 함수(디스크/마운트 상태에 손대지
// 않음). desc는 checksum 필드가 어떤 값이든 상관없이(내부에서 0으로
// 간주하고 계산) 안전하게 넘길 수 있다. uuid는 SuperblockCore::uuid
// (16바이트) 그대로.
uint16_t kExt4ComputeGroupDescChecksum(const uint8_t uuid[16], uint32_t groupNum, const GroupDesc32& desc);

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
    uint32_t flags;      // kExtentsFl 유무로 block[60]의 해석이 갈림(PN-E3629BE9로 둘 다 지원)
    uint32_t osd1;
    uint8_t  block[60];  // kExtentsFl 켜짐: ExtentHeader+엔트리 인라인. 꺼짐: uint32_t[15] 레거시 간접 블록 포인터(direct 12 + single/double/triple)
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
// 4. Ext4Volume - 마운트 + mount()가 캐싱한 슈퍼블록/그룹 디스크립터
// 상태의 읽기 전용 노출.
//
// [범위, 2026-09-22, PN-22784AD4 -> PN-9AE5BFE4로 갱신] 이 클래스는
// 원래(PN-22784AD4) resolvePath/readInode/statInode/readdirAt(전부
// 동기, `fs::BlockDevice::readBlocks()` 사용)까지 제공했었다 - 그
// 무렵엔 §4의 VFS 통합 계층(`FileSystemDriver`/`KernelFsDriver`)이
// 아직 뭘로 정해질지 몰라(`QU-08ACD701`) 어느 쪽이 오든 얹을 수
// 있는 무상태 API로 설계했던 것.
//
// `PN-9AE5BFE4`(Ext4Driver 구현) 단계에서 실측으로 확인된 사실 -
// `KernelFsDriver::onExec()`은 코루틴인데, 그 안에서 위 동기 메서드
// (`readBlocks()`의 Task 레벨 블로킹에 의존)를 부르면 무한 대기한다
// (`QU-FF7044DA`, `PN-6EDED542`로 해소된 코루틴 I/O 대기 메커니즘
// 문제). `PN-6EDED542`가 도입한 `kernel::AsyncTaskCoroAwaiter`
// (`co_await` 프로토콜)로 바꿔도, 그 클래스가 `AsyncTask::current()`
// (항상 최상위 - onExec 자신)를 기준으로 재개 대상을 고르기 때문에,
// 이 메서드들을 **별도 코루틴 함수**로 감싸 onExec이 다시 그걸
// `co_await`하는 합성(nested coroutine composition)은 안전하게
// 재개되지 않는다(중간 코루틴 프레임의 존재를 `AsyncTaskCoroAwaiter`
// 가 전혀 모름 - 실측 확인, 공유 타입 `kernel::AsyncExecCoro`를
// 고치지 않는 한 원천적으로 안 됨, 그 타입은 커널 전체 공유라
// 이 세션이 임의로 고치지 않는다).
//
// 그 결과 `Ext4Driver::onExec()`(ext4_driver.cpp)은 이 메서드들을
// 재사용하지 않고, I/O 지점마다 `co_await
// kernel::AsyncTaskCoroAwaiter(...)`를 onExec 자신의 몸체 안에 직접
// 박아 넣는 평탄화된 버전으로 별도 구현했다 - 그래서 옛
// resolvePath/readInode/statInode/readdirAt(과 그 private 헬퍼
// readInodeStruct/resolveExtent/resolveExtentNode/findDirEntry)는
// **더 이상 어디서도 호출되지 않는 죽은 코드**가 돼 제거했다
// (호출부 감사로 확인, RM-23F4B687 §4 "확실히 안 쓰면 완전히
// 삭제한다"). `mount()`만 여전히 실제로 쓰인다 - `Ext4Driver::
// mount()`(진짜 kernel::Task 컨텍스트에서 한 번 호출되는 준비
// 단계, `SP-2BCE5D60` §3.1)가 그대로 호출한다.
// ---------------------------------------------------------------------
class Ext4Volume {
public:
    // 오프셋 1024의 슈퍼블록을 읽어 매직/필수 incompat 기능을 확인하고
    // 그룹 디스크립터 테이블을 전부 메모리에 캐싱한다(그룹 수가 현실적
    // 볼륨 크기에서 항상 작다는 libswapfs의 badPages와 같은 전제).
    bool mount(fs::BlockDevice* device);

    // [PN-9AE5BFE4] mount()가 이미 파싱/캐싱해 둔 상태를 읽기 전용으로
    // 노출 - `Ext4Driver::onExec()`(코루틴 컨텍스트, 위 문서 주석
    // 참고)가 슈퍼블록을 다시 읽어 재파싱하는 대신 이 상태를 그대로
    // 재사용해 자신만의 평탄화된 순회 로직을 구현하는 데 쓴다.
    fs::BlockDevice* device() const { return device_; }
    const SuperblockCore& superblockInfo() const { return sb_; }
    uint32_t blockSizeValue() const { return blockSize_; }
    uint32_t groupCountValue() const { return groupCount_; }
    const GroupDesc32* groupDescsPtr() const { return groupDescs_; }

private:
    fs::BlockDevice* device_ = nullptr;
    SuperblockCore sb_{};
    uint32_t blockSize_ = 0;
    uint32_t groupCount_ = 0;
    GroupDesc32* groupDescs_ = nullptr;
};

constexpr uint32_t kRootInodeNumber = 2;

// ---------------------------------------------------------------------
// 3.6 jbd2 저널 온디스크 포맷 (PN-BC3A2F5F 준비 작업) - 리눅스 커널
// fs/jbd2/journal.h와 대조 확정, 실제 mke2fs -t ext4 기본 이미지의
// 저널 inode(보통 8번, extents 기반)로 오프셋까지 실측 확인
// (dumpe2fs -h/debugfs 'stat <8>'으로 첫 익스텐트 physical block을
// 찾아 dd로 그 블록을 직접 dump, 첫 4바이트가 `c0 3b 39 98`임을
// 확인). **주의: ext4 자신의 온디스크 구조(위 SuperblockCore 등)와
// 달리 jbd2 필드는 전부 빅엔디안이다** - 이 코드베이스의 다른
// 온디스크 포맷(ext4/vfat/exfat/ntfs)은 전부 리틀엔디안 그대로
// 읽으므로 바이트 스왑 헬퍼가 아직 없었다, 여기서 `kJbd2Be32`로
// 새로 추가한다(x86_64는 항상 리틀엔디안이라 무조건 스왑).
// **리플레이 로직 자체는 이번 증분에 없다** - 이 절은 순수 파싱
// 함수뿐이다(PN-5481287C의 FAT12 pack/unpack 준비 작업과 동일한
// "구현 전 순수 계산 함수부터" 패턴) - 실제 디스크립터/커밋/리보크
// 블록 파싱과 리플레이는 PN-BC3A2F5F 후속 작업.
// ---------------------------------------------------------------------
inline uint32_t kJbd2Be32(uint32_t v) { return __builtin_bswap32(v); }

#pragma pack(push, 1)
struct JournalHeader {
    uint32_t magic;      // kJbd2Magic - 빅엔디안 raw, kJbd2Be32로 변환 후 비교
    uint32_t blockType;  // kJbd2BlockType* - 빅엔디안 raw
    uint32_t sequence;   // 빅엔디안 raw
};
static_assert(sizeof(JournalHeader) == 12, "JournalHeader 레이아웃이 리눅스 소스와 어긋남");

// v1/v2 공용 슈퍼블록(저널 자신의 논리 블록 0, JournalHeader로
// 시작) - v1은 featureCompat 이후가 정의되지 않지만 오프셋은 동일.
struct JournalSuperblockV2 {
    JournalHeader header;      // 0
    uint32_t blockSize;        // 12 - 저널 자신의 블록 크기(바이트)
    uint32_t maxLen;           // 16 - 저널 전체 블록 수
    uint32_t first;            // 20 - 로그 정보의 첫 블록(보통 1)
    uint32_t sequence;         // 24 - 로그에서 기대하는 첫 커밋 ID
    uint32_t start;            // 28 - 로그 시작 블록 번호(0 = 저널이 비어있음/클린)
    uint32_t errno_;           // 32
    uint32_t featureCompat;    // 36 - v1엔 없는 필드(정의 안 됨)
    uint32_t featureIncompat;  // 40
    uint32_t featureRoCompat;  // 44
    uint8_t uuid[16];          // 48
    uint32_t nrUsers;          // 64
    uint32_t dynSuper;         // 68
    uint32_t maxTransaction;   // 72
    uint32_t maxTransData;     // 76
    uint8_t checksumType;      // 80
    uint8_t padding2[3];       // 81
    uint32_t numFcBlks;        // 84
    uint32_t head;             // 88
    // 이후 padding[40]+checksum+users[16*48] - 아직 안 씀(위
    // SuperblockCore와 같은 절단 관례, PN-BC3A2F5F가 필요해지면 추가).
};
static_assert(sizeof(JournalSuperblockV2) == 92, "JournalSuperblockV2 레이아웃이 리눅스 소스와 어긋남");
#pragma pack(pop)

constexpr uint32_t kJbd2Magic = 0xc03b3998;
constexpr uint32_t kJbd2BlockTypeDescriptor = 1;
constexpr uint32_t kJbd2BlockTypeCommit = 2;
constexpr uint32_t kJbd2BlockTypeSuperblockV1 = 3;
constexpr uint32_t kJbd2BlockTypeSuperblockV2 = 4;
constexpr uint32_t kJbd2BlockTypeRevoke = 5;

// 호출자가 이미 읽어 온 저널 논리 블록 0의 원시 바이트가 유효한
// jbd2 v1/v2 슈퍼블록인지 확인하고 호스트(리틀엔디안) 값으로 변환해
// *out에 채운다. false면 매직 불일치(저널 블록이 아니거나 손상) -
// 순수 함수, 어떤 I/O도 하지 않는다.
bool kJbd2ParseSuperblock(const void* rawBlock, uint32_t blockLen, JournalSuperblockV2* out);

// [추가, 2026-09-23, PN-BC3A2F5F 준비 작업 2단계] 커밋 블록(고정
// 60바이트) - 실제 파일 하나를 쓰고 sync한 뒤 언마운트해 만든 진짜
// 저널 트랜잭션(디스크립터+커밋 블록 쌍)으로 오프셋까지 실측 확인
// (`h_commit_sec`가 dd 실행 당시 실제 날짜의 유닉스 타임스탬프로,
// `h_commit_nsec`가 10억 미만의 유효한 나노초 값으로 정확히 나옴).
// `h_chksum_type`/`h_chksum_size`가 0이어도(이 이미지는 저널
// 체크섬 기능 자체가 꺼져 있음, `journal features: (none)`)
// `h_chksum[0]`엔 여전히 값이 들어 있었다 - jbd2가 기능 비트와
// 무관하게 항상 레거시 crc32 하나는 써 둔다는 뜻으로 보이나, 이
// 필드의 정확한 의미/검증 방법은 착수 세션이 실제 리플레이 시
// 재확인할 것(현재는 불투명 배열로만 다룬다).
#pragma pack(push, 1)
struct CommitHeader {
    JournalHeader header;    // 0
    uint8_t chksumType;      // 12
    uint8_t chksumSize;      // 13
    uint8_t padding[2];      // 14
    uint32_t chksum[8];      // 16 - 불투명(위 주석 참고), 32바이트
    uint64_t commitSec;      // 48
    uint32_t commitNsec;     // 56
};
static_assert(sizeof(CommitHeader) == 60, "CommitHeader 레이아웃이 리눅스 소스와 어긋남");
#pragma pack(pop)

// [추가, 2026-09-23, PN-BC3A2F5F 준비 작업 2단계] 리보크 블록 헤더
// (JournalHeader + r_count) - **아직 실제 리보크 블록으로 실측
// 확인하지 못했다**(리보크는 같은 저널 에폭 안에서 최근 쓰인
// 블록을 재사용/삭제할 때만 생기는데, 이번 검증 시나리오(파일 1개
// 생성+sync)는 그 조건을 만들지 않았음) - JournalHeader 자체는
// 슈퍼블록/디스크립터/커밋 세 곳에서 이미 실측 검증됐으므로 오프셋
// 신뢰도는 높지만, 착수 세션이 실제 리보크 블록으로 한 번 더
// 재확인할 것.
#pragma pack(push, 1)
struct RevokeHeader {
    JournalHeader header;  // 0
    uint32_t count;        // 12 - 이 블록에서 실제 쓰인 바이트 수(헤더 포함)
};
static_assert(sizeof(RevokeHeader) == 16, "RevokeHeader 레이아웃이 리눅스 소스와 어긋남");
#pragma pack(pop)

// 커밋 블록 하나를 파싱 - kJbd2ParseSuperblock과 동일한 관례(매직/
// 블록타입 확인 후 호스트 엔디안으로 변환).
bool kJbd2ParseCommitHeader(const void* rawBlock, uint32_t blockLen, CommitHeader* out);

// [추가, 2026-09-23, PN-BC3A2F5F 준비 작업 3단계] 디스크립터 블록의
// 가변 길이 태그 - v1(8바이트, blocknr+flags)과 CSUM_V3(16바이트,
// blocknr+flags+blocknrHigh+checksum) 둘 다 실측 확인 완료. 같은
// mke2fs 기본 옵션이라도 e2fsprogs 버전/환경에 따라 저널
// featureIncompat가 다르게 나온다는 걸 이번에 직접 확인했다 - 이전
// 준비 작업(2단계)이 "journal features: (none)"이라고 적어 둔 환경과
// 달리, 이번 세션의 WSL e2fsprogs는 기본으로 journal_64bit+
// journal_checksum_v3(featureIncompat=0x12)를 켠다. 그래서 두 이미지를
// 각각 만들어(`mke2fs` 기본값 vs `-O ^64bit,^metadata_csum`) 양쪽 다
// 실제 파일 쓰기+sync로 진짜 트랜잭션을 만들고 디스크립터 블록을
// 직접 스캔 - 태그 개수/블록 번호(슈퍼블록=0, GDT=1 등 실제 메타데이터
// 블록과 일치)/LAST_TAG 종료 위치까지 두 포맷 모두 확인됨.
constexpr uint32_t kJbd2FeatureIncompat64Bit = 0x00000002;
constexpr uint32_t kJbd2FeatureIncompatCsumV2 = 0x00000008;
constexpr uint32_t kJbd2FeatureIncompatCsumV3 = 0x00000010;

constexpr uint32_t kJbd2TagFlagEscape = 1;    // 이 블록의 실제 첫 4바이트가 우연히 jbd2 매직과 같아 0으로 치환됐었다는 표시(리플레이 시 원복 필요)
constexpr uint32_t kJbd2TagFlagSameUuid = 2;  // 세팅되면 이 태그 뒤에 16바이트 UUID가 없음(직전 UUID 재사용)
constexpr uint32_t kJbd2TagFlagDeleted = 4;
constexpr uint32_t kJbd2TagFlagLastTag = 8;   // 이 태그가 디스크립터 블록의 마지막 태그

// 정규화된 태그 하나 - v1(8바이트)/v3(16바이트) 두 온디스크 포맷
// 중 무엇으로 읽었는지와 무관하게 호출자에게는 이 형태로 준다.
struct DescriptorTag {
    uint64_t blockNr;  // 대상 파일시스템 블록 번호(v1은 항상 32비트 범위, v3는 blocknrHigh 결합)
    uint32_t flags;    // kJbd2TagFlag* 비트마스크(호스트 엔디안)
};

// 디스크립터 블록(JournalHeader로 시작, blockType==
// kJbd2BlockTypeDescriptor) 하나를 훑어 태그를 outTags[0..반환값)에
// 채운다. featureIncompat은 그 저널의 JournalSuperblockV2::
// featureIncompat을 그대로 넘긴다 - CSUM_V3 비트가 켜져 있으면
// 16바이트 태그, 아무 관련 비트도 없으면 8바이트 v1 태그로 해석한다
// (SAME_UUID가 없는 태그 뒤엔 16바이트 UUID가 따라오므로 함께
// 건너뜀). **CSUM_V2 단독 또는 64BIT 단독처럼 실제 이미지로 실측하지
// 못한 조합은 추측하지 않고 0(실패)을 반환**한다(정직한 실패 -
// RM-23F4B687 §4 취지). LAST_TAG를 만나거나 outTags 용량(maxTags)이
// 차거나 블록 끝에 닿으면 멈춘다. 순수 함수 - 어떤 I/O도, 파일시스템
// 상태 변경도 하지 않는다(태그 목록만 뽑아낼 뿐 리플레이가 아니다).
uint32_t kJbd2ParseDescriptorTags(const void* rawBlock, uint32_t blockLen, uint32_t featureIncompat,
                                   DescriptorTag* outTags, uint32_t maxTags);

}  // namespace ext4

#endif  // MINICORE_LIBEXT4_EXT4_H
