#ifndef MINICORE_LIBEXT4_EXT4_H
#define MINICORE_LIBEXT4_EXT4_H

#include "libkenv/mem.h"
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
    // [추가, PN-59C253E9] 오프셋 204~336(132바이트) - preallocBlocks/
    // preallocDirBlocks/reservedGdtBlocks/journalUuid/journalInum/
    // journalDev/lastOrphan/hashSeed/defHashVersion/jnlBackupType/
    // descSize/defaultMountOpts/firstMetaBg/mkfsTime/jnlBlocks[17] -
    // v1이 개별 필드로 안 읽으므로(GroupDesc32의 옛 reserved 묶음과
    // 같은 관례) 이름 없는 바이트 배열로 통째로 건너뛴다. 아래
    // *Hi 3종 필드의 정확한 오프셋(336/340/344)만 실제 mke2fs -O
    // 64bit,metadata_csum 이미지로 대조 확정(PN-59C253E9) - journalInum
    // (오프셋224, 실제 저널 inode 번호 8과 일치)/descSize(오프셋254,
    // 실제 64와 일치)/mkfsTime(오프셋264, 실제 생성 시각과 초 단위까지
    // 일치)/checksumType/kbytesWritten 등 여러 앵커 필드로 이 132바이트
    // 갭의 시작/끝 오프셋 자체도 함께 실측 검증했다.
    uint8_t reservedJournalAndHashFields[132];
    uint32_t blocksCountHi;      // 336 - INCOMPAT_64BIT일 때만 유효
    uint32_t rBlocksCountHi;     // 340
    uint32_t freeBlocksCountHi;  // 344
    // [추가, PN-D168A778] 오프셋 348~576(228바이트) - minExtraIsize부터
    // s_last_error_func까지, v1이 개별 필드로 안 읽어 위와 같은 관례로
    // 통째로 건너뛴다. 로컬 리눅스 커널 소스(fs/ext4/ext4.h struct
    // ext4_super_block)를 실제로 컴파일해 offsetof()로 뽑은 값과 실제
    // mke2fs -O quota,metadata_csum 이미지의 uid/gid quota inode 번호
    // (dumpe2fs "User/Group quota inode: 3/4") 둘 다로 아래 usrQuotaInum/
    // grpQuotaInum/prjQuotaInum 세 필드의 정확한 오프셋(576/580/620)을
    // 교차 검증했다(PN-D168A778) - 커널 헤더가 로컬에 있다고 그대로
    // 믿지 않고 실제 이미지로 다시 확인한다는 CLAUDE.md 규칙4 취지
    // 그대로.
    uint8_t reservedErrorAndMountOptFields[228];
    uint32_t usrQuotaInum;  // 576 - RO_COMPAT_QUOTA일 때만 유효, 사용자 쿼터 파일의 inode 번호
    uint32_t grpQuotaInum;  // 580 - 그룹 쿼터 파일의 inode 번호
    uint8_t reservedOverheadAndEncryptFields[36];  // 584~620 - overheadClusters/backupBgs/encryptAlgos/encryptPwSalt/lpfIno, v1 미사용
    uint32_t prjQuotaInum;  // 620 - 프로젝트 쿼터 파일의 inode 번호(RO_COMPAT_PROJECT 별도 기능 - v1 미지원, 오프셋만 기록)
    // 이후(624~) 여전히 v1이 안 읽는 필드 - 위와 같은 절단 관례.
};
static_assert(sizeof(SuperblockCore) == 624, "SuperblockCore 레이아웃이 리눅스 소스와 어긋남");
#pragma pack(pop)

constexpr uint32_t kSuperblockOffset = 1024;
constexpr uint16_t kMagic = 0xEF53;
constexpr uint32_t kStateValidFs = 0x1;

// s_checksum 필드의 슈퍼블록 시작 기준 바이트 오프셋(1024바이트
// 슈퍼블록 자체의 마지막 4바이트) - `kExt4ComputeSuperblockChecksum()`
// 선언부 참고.
constexpr uint32_t kSuperblockChecksumOffset = 0x3FC;

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
// [추가, PN-59C253E9] `kIncompat64Bit`도 허용 목록에 추가 - `GroupDesc64`
// 레이아웃이 실측 검증됐다(위 참고).
// [갱신, 2026-09-25, PN-36747363] 처음엔(PN-59C253E9) 각 그룹
// 디스크립터의 hi 필드(block/inode 비트맵·테이블 상위 32비트)가 전부
// 0인 경우에만 통과시키고 하나라도 0이 아니면 마운트를 거부했었다 -
// "64bit 포맷을 안전하게 파싱"까지만 지원하는 v1 안전장치였다. 이제
// `Ext4Volume`이 그룹 디스크립터 테이블을 압축하지 않고 원본 stride
// 그대로 보관하며 `groupInodeTableBlock()`이 hi 필드까지 실제로
// 합성해 주므로, 이 안전장치는 더 이상 필요 없어 제거했다 - 4G 블록을
// 실제로 초과하는 대용량 볼륨도 이제 정확한 64비트 주소로 inode 테이블에
// 접근한다(§2.2 항목4 나머지 해소).
constexpr uint32_t kSupportedIncompatMask = kIncompatFiletype | kIncompatExtents | kIncompatFlexBg | kIncompat64Bit;

// [추가, PN-59C253E9] `*Lo`/`*Hi` 필드 쌍을 실제 64비트 값으로 합성 -
// 리눅스 커널 `ext4_blocks_count()`/`ext4_r_blocks_count()`/
// `ext4_free_blocks_count()`(fs/ext4/ext4.h `ext4_read_incompat_64bit_val`
// 매크로) 관례 그대로: `INCOMPAT_64BIT`가 꺼져 있으면 `hi`는 아예
// 온디스크에 유효한 값이 아니므로 무시하고 `lo`만 쓴다 - `hi`가
// 우연히 0이 아닌 쓰레기여도(v1이 아직 hi를 쓰지 않는 볼륨 생성
// 경로가 없어 실제로는 항상 0) 안전하게 무시한다.
constexpr uint64_t kExt4Combine64(uint32_t featureIncompat, uint32_t lo, uint32_t hi) {
    return (featureIncompat & kIncompat64Bit) ? ((static_cast<uint64_t>(hi) << 32) | lo)
                                               : static_cast<uint64_t>(lo);
}
// 컴파일 타임 자체 검증 - 리눅스 커널 매크로(fs/ext4/ext4.h
// `ext4_read_incompat_64bit_val`: `(64bit_set ? hi<<32 : 0) | lo`)와
// 대조한 대표 케이스들.
static_assert(kExt4Combine64(0, 0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFFu,
              "64BIT 꺼져 있으면 hi는 무시하고 lo만 써야 함");
static_assert(kExt4Combine64(kIncompat64Bit, 0x00000001u, 0x00000001u) == 0x100000001ULL,
              "64BIT 켜져 있으면 hi<<32|lo로 합성해야 함");
static_assert(kExt4Combine64(kIncompat64Bit, 0, 0) == 0, "둘 다 0이면 0");

// RO_COMPAT_METADATA_CSUM(0x400) - PN-1750A32F(SP-7A9CED3E §2.2 항목3)가
// 다루는 대상. v1 마운트 허용 여부와는 무관(read-only compat 비트라
// 몰라도 마운트 자체는 안전) - kExt4ComputeGroupDescChecksum()을 실제로
// 쓸지 판단하는 호출자 쪽 조건으로만 쓰인다.
constexpr uint32_t kRoCompatMetadataCsum = 0x400;

// RO_COMPAT_QUOTA(0x100) - PN-D168A778(SP-7A9CED3E §2.2 항목5)가
// 다루는 대상. 켜져 있으면 `SuperblockCore::usrQuotaInum`/
// `grpQuotaInum`(624바이트 확장 뒤에 나온 세 필드 중 두 개)이
// 가리키는 일반 파일 inode 안에 "quota v2"(vfsv0/vfsv1 - 리눅스
// VFS 공용 포맷, ext4 전용 아님) 형식의 쿼터 데이터가 들어 있다 -
// `QuotaV2Header`/`QuotaV2Info` 참고.
constexpr uint32_t kRoCompatQuota = 0x100;

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

// ---------------------------------------------------------------------
// [추가, PN-59C253E9] INCOMPAT_64BIT 그룹 디스크립터(64바이트) -
// 리눅스 커널 소스(fs/ext4/ext4.h `struct ext4_group_desc`)와 1바이트
// 단위로 대조 확정(PN-22784AD4/RM-23F4B687 §1과 동일한 절차). 앞
// 32바이트는 `GroupDesc32`와 완전히 동일한 레이아웃(오프셋까지)이고,
// 오프셋 32부터 각 `*Lo` 필드의 `*Hi` 대응 필드가 이어진다.
// **`checksum`(bg_checksum, 오프셋 30)은 64바이트 디스크립터에서도
// 여전히 16비트 그대로다 - hi 확장 필드가 없다**(실측 확인). 반면
// `blockBitmapCsumHi`/`inodeBitmapCsumHi`(오프셋 56/58)는 실제로
// 존재하며, `kExt4ComputeBitmapChecksum()`이 돌려주는 32비트 crc32c
// 전체 값 중 상위 16비트를 그대로 담는다(하위 16비트만 담는
// `GroupDesc32`와 다른 점 - 실제 mke2fs -O 64bit,metadata_csum
// 이미지로 대조 확인).
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct GroupDesc64 {
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
    uint16_t checksum;  // bg_checksum - 64바이트 디스크립터에서도 16비트 그대로
    uint32_t blockBitmapHi;
    uint32_t inodeBitmapHi;
    uint32_t inodeTableHi;
    uint16_t freeBlocksCountHi;
    uint16_t freeInodesCountHi;
    uint16_t usedDirsCountHi;
    uint16_t itableUnusedHi;
    uint32_t excludeBitmapHi;
    uint16_t blockBitmapCsumHi;
    uint16_t inodeBitmapCsumHi;
    uint32_t reserved;  // v1 미사용
};
static_assert(sizeof(GroupDesc64) == 64, "GroupDesc64는 정확히 64바이트여야 함");
#pragma pack(pop)

// `GroupDesc64`(64바이트, INCOMPAT_64BIT) 버전의 그룹 디스크립터
// 체크섬 계산 - `kExt4ComputeGroupDescChecksum()`(32바이트 버전)과
// seed/이어붙임 순서는 동일하나, 체크섬 필드(오프셋30) 이후 나머지
// 34바이트(오프셋32~64, hi 필드들)까지 마저 이어붙이는 "tail 해시"
// 단계가 실제로 필요해진다(32바이트 버전은 체크섬 필드가 곧 구조체
// 끝이라 이 단계가 없었음 - 코드 주석 참고). 실제 mke2fs -O
// 64bit,metadata_csum 이미지로 대조 확인(PN-59C253E9).
uint16_t kExt4ComputeGroupDesc64Checksum(const uint8_t uuid[16], uint32_t groupNum, const GroupDesc64& desc);

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

// 블록/inode 비트맵 체크섬(bg_block_bitmap_csum_lo/bg_inode_bitmap_csum_lo,
// RO_COMPAT_METADATA_CSUM 방식) 계산 - 그룹 디스크립터 체크섬과 달리
// 그룹 번호를 이어붙이지 않는다(uuid 시드 다음 바로 비트맵 바이트).
// [PN-625E2804 실측 확인] 해시 범위는 항상 "이 볼륨의 명목상"
// bitCount(=`SuperblockCore::blocksPerGroup` 또는 `inodesPerGroup`,
// 특정 그룹의 실제 유효 비트 수가 아님)를 8로 나눠 올림한 바이트 수
// 고정이다 - 마지막 그룹이 blocksPerGroup보다 적은 블록만 가져도
// (실제 mkfs.ext4 이미지로 대조 확인) 여전히 같은 길이를 해시한다.
// bitCount에는 항상 `blocksPerGroup`/`inodesPerGroup`을 그대로
// 넘길 것 - 그룹별로 다른 값을 계산해 넘기면 틀린다.
// [PN-59C253E9 실측 확인] 반환값은 crc32c 전체 32비트 그대로 - 32바이트
// (INCOMPAT_64BIT 미지원) 디스크립터는 하위 16비트만 `*_csum_lo`에
// 저장하고 상위는 버리면 되지만, 64바이트 디스크립터는 실제
// mke2fs -O 64bit 이미지로 대조 확인한 결과 **상위 16비트도 버리지
// 않고 `*_csum_hi` 필드에 그대로 저장한다**(16비트 절반짜리 체크섬이
// 아니라 32비트 전체가 두 필드에 나뉘어 저장되는 것) - 32비트
// GroupDesc32에는 hi 필드 자체가 없으므로 자동으로 버려질 뿐, 함수
// 자체는 desc_size를 모르므로 항상 전체 32비트를 돌려준다.
uint32_t kExt4ComputeBitmapChecksum(const uint8_t uuid[16], const void* bitmapData, uint32_t bitCount);

// ---------------------------------------------------------------------
// [신규, 2026-09-25, PN-FE718C87] 그룹 하나 안에서 블록/inode 하나를
// 실제로 할당/해제 - 비트맵 비트 갱신 + 그룹 디스크립터의 free count
// 갱신 + (RO_COMPAT_METADATA_CSUM이면) 비트맵/그룹 디스크립터 체크섬
// 재계산까지 한 번에 처리하는 순수 함수(I/O 없음, 이미 읽어 온 버퍼를
// 제자리에서 갱신). `groupDescBuf`는 그 그룹 디스크립터 하나의 온디스크
// stride(32 또는 64바이트, `is64Bit`로 구분) 그대로, `bitmapBuf`는 그
// 그룹의 block/inode 비트맵 내용 그대로(둘 다 호출자가 이미 디스크에서
// 읽어 옴). 할당 실패(빈 비트 없음)/해제 실패(이미 free인 비트를
// 또 해제하려 함)면 아무것도 바꾸지 않고 false.
bool kExt4AllocateBlockInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t blocksPerGroup,
                                uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                                uint8_t* groupDescBuf, uint32_t* outRelIndex);
bool kExt4FreeBlockInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t blocksPerGroup,
                            uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                            uint8_t* groupDescBuf, uint32_t relIndex);
bool kExt4AllocateInodeInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t inodesPerGroup,
                                uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                                uint8_t* groupDescBuf, uint32_t* outRelIndex);
bool kExt4FreeInodeInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t inodesPerGroup,
                            uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                            uint8_t* groupDescBuf, uint32_t relIndex);

// [신규, 2026-09-25, PN-FE718C87 - PN-4C67E1ED 실측(e2fsck)으로 발견된
// 갭] bg_used_dirs_count(그 그룹 안의 "디렉터리" inode 개수 - 일반
// 파일은 세지 않음) 조정 - `kExt4AllocateInodeInGroup()`은 할당하는
// inode가 디렉터리인지 일반 파일인지 모르는 범용 할당자라 이 값을
// 대신 다뤄줄 수 없다(그래서 별도 함수). 호출자(Mkdir/Rmdir 구현부,
// ext4_driver.cpp)가 "지금 만들거나 지우는 게 디렉터리다"를 이미
// 알고 있을 때만 delta(+1/-1)를 넘겨 부른다. 결과가 음수가 되면
// (버그 방어) 아무것도 바꾸지 않고 false. **실측(PN-4C67E1ED)**: 이
// 갱신 없이 실제 Mkdir을 태우면 e2fsck가 "Directories count wrong
// for group #0"으로 정확히 잡아낸다(그 외 실제 데이터/체크섬/링크
// 카운트는 전부 정상이었음 - 이 카운터 하나만의 문제).
bool kExt4AdjustGroupDescUsedDirs(const uint8_t uuid[16], uint32_t groupNum, uint32_t featureRoCompat, bool is64Bit,
                                    uint8_t* groupDescBuf, kernel::int64_t delta);

// 슈퍼블록의 전역 free 블록/inode 수를 blocksDelta/inodesDelta만큼
// 조정(양수=증가/음수=감소)하고, RO_COMPAT_METADATA_CSUM이면 s_checksum
// 도 재계산한다 - `kExt4ComputeSuperblockChecksum()`과 동일하게
// `rawSuperblock1024Bytes`는 오프셋 1024부터의 실제 온디스크 1024바이트
// 전체(이 struct가 파싱하는 부분 절단본이 아님). 결과가 음수가 되면
// (버그 방어) 아무것도 바꾸지 않고 false.
bool kExt4AdjustSuperblockFreeBlocks(void* rawSuperblock1024Bytes, kernel::int64_t blocksDelta,
                                      uint32_t featureIncompat, uint32_t featureRoCompat);
bool kExt4AdjustSuperblockFreeInodes(void* rawSuperblock1024Bytes, kernel::int64_t inodesDelta,
                                      uint32_t featureRoCompat);

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

// ext4 inode 체크섬(i_checksum_lo/i_checksum_hi, RO_COMPAT_METADATA_CSUM
// 방식) 계산 - `InodeCore`가 파싱하는 132바이트가 아니라 **실제 온디스크
// inode 레코드 전체**(inodeSize바이트, `SuperblockCore::inodeSize` -
// 보통 256, 128을 넘는 crtime 등 v1이 안 읽는 확장 필드까지 해시 범위에
// 포함되기 때문)가 필요하다. rawInode는 그 inodeSize바이트를 그대로
// 가리키는 포인터, generation은 그 inode의 `InodeCore::generation`과
// 같은 값(오프셋 100). [PN-625E2804 실측 확인] 그룹 디스크립터/비트맵과
// 또 다른 세 번째 이어붙임 규칙 - per-inode seed가 전역 uuid seed에
// inode 번호(LE32)와 generation(LE32)을 순서대로 이어붙여 별도로
// 만들어진다(실제 mkfs.ext4 이미지의 root inode(2번)+debugfs로 만든
// 파일 inode 2개, 총 3개 inode 전부 실측 대조 완료). 반환값 하위
// 16비트가 i_checksum_lo, 상위 16비트가 i_checksum_hi(inodeSize<=128이면
// 그 필드 자체가 온디스크에 없으므로 호출자가 무시할 것).
uint32_t kExt4ComputeInodeChecksum(const uint8_t uuid[16], uint32_t inodeNum, uint32_t generation,
                                    const void* rawInode, uint32_t inodeSize);

// 슈퍼블록 자신의 체크섬(s_checksum, RO_COMPAT_METADATA_CSUM 방식)
// 계산 - `SuperblockCore`가 파싱하는 범위(현재 348바이트)가 아니라
// **실제 온디스크 슈퍼블록 전체 1024바이트**(오프셋 `kSuperblockOffset`
// 부터)가 필요
// 하다. [PN-625E2804 실측 확인] 그룹 디스크립터/비트맵/inode 세
// 체크섬과 또 다른(네 번째) 규칙 - uuid를 별도 seed로 쓰지 않고
// `seed=0xFFFFFFFF`에서 슈퍼블록 원본 바이트를 그대로 이어붙인다
// (그 안에 담긴 uuid 필드까지 자연히 함께 해시됨). 체크섬 필드
// (`kSuperblockChecksumOffset`~1024)가 곧 슈퍼블록의 끝이라 잘라서
// 넘기는 것만으로 자연히 제외되므로, 그룹 디스크립터/inode 체크섬과
// 달리 그 필드를 별도로 0으로 채워 이어붙이는 단계가 없다. 반환값은
// 16비트 절반이 아니라 32비트 값 그대로가 s_checksum(실제 mkfs.ext4
// 이미지 2개, 서로 다른 크기/볼륨 라벨로 대조 확인).
uint32_t kExt4ComputeSuperblockChecksum(const void* rawSuperblock1024Bytes);

constexpr uint32_t kExtentsFl = 0x80000;

// [신규, 2026-09-25, PN-9AA8B1EF] EXT4_INDEX_FL - 이 디렉터리가 htree
// (해시 인덱스) 구조를 쓰고 있다는 inode 플래그. 이 프로젝트는 htree
// 인덱스를 직접 만들거나 갱신하지 않는다(`SP-7A9CED3E` §5가 명시적으로
// "정확한 정책은 구현 세션이 확인"으로 미뤄 둔 지점).
//
// **[실측 반박, 2026-09-25] "플래그만 지우고 계속 진행"은 안전하지
// 않다** - 처음엔 이 플래그를 지워 "htree 인덱스 없는 평범한
// 디렉터리로 격하"시키면 되리라 가정했으나, 실제 리눅스 커널로
// 만든 진짜 htree 루트 블록(블록 0)으로 실측한 결과 `e2fsck -fn`이
// "directory corrupted"로 거부했다. 원인: htree 루트 블록의 ".."
// 엔트리는 recLen이 블록 끝(1024)까지 그대로 이어지고(그 안에
// dx_root_info/dx_entries가 숨어 있음) - metadata_csum 리프 블록의
// 표준 규약(마지막 12바이트는 항상 DirEntryTail을 위해 비워 둠)을
// 따르지 않는다. 이는 htree 루트만의 별도 체크섬 배치 규약
// (dx_entries 배열 끝에 붙는 `dx_tail`, 일반 디렉터리 블록의
// DirEntryTail과는 다른 구조)이기 때문 - 그래서 플래그만 지우고
// 내용을 그대로 두면, e2fsck가 "이제 이건 평범한 디렉터리 블록"
// 이라는 전제로 재검사하면서 "마지막 엔트리가 tail 자리를 안 남김"
// 을 손상으로 잡아낸다(이 프로젝트가 실제로 그 블록을 건드렸는지와
// 무관 - 그냥 재해석만으로도 발생).
//
// **v1 정책(확정)**: 이 플래그가 켜진 디렉터리는 새 엔트리를 넣지
// 않고 정직하게 `PermissionDenied`로 거부한다(레거시 간접 블록/
// depth>1 익스텐트 트리 초과와 동일한 "v1 미지원, 손대지 않음"
// 관례) - htree 구조를 전혀 건드리지 않으므로 실제 리눅스 커널/
// e2fsck 양쪽에서 계속 완전히 정상으로 읽힌다. 인덱스를 실제로
// 유지/갱신하는 전체 htree 쓰기 지원은 이 플래그 검사를 대체하는
// 훨씬 큰 후속 작업(진짜 해시 계산 + 리프 분할 + dx_node 갱신)이
// 필요하다 - PN-9AA8B1EF 계획 본문 참고.
constexpr uint32_t kIndexFl = 0x1000;
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

// [신규, 2026-09-25, PN-FE718C87] `InodeCore::block[60]`을 인라인
// 익스텐트 리프(depth=0)로 다루는 순수 함수 - 헤더(12바이트) 뒤에
// Extent 엔트리가 최대 (60-12)/12=4개 들어간다. 새로 할당된 inode는
// 항상 이 "빈 인라인 리프" 상태에서 시작하고, 파일이 4개보다 많은
// 익스텐트를 필요로 할 때만 비로소 실제 온디스크 트리 확장/분할
// (§5, 이 함수 범위 밖 - PN-FE718C87 3단계 이후)이 필요해진다 -
// 즉 "작은 파일"(익스텐트 4개 이하로 표현되는 한)은 이 두 함수만으로
// 충분하다. 실제 mke2fs+debugfs로 만든 파일(6바이트, 익스텐트 1개
// (0):1618)의 진짜 inode.block[60] 원본 바이트와 1바이트 단위로
// 대조 완료(PN-FE718C87 3단계 검증 기록 참고).
constexpr uint32_t kExtentInlineBytes = 60;  // sizeof(InodeCore::block)
constexpr uint16_t kExtentInlineMaxEntries = 4;  // (60-sizeof(ExtentHeader))/sizeof(Extent)

inline void kExt4InitInlineExtentLeaf(uint8_t block60[kExtentInlineBytes]) {
    ExtentHeader header{};
    header.magic = kExtentMagic;
    header.entries = 0;
    header.max = kExtentInlineMaxEntries;
    header.depth = 0;
    header.generation = 0;
    memcpy(block60, &header, sizeof(header));
    memset(block60 + sizeof(header), 0, kExtentInlineBytes - sizeof(header));
}

// logicalBlock부터 len개 연속 논리 블록을 physicalBlock부터 len개
// 연속 물리 블록에 매핑하는 익스텐트 하나를 리프 끝에 이어붙인다 -
// 이 v1은 "파일 끝에 이어 붙이는" 단순 append만 지원(logicalBlock이
// 기존 마지막 익스텐트 바로 다음이라는 정렬 불변조건은 호출자
// 책임 - 이 함수 자체는 검사하지 않는다). 이미 꽉 찼거나(entries==max)
// block60이 인라인 리프가 아니면(magic 불일치/depth!=0) 아무것도
// 바꾸지 않고 false.
inline bool kExt4AppendInlineExtent(uint8_t block60[kExtentInlineBytes], uint32_t logicalBlock,
                                     uint64_t physicalBlock, uint16_t len) {
    ExtentHeader header;
    memcpy(&header, block60, sizeof(header));
    if (header.magic != kExtentMagic || header.depth != 0 || header.entries >= header.max) {
        return false;
    }
    Extent ext{};
    ext.block = logicalBlock;
    ext.len = len;
    ext.startHi = static_cast<uint16_t>(physicalBlock >> 32);
    ext.startLo = static_cast<uint32_t>(physicalBlock & 0xFFFFFFFFu);
    memcpy(block60 + sizeof(ExtentHeader) + static_cast<uint32_t>(header.entries) * sizeof(Extent), &ext,
           sizeof(ext));
    ++header.entries;
    memcpy(block60, &header, sizeof(header));
    return true;
}

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

// [신규, 2026-09-25, PN-FE718C87] RO_COMPAT_METADATA_CSUM 디렉터리
// 리프 블록의 "가짜" 끝 엔트리 - 이 프로젝트가 새로 고안한 게 아니라
// 리눅스 커널 그대로(`struct ext4_dir_entry_tail`, fs/ext4/ext4.h,
// WSL2-Linux-Kernel 로컬 소스로 확인 - 자세한 경로는
// reference_local_linux_kernel_source 메모리 참고). `DirEntry2Header`
// 와 완전히 같은 8바이트 레이아웃(inode=0/recLen=12/nameLen=0/
// fileType=0xDE로 채워 "평범한 빈 엔트리"처럼 보이게 위장)에 4바이트
// 체크섬이 이어붙는다 - 그 4바이트는 이름이 없는(nameLen=0) 엔트리의
// "이름" 자리를 그대로 재사용하는 것. 마지막 실제 엔트리의 recLen이
// 이 12바이트를 정확히 남기도록 이미 계산돼 있어야 한다(호출자 책임).
constexpr uint8_t kFtDirCsum = 0xDE;

#pragma pack(push, 1)
struct DirEntryTail {
    DirEntry2Header header;  // inode=0, recLen=12, nameLen=0, fileType=kFtDirCsum
    uint32_t checksum;
};
static_assert(sizeof(DirEntryTail) == 12, "DirEntryTail은 12바이트여야 함");
#pragma pack(pop)

// 디렉터리 리프 블록의 체크섬(det_checksum) 계산 - 리눅스 커널
// fs/ext4/namei.c `ext4_dirblock_csum()`과 동일한 알고리즘(로컬 커널
// 소스로 확인 + 실제 mke2fs 이미지 루트 디렉터리 블록의 진짜
// det_checksum 값과 1바이트도 안 틀리게 대조 완료, PN-FE718C87) -
// per-inode seed(`kExt4ComputeInodeChecksum()`과 동일한 유도: uuid
// seed에 inode 번호+generation을 순서대로 이어붙임)로 블록 바이트 중
// [0, blockSize - sizeof(DirEntryTail))까지만 해시한다 - tail 엔트리
// 자신(12바이트, 체크섬 필드 포함)은 통째로 해시 범위 밖(체크섬
// 필드만 0으로 간주하는 다른 체크섬류와 다른 관례 - 커널 소스가
// 그렇게 계산함).
uint32_t kExt4ComputeDirBlockChecksum(const uint8_t uuid[16], uint32_t inodeNum, uint32_t generation,
                                       const void* dirBlockData, uint32_t blockSize);

// [신규, 2026-09-25, PN-FE718C87] 디렉터리 엔트리 하나가 필요로 하는
// 최소 온디스크 크기(4바이트 정렬) - 리눅스 커널 fs/ext4/ext4.h
// `ext4_dir_rec_len()`과 동일(이 프로젝트는 hash-in-dirent 확장
// 기능을 안 쓰므로 그 조건 분기는 없음, WSL2-Linux-Kernel 로컬 소스
// 확인).
inline uint32_t kExt4DirRecLen(uint8_t nameLen) {
    return (static_cast<uint32_t>(nameLen) + 8u + 3u) & ~3u;
}

// 디렉터리 리프 블록 하나(dirBlockData, blockSize바이트, 디스크에서
// 이미 읽어 온 것 - 제자리에서 갱신됨)에 새 엔트리를 실제로 삽입
// - 리눅스 커널 fs/ext4/namei.c `ext4_find_dest_de()`+
// `ext4_insert_dentry()`와 완전히 동일한 알고리즘(이 프로젝트가 새로
// 고안한 게 아니다, WSL2-Linux-Kernel 로컬 소스 확인 - 자세한 경로는
// reference_local_linux_kernel_source 메모리 참고). 기존 엔트리를
// 순서대로 훑으며 first-fit으로 빈 공간을 찾는다:
//   - 그 엔트리가 삭제됨(inode==0)이면 recLen 전체를 그대로 재사용
//     가능한 후보로 본다(분할 없음 - 통째로 덮어씀),
//   - 사용 중(inode!=0)이면 "그 엔트리 자신의 실제 필요 크기"를 뺀
//     나머지(slack)만 후보로 보고, 맞으면 그 슬랙만큼만 떼어 새
//     엔트리로 분할한다(기존 엔트리의 recLen은 자신의 최소 크기로
//     줄어듦).
// hasTailBytes(RO_COMPAT_METADATA_CSUM 볼륨의 디렉터리면
// sizeof(DirEntryTail)=12, 아니면 0)만큼은 애초에 탐색·분할 대상
// 범위에서 제외한다(그 자리는 항상 체크섬 엔트리 몫). 실제 mke2fs+
// debugfs로 재현한 두 시나리오(기존 엔트리 슬랙 분할/디렉터리 끝
// 확장) 모두와 결과 바이트가 1바이트도 안 틀리게 일치 확인
// (PN-FE718C87 검증 기록 참고). 자리가 전혀 없으면(모든 기존
// 엔트리가 빡빡함) 아무것도 바꾸지 않고 false.
bool kExt4InsertDirEntry(uint8_t* dirBlockData, uint32_t blockSize, uint32_t hasTailBytes, uint32_t inode,
                          const char* name, uint8_t nameLen, uint8_t fileType);

// [신규, 2026-09-25, PN-FE718C87 - Rmdir/Unlink] 디렉터리 블록에서
// name과 일치하는 엔트리를 찾아 inode=0으로 표시(지연 삭제 - 리눅스
// 커널과 동일한 관례). **이전 엔트리 recLen으로 흡수하는 백워드
// 병합은 v1 범위 밖**(`kExt4InsertDirEntry`가 이미 "삭제된(inode==0)
// 엔트리는 recLen 그대로 통째로 재사용"을 전제하므로, 병합 없이
// 지워도 다음 삽입이 그 자리를 그대로 되찾아 쓸 수 있다는 것이
// 구조적 전제다(실측 검증은 PN-FE718C87 Rmdir 배선의 실제 e2fsck
// 대조로 확인할 것). 찾아서 지웠으면 outFileType에 원래 fileType을
// 채우고 true, 못 찾았으면 아무것도 바꾸지 않고 false.
bool kExt4RemoveDirEntry(uint8_t* dirBlockData, uint32_t blockSize, const char* name, uint8_t nameLen,
                          uint8_t* outFileType);

// ---------------------------------------------------------------------
// [신규, 2026-09-25, PN-81C6322C] 인라인 리프(depth==0, inode->block의
// 60바이트)가 4개 엔트리로 꽉 찼을 때 실제 온디스크 익스텐트 트리를
// depth==1로 승격 - 리눅스 커널 fs/ext4/extents.c
// `ext4_ext_grow_indepth()`와 동일한 알고리즘(이 프로젝트가 새로
// 고안한 게 아니다, WSL2-Linux-Kernel 로컬 소스 확인 - 자세한 경로는
// reference_local_linux_kernel_source 메모리 참고). `kExt4InsertDirEntry`
// 부류와 달리 "5개 이상 익스텐트" 자체는 이 함수 범위가 아니고, 이
// 함수는 딱 한 번의 depth 0→1 전환만 담당한다(전환 후 새 리프
// 블록에 실제로 엔트리를 추가하는 건 호출자가 이어서
// `kExt4AppendInlineExtent`를 그 블록 버퍼에 대고 부르면 됨 - 그
// 함수는 버퍼 자신의 `header.max` 필드로 용량을 판단하므로 60바이트
// 인라인이든 전체 블록 리프든 그대로 재사용 가능, 별도 함수 불필요).
// ---------------------------------------------------------------------

// [신규, 2026-09-25, PN-81C6322C] metadata_csum 볼륨의 익스텐트
// 트리 블록(리프/인덱스 공용, 인라인 루트 제외) 끝에 붙는 체크섬
// 꼬리 - 리눅스 커널 `struct ext4_extent_tail`(4바이트, checksum
// 하나뿐)과 동일. 오프셋은 항상 "그 블록이 담을 수 있는 최대
// 엔트리 수(eh_max)만큼 다 찼다고 가정한 위치"(`sizeof(ExtentHeader)
// + eh_max*sizeof(Extent)`) - 실제 엔트리 개수(eh_entries)와 무관하게
// 고정이다(디렉터리 블록의 DirEntryTail이 recLen 체인 끝에 붙는 것과
// 달리, 이쪽은 항상 같은 자리).
#pragma pack(push, 1)
struct ExtentTail {
    uint32_t checksum;
};
static_assert(sizeof(ExtentTail) == 4, "ExtentTail은 4바이트여야 함");
#pragma pack(pop)

// 블록 하나(전체 blockSize)가 리프든 인덱스든 담을 수 있는 최대
// 엔트리 수 - Extent/ExtentIdx 둘 다 12바이트라 공식이 같다(리눅스
// 커널 `ext4_ext_space_block()`/`ext4_ext_space_block_idx()`가 실제로
// 동일한 공식을 씀, WSL2-Linux-Kernel 로컬 소스 확인). 체크섬 꼬리
// (4바이트)를 위한 별도 차감은 없다 - 블록 크기가 12로 나눠떨어지지
// 않는 나머지 공간(예: 1024바이트 블록이면 1024-12-84*12=4바이트)에
// 정확히 들어맞기 때문(리눅스 커널 주석이 이 성질을 명시적으로 전제).
inline uint32_t kExt4ExtentBlockMaxEntries(uint32_t blockSize) {
    return (blockSize - static_cast<uint32_t>(sizeof(ExtentHeader))) / static_cast<uint32_t>(sizeof(Extent));
}

// 익스텐트 트리 블록(리프/인덱스 공용) 체크섬 계산 - inode/디렉터리
// 블록 체크섬과 동일한 per-inode seed 유도(uuid seed에 inode 번호+
// generation을 순서대로 이어붙임, 리눅스 커널 `ext4_extent_block_csum()`
// 확인). 해시 범위는 [0, tailOffset)만 - tailOffset은 그 블록
// 버퍼 자신의 헤더에 이미 쓰여 있는 eh_max로 계산한다(호출 전에
// eh_max를 올바른 값으로 맞춰 둘 것 - `kExt4GrowExtentTreeToDepth1`이
// 이미 그렇게 한다).
uint32_t kExt4ComputeExtentBlockChecksum(const uint8_t uuid[16], uint32_t inodeNum, uint32_t generation,
                                          const void* extentBlockData, uint32_t blockSize);

// 인라인 리프(block60, depth==0 유효 상태여야 함)를 depth==1 인덱스
// 노드로 승격한다:
//   1) newBlockData(전체 blockSize바이트, 호출자가 이미 할당해 둔
//      newBlockAbs에 대응하는 버퍼) 시작 60바이트에 인라인 영역을
//      통째로 복사(헤더+최대 4개 엔트리 그대로), 나머지는 0으로.
//   2) 그 새 블록 헤더의 max를 블록 전체 용량(kExt4ExtentBlockMaxEntries)
//      으로 다시 씀(entries/depth/magic/generation은 복사된 값 그대로
//      - 보통 depth=0, entries=4).
//   3) 인라인 영역(block60) 자신을 인덱스 노드로 다시 씀 - 헤더
//      (entries=1, max=`kExtentInlineMaxEntries`, depth=1) + 인덱스
//      엔트리 1개(예전 첫 익스텐트의 ee_block을 그대로 ei_block으로,
//      newBlockAbs를 가리킴). **나머지 36바이트는 의도적으로 건드리지
//      않는다**(리눅스 커널도 그대로 - eh_entries=1이 그 바이트들을
//      무효로 만들 뿐 지우지 않음, 실측 검증 완료 시 기록).
// 체크섬(newBlockData의 tail)은 이 함수가 채우지 않는다 - eh_max를
// 고친 뒤에도 호출자가 새 데이터 엔트리를 마저 추가할 수 있으므로
// (호출자가 이어서 `kExt4AppendInlineExtent(newBlockData, ...)`를
// 부른 다음에야 최종 내용이 확정됨), 체크섬 계산은 그 다음 호출자
// 몫(`kExt4ComputeExtentBlockChecksum`). block60이 이미 유효한
// depth==0 리프가 아니면 아무것도 바꾸지 않고 false.
bool kExt4GrowExtentTreeToDepth1(uint8_t block60[kExtentInlineBytes], uint8_t* newBlockData, uint32_t blockSize,
                                  uint64_t newBlockAbs);

constexpr uint16_t kModeDir = 0x4000;      // S_IFDIR
constexpr uint16_t kModeRegular = 0x8000;  // S_IFREG - 파일 초기화 시 재사용할 수 있게 함께 정의
constexpr uint16_t kDefaultDirPerm = 0755;
constexpr uint16_t kDefaultFilePerm = 0644;

// [신규, 2026-09-25, PN-FE718C87] 새로 할당된(비어 있는) inode를
// "빈 디렉터리 하나, 데이터 블록 1개짜리" 상태로 채운다 - 실제
// mke2fs+debugfs가 만든 새 디렉터리 inode와 mode/linksCount/sizeLo/
// blocksLo/flags/times/block[60] 전부 1바이트도 안 틀리게 대조
// 완료(PN-FE718C87 검증 기록 참고). firstBlock은 이미 할당된
// (`kExt4AllocateBlockInGroup`) 이 디렉터리의 유일한 데이터 블록 -
// 이 함수가 그 위에 `kExt4InitInlineExtentLeaf`+
// `kExt4AppendInlineExtent`를 바로 적용한다(호출부가 따로 부를
// 필요 없음). epochSeconds는 호출자가 `kernel::Rtc::readWallClock()`
// +`Rtc::toEpochSeconds()`로 구해 넘긴다(이 라이브러리는
// `kernel::Rtc`를 모른다 - 계층 분리 유지). uid/gid는 호출자가
// 실제 호출 주체의 신원을 안다면 그 값을, 아직 모르면 0(root)을
// 넘긴다. **체크섬(i_checksum_lo/hi)은 여기서 채우지 않는다** -
// `kExt4ComputeInodeChecksum()`이 실제 온디스크 inodeSize 전체
// 원시 바이트를 대상으로 별도로 계산해야 하므로(이 함수는 132바이트
// `InodeCore`만 다룸), 그 계산과 osd2/checksumHi에 써넣는 건
// 호출자 몫.
void kExt4InitDirInode(InodeCore* inode, uint32_t blockSize, uint64_t firstBlock, uint32_t epochSeconds,
                        uint16_t uid, uint16_t gid);

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

    // [갱신, PN-36747363] `groupDescsPtr()`(GroupDesc32*로 압축된 뷰)를
    // 제거하고 이 접근자로 대체했다 - 압축은 hi 필드(4G 블록 초과 실제
    // 대용량 볼륨의 상위 32비트 주소)를 버리는 손실 변환이라 이 계획이
    // 요구하는 실제 지원과 근본적으로 상충된다. 그룹 group의 inode
    // 테이블 시작 블록의 진짜 64비트 절대 블록 번호를 돌려준다 -
    // `INCOMPAT_64BIT`이면 온디스크 `GroupDesc64::inodeTableHi`까지
    // `kExt4Combine64()`로 합성, 아니면 `GroupDesc32::inodeTableLo`
    // 그대로. `group >= groupCountValue()`거나 아직 mount()가 안
    // 됐으면 0을 돌려준다(호출자가 범위 검사를 이미 하는 관례 -
    // ext4_driver.cpp의 kLocateInode 참고).
    uint64_t groupInodeTableBlock(uint32_t group) const;

    // [신규, 2026-09-25, PN-FE718C87] groupInodeTableBlock()과 완전히
    // 같은 방식(hi 필드까지 kExt4Combine64()로 합성) - 그룹 group의
    // block/inode 비트맵 시작 블록의 진짜 64비트 절대 블록 번호.
    // 읽기 경로(1차 증분)는 이 두 비트맵을 전혀 안 썼으나(free 여부를
    // 신경 쓸 필요가 없었으므로), 쓰기 경로(블록/inode 할당)는
    // 이 비트맵을 실제로 스캔/갱신해야 한다.
    uint64_t groupBlockBitmapBlock(uint32_t group) const;
    uint64_t groupInodeBitmapBlock(uint32_t group) const;

private:
    fs::BlockDevice* device_ = nullptr;
    SuperblockCore sb_{};
    uint32_t blockSize_ = 0;
    uint32_t groupCount_ = 0;
    // [갱신, PN-36747363] 온디스크 그룹 디스크립터 테이블을 그대로
    // (압축하지 않고) 담는 원시 버퍼 - stride가 32(GroupDesc32) 또는
    // 64(GroupDesc64)바이트인지는 is64Bit_로만 구분한다.
    uint8_t* groupDescsRaw_ = nullptr;
    uint32_t groupDescStride_ = 0;
    bool is64Bit_ = false;
};

constexpr uint32_t kRootInodeNumber = 2;

// ---------------------------------------------------------------------
// [신규, 2026-09-25, PN-FE718C87] 블록/inode 비트맵 - 이 프로젝트가
// 새로 고안한 게 아니라 ext4의 표준 관례 그대로: 비트맵은 그룹당
// blocksPerGroup(또는 inodesPerGroup) 비트, **비트값 1=사용 중,
// 0=free**(리눅스 커널 fs/ext4/balloc.c/ialloc.c와 동일 - 일반적인
// "1=set/활성"이라는 직관과 반대이니 주의). 비트 인덱스는 그
// 그룹에서의 **상대** 인덱스 - 블록 비트맵은 그 그룹의 첫 데이터
// 블록을 인덱스 0으로, inode 비트맵은 그 그룹의 첫 inode 번호를
// 인덱스 0으로 삼는다(호출자가 이미 그룹 상대 인덱스로 변환해
// 넘길 것 - 이 함수들 자체는 절대 블록/inode 번호를 모른다).
// 순수 함수(I/O 없음) - 이미 읽어 온 비트맵 바이트 버퍼를 스캔/
// 갱신만 한다.
// ---------------------------------------------------------------------

// bitCount(그 그룹의 명목상 blocksPerGroup/inodesPerGroup)개 비트 중
// 처음 나오는 0비트(=free)의 상대 인덱스를 찾는다. 없으면 bitCount를
// 돌려준다(호출자가 "이 그룹엔 없음"으로 해석할 것).
inline uint32_t kExt4BitmapFindFirstFree(const uint8_t* bitmap, uint32_t bitCount) {
    const uint32_t byteCount = (bitCount + 7u) / 8u;
    for (uint32_t byteIndex = 0; byteIndex < byteCount; ++byteIndex) {
        const uint8_t byteValue = bitmap[byteIndex];
        if (byteValue == 0xFFu) {
            continue;  // 이 바이트 8비트 전부 사용 중 - 다음 바이트로
        }
        for (uint32_t bit = 0; bit < 8; ++bit) {
            const uint32_t index = byteIndex * 8u + bit;
            if (index >= bitCount) {
                return bitCount;
            }
            if ((byteValue & (1u << bit)) == 0) {
                return index;
            }
        }
    }
    return bitCount;
}

inline bool kExt4BitmapTestBit(const uint8_t* bitmap, uint32_t relIndex) {
    return (bitmap[relIndex / 8u] & (1u << (relIndex % 8u))) != 0;
}

inline void kExt4BitmapSetBit(uint8_t* bitmap, uint32_t relIndex) {
    bitmap[relIndex / 8u] = static_cast<uint8_t>(bitmap[relIndex / 8u] | (1u << (relIndex % 8u)));
}

inline void kExt4BitmapClearBit(uint8_t* bitmap, uint32_t relIndex) {
    bitmap[relIndex / 8u] = static_cast<uint8_t>(bitmap[relIndex / 8u] & ~(1u << (relIndex % 8u)));
}

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
// 가변 길이 태그 - v1(8바이트)과 CSUM_V3(16바이트,
// blocknr+flags+blocknrHigh+checksum) 둘 다 실측 확인 완료. [정확화,
// 2026-09-24, 로컬 리눅스 커널 소스(include/linux/jbd2.h
// journal_block_tag_t, fs/jbd2/journal.c journal_tag_bytes()) 대조]
// 8바이트 v1 태그는 "blocknr+flags"가 아니라 **blocknr(4)+
// checksum_be16(2)+flags_be16(2)**다 - 이 checksum 필드가 CSUM_V2/V3
// 기능이 꺼져 있을 때 항상 0이라, 그 2바이트+뒤이은 flags 2바이트를
// 하나의 be32로 읽어도(현재 구현) checksum 상위 바이트가 전부
// 0이므로 flags 값과 수치가 정확히 같아 결과적으로 문제없다(우연이
// 아니라 checksum=0이 보장되는 조건에서만 성립하는 구조적 사실).
// **CSUM_V2 단독**(checksum 필드가 실제로 0이 아닐 수 있는 유일한
// 8~10바이트 조합)을 여전히 미지원으로 거부하는 이유가 바로 이것 -
// 그 경우 이 4바이트 뭉치 읽기가 flags를 잘못된 값으로 오염시킨다.
// 같은
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

// ---------------------------------------------------------------------
// 3.7 쿼터 파일 온디스크 포맷 "quota v2"(vfsv0/vfsv1, PN-D168A778 준비
// 작업) - **이 프로젝트가 새로 고안한 포맷이 아니고, ext4 전용도
// 아니다** - 리눅스 VFS가 여러 파일시스템에 공용으로 쓰는 포맷
// (`fs/quota/quotaio_v2.h`)을 `RO_COMPAT_QUOTA`(위 `kRoCompatQuota`)
// 켜진 ext4가 `SuperblockCore::usrQuotaInum`/`grpQuotaInum`이 가리키는
// 일반 파일 inode 안에 그대로 저장하는 것뿐이다. 실제
// `mke2fs -O quota,metadata_csum` 이미지의 사용자 쿼터 inode(3번)
// 내용을 직접 읽어 매직(`0xd9c01f11`=USRQUOTA)/버전(1)/
// `dqi_bgrace`·`dqi_igrace`(둘 다 604800초=7일, 표준 기본값)/
// `dqi_blocks`(파일 크기 6144바이트 = 1024바이트 쿼터 블록 6개와
// 일치)까지 실측 대조 완료(PN-D168A778). **이 두 struct(헤더+정보)
// 까지만 검증했다 - 실제 쿼터 레코드가 저장되는 radix-tree 구조
// (`fs/quota/quota_tree.c`, `V2_DQBLKSIZE_BITS=10`=1024바이트 리프
// 블록)와 사용자별 `v2r1_disk_dqblk` 레코드 파싱/탐색은 이번 준비
// 작업 범위 밖 - 착수 세션이 실제 사용량이 기록된 이미지(quotacheck
// 등으로 채워야 함, 이 세션 WSL 환경엔 quota 패키지 자체가 없어
// 준비 못 함)로 별도 검증할 것.
// ---------------------------------------------------------------------
constexpr uint32_t kQuotaV2MagicUser = 0xd9c01f11;
constexpr uint32_t kQuotaV2MagicGroup = 0xd9c01927;
constexpr uint32_t kQuotaV2MagicProject = 0xd9c03f14;
constexpr uint32_t kQuotaV2Version = 1;

#pragma pack(push, 1)
struct QuotaV2Header {
    uint32_t magic;    // kQuotaV2Magic* 중 하나와 일치해야 함
    uint32_t version;  // kQuotaV2Version(1)과 일치 - v1은 그 외 버전 미지원
};
static_assert(sizeof(QuotaV2Header) == 8, "QuotaV2Header는 8바이트");

// 헤더(오프셋 0~8) 바로 뒤(오프셋 8~32)에 이어지는 포맷 정보 - 실제
// 쿼터 레코드가 담긴 radix-tree는 이 뒤(쿼터 파일의 두 번째
// 1024바이트 블록부터) 시작한다.
struct QuotaV2Info {
    uint32_t bgrace;      // 블록 소프트 한도 초과 유예 시간(초) - 보통 604800(7일)
    uint32_t igrace;      // inode 소프트 한도 초과 유예 시간(초)
    uint32_t flags;       // DQF_* 비트마스크(v1 미해석)
    uint32_t blocks;      // 쿼터 파일 전체의 1024바이트 블록 수
    uint32_t freeBlk;     // 프리 리스트의 첫 블록 번호
    uint32_t freeEntry;   // 빈 엔트리가 있는 블록 중 하나의 번호
};
static_assert(sizeof(QuotaV2Info) == 24, "QuotaV2Info는 24바이트");
#pragma pack(pop)

// ---------------------------------------------------------------------
// [신규, 2026-09-25, PN-D168A778] quota v2 레코드 radix-tree
// (`fs/quota/quota_tree.c`) - 이전 준비 작업이 "실제 사용량이 기록된
// 이미지가 없어 검증 못함, 범위 밖"으로 남겨 뒀던 부분. 이번엔 실제
// `quota` 패키지를 설치(WSL, sudo apt-get install quota)하고 loop
// mount + `setquota`로 uid 0(root, quotacheck가 자동 기록)/1001/1002
// 세 사용자의 실제 레코드가 채워진 이미지를 만들어 1바이트 단위까지
// 대조 완료 - 세 레코드 전부(ihardlimit/isoftlimit/curinodes/
// bhardlimit/bsoftlimit/curspace/btime/itime 8개 필드) `repquota`
// 출력과 정확히 일치.
//
// **트리 구조 실측 결과**: 쿼터 파일의 두 번째 1024바이트 블록
// (파일 블록 인덱스 `kQtreeTreeOff`=1)이 루트 인덱스 노드 - 1024바이트
// 블록 기준 `epb`(entries per block)=256(4바이트 참조 256개)이고
// `kQtreeDepth(1024)`=4단계 인덱스 홉(레벨 0~3, 각 레벨에서 id의
// 해당 바이트로 다음 파일 블록 인덱스를 찾음) 뒤에야 실제 리프
// 블록에 도달한다 - id=0(root)/1001/1002 전부 상위 두 레벨(레벨0/1)
// 에서는 인덱스 0을 공유(전부 65536 미만)하고, 레벨2에서 갈라짐
// (root는 0, 1001·1002는 `(id>>8)&0xFF`=3으로 동일) - 레벨3
// (`id&0xFF`)에서는 root=0, 1001=233, 1002=234로 서로 다른 슬롯을
// 쓰지만, **실측 결과 셋 다 결국 같은 리프 블록(파일 블록 인덱스
// 5)을 가리켰다** - `QuotaV2Info::freeEntry`가 가리키는 "빈 자리
// 있는 블록"을 재사용하는 할당 정책 때문으로 보인다(쓰기 경로 고유
// 동작 - 이 갭 자체는 읽기 전용 순회 로직과 무관, 그냥 "인덱스가
// 갈라져도 같은 리프를 공유할 수 있다"는 사실만 알면 충분).
//
// v1은 순수 계산 함수만 제공한다(단계별 인덱스 계산, 리프 엔트리
// 파싱/탐색) - 여러 블록을 오가는 실제 I/O 순회는 아직 이 순수
// 함수들을 쓸 실제 소비자(예: uid별 쿼터 조회 syscall)가 없어
// ext4_driver.cpp에 배선하지 않는다(불필요한 추상화를 미리 만들지
// 않는다는 이 프로젝트의 원칙) - 실제 소비자가 생기면 Open/Read
// 케이스의 익스텐트 순회(`kLookupExtent`+`NeedChild` 루프)와 동일한
// 패턴으로 그 소비자 쪽에서 조립할 것.
// ---------------------------------------------------------------------
constexpr uint32_t kQtreeTreeOff = 1;  // QT_TREEOFF - 쿼터 파일의 두 번째 블록(파일 블록 인덱스 1)부터 트리 루트

// 1024바이트 블록 기준 리프 엔트리 하나(quota v2r1, e2fsprogs/커널
// 공용 온디스크 레코드) - id(4)+pad(4) 뒤에 8바이트 정수 8개가
// 이어진다. 실측(uid 0/1001/1002 세 레코드 전부)으로 필드 순서
// 확정: ihardlimit → isoftlimit → curinodes → bhardlimit →
// bsoftlimit → curspace(바이트 단위, 블록 수 아님) → btime → itime.
#pragma pack(push, 1)
struct QuotaV2DiskDqblk {
    uint32_t id;
    uint32_t pad;
    uint64_t ihardlimit;
    uint64_t isoftlimit;
    uint64_t curinodes;
    uint64_t bhardlimit;
    uint64_t bsoftlimit;
    uint64_t curspace;
    uint64_t btime;
    uint64_t itime;
};
static_assert(sizeof(QuotaV2DiskDqblk) == 72, "QuotaV2DiskDqblk는 72바이트여야 함");

// 리프 블록 맨 앞 16바이트(`qt_disk_dqdbheader`) - entries가 그
// 블록에 실제로 들어 있는(=유효한) `QuotaV2DiskDqblk` 레코드 개수
// (블록 앞쪽부터 그 개수만큼 유효하다고 실측으로 확인 - PN-D168A778).
struct QtreeLeafHeader {
    uint32_t nextFree;
    uint32_t prevFree;
    uint16_t entries;
    uint16_t pad1;
    uint32_t pad2;
};
static_assert(sizeof(QtreeLeafHeader) == 16, "QtreeLeafHeader는 16바이트여야 함");
#pragma pack(pop)

// 트리 인덱스 홉 수(`fs/quota/quota_tree.c`의 `qtree_depth()`와 동일
// 계산 - "블록당 4바이트 참조 개수(epb)를 계속 곱해 2^32를 넘기는
// 데 필요한 횟수") - 1024바이트 블록이면 epb=256, 256^4=2^32라 4를
// 돌려준다(실측 확인).
inline uint32_t kQtreeDepth(uint32_t blockSize) {
    const uint32_t epb = blockSize / sizeof(uint32_t);
    uint64_t entries = epb;
    uint32_t depth = 1;
    while (entries < (uint64_t{1} << 32)) {
        entries *= epb;
        ++depth;
    }
    return depth;
}

// 트리 레벨 하나(0-based, 0=루트 바로 다음 홉)에서 id가 가리키는
// 그 레벨 배열의 상대 인덱스 - `__get_index(id, depth)`와 동일한
// 계산(최상위 자리부터 `level`번째 자리를 뽑음: `(totalDepth-level-1)`
// 번 `id /= epb`한 뒤 `id % epb`). 실측(uid 1001=0x3E9)으로 확인:
// level0=0, level1=0, level2=(1001>>8)&0xFF=3, level3=1001&0xFF=233
// (epb=256일 때는 나눗셈이 바이트 시프트와 결과가 같지만, 이 함수는
// 일반적인 나눗셈으로 계산해 epb가 256이 아닌 블록 크기에서도 맞다).
inline uint32_t kQtreeGetIndex(uint32_t id, uint32_t level, uint32_t totalDepth, uint32_t epb) {
    uint32_t remaining = id;
    for (uint32_t i = 0; i < totalDepth - level - 1; ++i) {
        remaining /= epb;
    }
    return remaining % epb;
}

// 리프 블록 하나(blockSize바이트, 이미 디스크에서 읽어 옴)에서 id가
// 일치하는 레코드를 찾는다 - 블록 맨 앞 `QtreeLeafHeader::entries`
// 개수만큼만 유효하다고 보고 순서대로 스캔(실측 확인 - 이 프로젝트
// 순회 범위에서는 뒤쪽에 가비지가 남아 있어도 entries 카운트 밖은
// 안 본다). 찾으면 outEntry를 채우고 true, 못 찾으면 false.
inline bool kQtreeFindEntryInLeaf(const uint8_t* leafBlockData, uint32_t blockSize, uint32_t id,
                                   QuotaV2DiskDqblk* outEntry) {
    QtreeLeafHeader header;
    memcpy(&header, leafBlockData, sizeof(header));
    uint32_t offset = sizeof(QtreeLeafHeader);
    for (uint16_t i = 0; i < header.entries; ++i) {
        if (offset + sizeof(QuotaV2DiskDqblk) > blockSize) {
            break;  // 손상 방어
        }
        QuotaV2DiskDqblk entry;
        memcpy(&entry, leafBlockData + offset, sizeof(entry));
        if (entry.id == id) {
            *outEntry = entry;
            return true;
        }
        offset += sizeof(QuotaV2DiskDqblk);
    }
    return false;
}

}  // namespace ext4

#endif  // MINICORE_LIBEXT4_EXT4_H
