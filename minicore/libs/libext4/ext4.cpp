#include "ext4.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace ext4 {

namespace {

constexpr uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

}  // namespace

uint32_t kCrc32c(uint32_t seed, const void* data, uint32_t len) {
    // 표준 reflected CRC-32C(Castagnoli) 다항식. 초기/최종 보수는 호출자
    // 책임(위 ext4.h 선언부 주석 참고) - 이 함수 자체는 seed를 그대로
    // 이어받아 이어붙이는 raw continuation 스텝일 뿐이다.
    constexpr uint32_t kPoly = 0x82F63B78u;
    uint32_t crc = seed;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (kPoly & mask);
        }
    }
    return crc;
}

uint16_t kExt4ComputeGroupDescChecksum(const uint8_t uuid[16], uint32_t groupNum, const GroupDesc32& desc) {
    // 실제 mkfs.ext4 -O metadata_csum 이미지(단일/3그룹 구성 둘 다)의
    // bg_checksum과 1바이트씩 대조해 확인한 순서(PN-1750A32F) -
    // 1) uuid로 시드, 2) 그룹 번호(LE) 이어붙임, 3) 체크섬 필드 앞부분,
    // 4) 체크섬 필드 자리는 0으로 간주해 이어붙임 - 32바이트 디스크립터
    // (INCOMPAT_64BIT 미지원 범위)라 체크섬 필드가 곧 구조체 끝이므로
    // 그 뒤에 이어붙일 나머지 바이트는 없다.
    GroupDesc32 raw = desc;
    const uint32_t offsetToChecksum = static_cast<uint32_t>(offsetof(GroupDesc32, checksum));
    static_assert(offsetof(GroupDesc32, checksum) == 30, "checksum 필드 오프셋이 어긋나면 아래 체크섬 계산이 리눅스와 안 맞음");
    uint8_t* rawBytes = reinterpret_cast<uint8_t*>(&raw);

    uint32_t crc = kCrc32c(0xFFFFFFFFu, uuid, 16);
    const uint32_t groupNumLe = groupNum;  // 이 프로젝트는 x86_64 리틀엔디안 전용 - 별도 변환 불필요
    crc = kCrc32c(crc, &groupNumLe, sizeof(groupNumLe));
    crc = kCrc32c(crc, rawBytes, offsetToChecksum);
    const uint16_t zeroChecksum = 0;
    crc = kCrc32c(crc, &zeroChecksum, sizeof(zeroChecksum));
    // offsetToChecksum + sizeof(checksum) == sizeof(GroupDesc32) == 32이므로
    // 체크섬 필드 뒤에 이어붙일 나머지 바이트가 없다(64bit 확장 디스크립터
    // 지원 시 여기에 tail 해시가 추가돼야 함 - §2.2 후속 스코프).
    static_assert(offsetof(GroupDesc32, checksum) + sizeof(GroupDesc32::checksum) == sizeof(GroupDesc32),
                  "checksum이 GroupDesc32의 마지막 필드가 아니면 tail 해시 단계를 추가해야 함");

    return static_cast<uint16_t>(crc & 0xFFFFu);
}

uint16_t kExt4ComputeGroupDesc64Checksum(const uint8_t uuid[16], uint32_t groupNum, const GroupDesc64& desc) {
    // kExt4ComputeGroupDescChecksum()(32바이트 버전)과 seed/이어붙임
    // 순서는 동일 - 다른 점은 체크섬 필드(오프셋30) 뒤에 남는 34바이트
    // (오프셋32~64, hi 필드들)를 마저 이어붙이는 tail 해시 단계뿐
    // (실제 mke2fs -O 64bit,metadata_csum 이미지로 대조 확인,
    // PN-59C253E9).
    GroupDesc64 raw = desc;
    constexpr uint32_t kChecksumOffset = 30;
    static_assert(offsetof(GroupDesc64, checksum) == kChecksumOffset,
                  "checksum 필드 오프셋이 어긋나면 아래 체크섬 계산이 리눅스와 안 맞음");
    uint8_t* rawBytes = reinterpret_cast<uint8_t*>(&raw);

    uint32_t crc = kCrc32c(0xFFFFFFFFu, uuid, 16);
    const uint32_t groupNumLe = groupNum;
    crc = kCrc32c(crc, &groupNumLe, sizeof(groupNumLe));
    crc = kCrc32c(crc, rawBytes, kChecksumOffset);
    const uint16_t zeroChecksum = 0;
    crc = kCrc32c(crc, &zeroChecksum, sizeof(zeroChecksum));
    crc = kCrc32c(crc, rawBytes + kChecksumOffset + 2, sizeof(GroupDesc64) - (kChecksumOffset + 2));

    return static_cast<uint16_t>(crc & 0xFFFFu);
}

uint32_t kExt4ComputeBitmapChecksum(const uint8_t uuid[16], const void* bitmapData, uint32_t bitCount) {
    // 실제 mkfs.ext4 -O metadata_csum 이미지(단일 그룹 + "마지막 그룹이
    // blocksPerGroup보다 작은" 3그룹 258MB 구성 둘 다)의 bg_block_bitmap_
    // csum_lo/bg_inode_bitmap_csum_lo와 1바이트씩 대조해 확인(PN-625E2804) -
    // 그룹 디스크립터 체크섬과 달리 그룹 번호를 이어붙이지 않고, uuid
    // 시드 바로 다음에 비트맵 바이트를 이어붙인다. 해시 길이는 호출자가
    // 넘긴 bitCount(항상 볼륨 전체의 명목상 blocksPerGroup/inodesPerGroup -
    // 그 그룹의 실제 유효 비트 수가 아님, 위 ext4.h 선언부 주석 참고)를
    // 8로 나눠 올림한 바이트 수. 반환값은 32비트 전체 그대로(위 ext4.h
    // 선언부 주석 참고 - 64바이트 디스크립터는 상위 16비트도 실제로
    // 저장된다, PN-59C253E9 실측 확인).
    const uint32_t byteLen = (bitCount + 7u) / 8u;
    return kCrc32c(kCrc32c(0xFFFFFFFFu, uuid, 16), bitmapData, byteLen);
}

namespace {

// groupDescBuf(온디스크 stride 그대로)에서 free count 필드 하나를
// lo+hi 16비트 쌍으로 읽어 합성 - freeBlocksCountLo/Hi와
// freeInodesCountLo/Hi 둘 다 같은 오프셋 패턴(GroupDesc32/64에서
// 완전히 동일한 상대 위치)이라 오프셋만 매개변수로 받는다.
uint32_t kReadGroupDescFreeCount(const uint8_t* groupDescBuf, uint32_t loOffset, uint32_t hiOffset, bool is64Bit) {
    uint16_t lo;
    memcpy(&lo, groupDescBuf + loOffset, sizeof(lo));
    uint16_t hi = 0;
    if (is64Bit) {
        memcpy(&hi, groupDescBuf + hiOffset, sizeof(hi));
    }
    return (static_cast<uint32_t>(hi) << 16) | lo;
}

void kWriteGroupDescFreeCount(uint8_t* groupDescBuf, uint32_t loOffset, uint32_t hiOffset, bool is64Bit,
                               uint32_t value) {
    const uint16_t lo = static_cast<uint16_t>(value & 0xFFFFu);
    memcpy(groupDescBuf + loOffset, &lo, sizeof(lo));
    if (is64Bit) {
        const uint16_t hi = static_cast<uint16_t>(value >> 16);
        memcpy(groupDescBuf + hiOffset, &hi, sizeof(hi));
    }
}

// groupDescBuf의 bg_checksum 필드를 재계산해 써 넣는다 - is64Bit
// 여부에 따라 32/64바이트 버전 체크섬 함수 중 맞는 쪽을 호출.
void kRecomputeGroupDescChecksum(const uint8_t uuid[16], uint32_t groupNum, bool is64Bit, uint8_t* groupDescBuf) {
    uint16_t checksum;
    if (is64Bit) {
        GroupDesc64 desc;
        memcpy(&desc, groupDescBuf, sizeof(desc));
        checksum = kExt4ComputeGroupDesc64Checksum(uuid, groupNum, desc);
    } else {
        GroupDesc32 desc;
        memcpy(&desc, groupDescBuf, sizeof(desc));
        checksum = kExt4ComputeGroupDescChecksum(uuid, groupNum, desc);
    }
    memcpy(groupDescBuf + offsetof(GroupDesc32, checksum), &checksum, sizeof(checksum));
}

// 비트맵 할당/해제 공용 본체(블록/inode 어느 쪽이든 동일 절차) -
// freeCountLoOffset/HiOffset과 csumLoOffset/HiOffset만 호출자(블록용/
// inode용)가 다르게 넘긴다. allocating=true면 첫 free 비트를 찾아
// 세우고 free count -1, false면 relIndex 비트를 지우고 free count +1.
bool kExt4ToggleBitInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t bitsPerGroup,
                            uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf, uint8_t* groupDescBuf,
                            uint32_t freeCountLoOffset, uint32_t freeCountHiOffset, uint32_t csumLoOffset,
                            uint32_t csumHiOffset, bool allocating, uint32_t* relIndexInOut) {
    if (allocating) {
        const uint32_t found = kExt4BitmapFindFirstFree(bitmapBuf, bitsPerGroup);
        if (found >= bitsPerGroup) {
            return false;
        }
        *relIndexInOut = found;
    } else {
        if (!kExt4BitmapTestBit(bitmapBuf, *relIndexInOut)) {
            return false;  // 이미 free인 비트를 또 해제하려 함 - 이중 해제 방어
        }
    }

    uint32_t freeCount = kReadGroupDescFreeCount(groupDescBuf, freeCountLoOffset, freeCountHiOffset, is64Bit);
    if (allocating) {
        if (freeCount == 0) {
            return false;  // 비트맵-free count 불일치(방어적) - 아무것도 안 바꿈
        }
        kExt4BitmapSetBit(bitmapBuf, *relIndexInOut);
        --freeCount;
    } else {
        kExt4BitmapClearBit(bitmapBuf, *relIndexInOut);
        ++freeCount;
    }
    kWriteGroupDescFreeCount(groupDescBuf, freeCountLoOffset, freeCountHiOffset, is64Bit, freeCount);

    if (featureRoCompat & kRoCompatMetadataCsum) {
        const uint32_t bitmapCsum = kExt4ComputeBitmapChecksum(uuid, bitmapBuf, bitsPerGroup);
        const uint16_t csumLo = static_cast<uint16_t>(bitmapCsum & 0xFFFFu);
        memcpy(groupDescBuf + csumLoOffset, &csumLo, sizeof(csumLo));
        if (is64Bit) {
            const uint16_t csumHi = static_cast<uint16_t>(bitmapCsum >> 16);
            memcpy(groupDescBuf + csumHiOffset, &csumHi, sizeof(csumHi));
        }
        kRecomputeGroupDescChecksum(uuid, groupNum, is64Bit, groupDescBuf);
    }
    return true;
}

}  // namespace

bool kExt4AllocateBlockInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t blocksPerGroup,
                                uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                                uint8_t* groupDescBuf, uint32_t* outRelIndex) {
    return kExt4ToggleBitInGroup(uuid, groupNum, blocksPerGroup, featureRoCompat, is64Bit, bitmapBuf,
                                  groupDescBuf, offsetof(GroupDesc32, freeBlocksCountLo),
                                  offsetof(GroupDesc64, freeBlocksCountHi), offsetof(GroupDesc32, blockBitmapCsumLo),
                                  offsetof(GroupDesc64, blockBitmapCsumHi), /*allocating=*/true, outRelIndex);
}

bool kExt4FreeBlockInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t blocksPerGroup,
                            uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                            uint8_t* groupDescBuf, uint32_t relIndex) {
    return kExt4ToggleBitInGroup(uuid, groupNum, blocksPerGroup, featureRoCompat, is64Bit, bitmapBuf,
                                  groupDescBuf, offsetof(GroupDesc32, freeBlocksCountLo),
                                  offsetof(GroupDesc64, freeBlocksCountHi), offsetof(GroupDesc32, blockBitmapCsumLo),
                                  offsetof(GroupDesc64, blockBitmapCsumHi), /*allocating=*/false, &relIndex);
}

bool kExt4AllocateInodeInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t inodesPerGroup,
                                uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                                uint8_t* groupDescBuf, uint32_t* outRelIndex) {
    if (!kExt4ToggleBitInGroup(uuid, groupNum, inodesPerGroup, featureRoCompat, is64Bit, bitmapBuf, groupDescBuf,
                                offsetof(GroupDesc32, freeInodesCountLo), offsetof(GroupDesc64, freeInodesCountHi),
                                offsetof(GroupDesc32, inodeBitmapCsumLo), offsetof(GroupDesc64, inodeBitmapCsumHi),
                                /*allocating=*/true, outRelIndex)) {
        return false;
    }
    // [신규, 2026-09-25, PN-FE718C87 - PN-4C67E1ED 실측(e2fsck)으로
    // 발견된 갭] bg_itable_unused(그 그룹의 inode 테이블 끝에서부터
    // "한 번도 안 쓰여 반드시 0으로 채워져 있다고 보장되는" inode
    // 개수 - mke2fs/e2fsck가 그 구간을 안 읽어도 되게 해주는 최적화
    // 힌트, 리눅스 커널 fs/ext4/ialloc.c `ext4_new_inode()`와 동일한
    // 조건) 갱신을 빼먹으면, 방금 할당한 inode가 여전히 "미사용
    // 꼬리 구간 안"이라고 잘못 표시된 채로 남는다. 1-based 그룹
    // 상대 inode 번호(ino)가 (inodesPerGroup - 현재 unused값)를
    // 넘으면(=미사용 꼬리 구간을 침범하면) unused를 (inodesPerGroup
    // - ino)로 줄인다. **실측(PN-4C67E1ED)**: 이 갱신 없이 실제
    // Mkdir을 태워 e2fsck -fn을 돌리면 "Group descriptor 0 has
    // invalid unused inodes count"부터 그 inode를 "unused 영역에
    // 있다"고 오판해 디렉터리 엔트리/블록 비트맵/부모 링크 카운트
    // 까지 전부 연쇄로 틀렸다고 보고한다(단일 근본 원인의 연쇄
    // 증상이었음, 실제 데이터 자체는 멀쩡했다).
    const uint32_t ino = *outRelIndex + 1;
    const uint32_t unusedLoOffset = offsetof(GroupDesc32, itableUnusedLo);
    const uint32_t unusedHiOffset = offsetof(GroupDesc64, itableUnusedHi);
    const uint32_t unused = kReadGroupDescFreeCount(groupDescBuf, unusedLoOffset, unusedHiOffset, is64Bit);
    if (ino > inodesPerGroup - unused) {
        kWriteGroupDescFreeCount(groupDescBuf, unusedLoOffset, unusedHiOffset, is64Bit, inodesPerGroup - ino);
        if (featureRoCompat & kRoCompatMetadataCsum) {
            kRecomputeGroupDescChecksum(uuid, groupNum, is64Bit, groupDescBuf);
        }
    }
    return true;
}

bool kExt4FreeInodeInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t inodesPerGroup,
                            uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                            uint8_t* groupDescBuf, uint32_t relIndex) {
    return kExt4ToggleBitInGroup(uuid, groupNum, inodesPerGroup, featureRoCompat, is64Bit, bitmapBuf,
                                  groupDescBuf, offsetof(GroupDesc32, freeInodesCountLo),
                                  offsetof(GroupDesc64, freeInodesCountHi), offsetof(GroupDesc32, inodeBitmapCsumLo),
                                  offsetof(GroupDesc64, inodeBitmapCsumHi), /*allocating=*/false, &relIndex);
}

bool kExt4AdjustGroupDescUsedDirs(const uint8_t uuid[16], uint32_t groupNum, uint32_t featureRoCompat, bool is64Bit,
                                    uint8_t* groupDescBuf, kernel::int64_t delta) {
    const uint32_t loOffset = offsetof(GroupDesc32, usedDirsCountLo);
    const uint32_t hiOffset = offsetof(GroupDesc64, usedDirsCountHi);
    const uint32_t current = kReadGroupDescFreeCount(groupDescBuf, loOffset, hiOffset, is64Bit);
    const kernel::int64_t updated = static_cast<kernel::int64_t>(current) + delta;
    if (updated < 0) {
        return false;
    }
    kWriteGroupDescFreeCount(groupDescBuf, loOffset, hiOffset, is64Bit, static_cast<uint32_t>(updated));
    if (featureRoCompat & kRoCompatMetadataCsum) {
        kRecomputeGroupDescChecksum(uuid, groupNum, is64Bit, groupDescBuf);
    }
    return true;
}

bool kExt4AdjustSuperblockFreeBlocks(void* rawSuperblock1024Bytes, kernel::int64_t blocksDelta,
                                      uint32_t featureIncompat, uint32_t featureRoCompat) {
    auto* raw = static_cast<uint8_t*>(rawSuperblock1024Bytes);
    uint32_t lo;
    memcpy(&lo, raw + offsetof(SuperblockCore, freeBlocksCountLo), sizeof(lo));
    uint32_t hi = 0;
    if (featureIncompat & kIncompat64Bit) {
        memcpy(&hi, raw + offsetof(SuperblockCore, freeBlocksCountHi), sizeof(hi));
    }
    const uint64_t current = (static_cast<uint64_t>(hi) << 32) | lo;
    const kernel::int64_t updated = static_cast<kernel::int64_t>(current) + blocksDelta;
    if (updated < 0) {
        return false;
    }
    lo = static_cast<uint32_t>(static_cast<uint64_t>(updated) & 0xFFFFFFFFu);
    memcpy(raw + offsetof(SuperblockCore, freeBlocksCountLo), &lo, sizeof(lo));
    if (featureIncompat & kIncompat64Bit) {
        hi = static_cast<uint32_t>(static_cast<uint64_t>(updated) >> 32);
        memcpy(raw + offsetof(SuperblockCore, freeBlocksCountHi), &hi, sizeof(hi));
    }
    if (featureRoCompat & kRoCompatMetadataCsum) {
        const uint32_t checksum = kExt4ComputeSuperblockChecksum(raw);
        memcpy(raw + kSuperblockChecksumOffset, &checksum, sizeof(checksum));
    }
    return true;
}

bool kExt4AdjustSuperblockFreeInodes(void* rawSuperblock1024Bytes, kernel::int64_t inodesDelta,
                                      uint32_t featureRoCompat) {
    auto* raw = static_cast<uint8_t*>(rawSuperblock1024Bytes);
    uint32_t count;
    memcpy(&count, raw + offsetof(SuperblockCore, freeInodesCount), sizeof(count));
    const kernel::int64_t updated = static_cast<kernel::int64_t>(count) + inodesDelta;
    if (updated < 0) {
        return false;
    }
    count = static_cast<uint32_t>(updated);
    memcpy(raw + offsetof(SuperblockCore, freeInodesCount), &count, sizeof(count));
    if (featureRoCompat & kRoCompatMetadataCsum) {
        const uint32_t checksum = kExt4ComputeSuperblockChecksum(raw);
        memcpy(raw + kSuperblockChecksumOffset, &checksum, sizeof(checksum));
    }
    return true;
}

uint32_t kExt4ComputeInodeChecksum(const uint8_t uuid[16], uint32_t inodeNum, uint32_t generation,
                                    const void* rawInode, uint32_t inodeSize) {
    // 실제 mkfs.ext4 -O metadata_csum 이미지의 root inode(2번, generation=0)
    // + debugfs로 새로 쓴 파일 inode 2개(11/12번)까지 총 3개를 실측
    // 대조해 확정한 순서(PN-625E2804) - 리눅스 커널
    // ext4_inode_csum()/__ext4_iget()의 per-inode seed 계산과 동일:
    // 1) 전역 uuid seed에 inode 번호(LE32)+generation(LE32) 순서로
    //    이어붙여 이 inode 전용 seed를 만들고,
    // 2) good-old 128바이트 영역을 i_checksum_lo 필드(오프셋 124) 앞/
    //    뒤로 나눠 그 필드 자리는 0으로 간주해 이어붙이고,
    // 3) inodeSize>128(확장 필드 존재)이면 128~130(extraIsize 필드
    //    자체)을 이어붙인 뒤, extraIsize가 i_checksum_hi(오프셋 130)까지
    //    실제로 덮는 경우에만 그 필드도 0으로 간주해 이어붙이고, 나머지
    //    꼬리를 이어붙인다.
    constexpr uint32_t kGoodOldInodeSize = 128;
    constexpr uint32_t kChecksumLoOffset = 124;  // osd2[12](오프셋116)의 8~10바이트 - l_i_checksum_lo
    constexpr uint32_t kChecksumHiOffset = 130;  // extraIsize(128,2) 다음 - InodeCore::checksumHi와 동일 오프셋
    static_assert(offsetof(InodeCore, osd2) + 8 == kChecksumLoOffset,
                  "l_i_checksum_lo 오프셋이 InodeCore::osd2 레이아웃과 어긋남");
    static_assert(offsetof(InodeCore, checksumHi) == kChecksumHiOffset,
                  "i_checksum_hi 오프셋이 InodeCore::checksumHi와 어긋남");
    const uint8_t* raw = static_cast<const uint8_t*>(rawInode);
    const uint16_t zero16 = 0;

    uint32_t seed = kCrc32c(kCrc32c(0xFFFFFFFFu, uuid, 16), &inodeNum, sizeof(inodeNum));
    seed = kCrc32c(seed, &generation, sizeof(generation));

    uint32_t crc = kCrc32c(seed, raw, kChecksumLoOffset);
    crc = kCrc32c(crc, &zero16, sizeof(zero16));
    crc = kCrc32c(crc, raw + kChecksumLoOffset + 2, kGoodOldInodeSize - (kChecksumLoOffset + 2));

    if (inodeSize > kGoodOldInodeSize) {
        crc = kCrc32c(crc, raw + kGoodOldInodeSize, kChecksumHiOffset - kGoodOldInodeSize);
        uint16_t extraIsize;
        memcpy(&extraIsize, raw + kGoodOldInodeSize, sizeof(extraIsize));
        const bool fitsChecksumHi = (kGoodOldInodeSize + extraIsize) >= (kChecksumHiOffset + 2);
        uint32_t offset = kChecksumHiOffset;
        if (fitsChecksumHi) {
            crc = kCrc32c(crc, &zero16, sizeof(zero16));
            offset += 2;
        }
        crc = kCrc32c(crc, raw + offset, inodeSize - offset);
    }
    return crc;
}

uint32_t kExt4ComputeDirBlockChecksum(const uint8_t uuid[16], uint32_t inodeNum, uint32_t generation,
                                       const void* dirBlockData, uint32_t blockSize) {
    uint32_t seed = kCrc32c(kCrc32c(0xFFFFFFFFu, uuid, 16), &inodeNum, sizeof(inodeNum));
    seed = kCrc32c(seed, &generation, sizeof(generation));
    return kCrc32c(seed, dirBlockData, blockSize - static_cast<uint32_t>(sizeof(DirEntryTail)));
}

uint32_t kExt4ComputeDxTailChecksum(const uint8_t uuid[16], uint32_t inodeNum, uint32_t generation,
                                     const void* blockData, uint32_t countOffset, uint32_t count, uint32_t limit) {
    const uint8_t* raw = static_cast<const uint8_t*>(blockData);
    uint32_t seed = kCrc32c(kCrc32c(0xFFFFFFFFu, uuid, 16), &inodeNum, sizeof(inodeNum));
    seed = kCrc32c(seed, &generation, sizeof(generation));
    const uint32_t hashSize = countOffset + count * 8u;
    uint32_t crc = kCrc32c(seed, raw, hashSize);
    // dx_tail은 count가 아니라 limit 기준 오프셋에 있다(위 ext4.h 문서
    // 주석 참고) - dt_reserved는 실제 온디스크 값 그대로 해시하고,
    // dt_checksum 필드 자신만 0으로 간주한다.
    const uint32_t tailOffset = countOffset + limit * 8u;
    crc = kCrc32c(crc, raw + tailOffset, 4);
    const uint32_t dummyChecksum = 0;
    crc = kCrc32c(crc, &dummyChecksum, sizeof(dummyChecksum));
    return crc;
}

bool kExt4InsertDirEntry(uint8_t* dirBlockData, uint32_t blockSize, uint32_t hasTailBytes, uint32_t inode,
                          const char* name, uint8_t nameLen, uint8_t fileType) {
    const uint32_t reclenNeeded = kExt4DirRecLen(nameLen);
    const uint32_t searchLimit = blockSize - hasTailBytes;
    uint32_t offset = 0;
    while (offset + sizeof(DirEntry2Header) <= searchLimit) {
        DirEntry2Header entry;
        memcpy(&entry, dirBlockData + offset, sizeof(entry));
        if (entry.recLen < sizeof(DirEntry2Header) || offset + entry.recLen > searchLimit) {
            break;  // 손상 방어 - 더 진행하지 않고 실패 처리
        }
        const uint32_t entryOwnSize = (entry.inode != 0) ? kExt4DirRecLen(entry.nameLen) : 0;
        const uint32_t available = entry.recLen - entryOwnSize;
        if (available >= reclenNeeded) {
            uint32_t newEntryOffset = offset;
            uint16_t newEntryRecLen = entry.recLen;
            if (entry.inode != 0) {
                // 사용 중인 엔트리의 슬랙만 떼어낸다 - 기존 엔트리는
                // 자신의 실제 필요 크기로 줄어들고, 새 엔트리가 그
                // 뒤(슬랙 자리)에 들어간다.
                entry.recLen = static_cast<uint16_t>(entryOwnSize);
                memcpy(dirBlockData + offset, &entry, sizeof(entry));
                newEntryOffset = offset + entryOwnSize;
                newEntryRecLen = static_cast<uint16_t>(available);
            }
            // else: 삭제된 엔트리(inode==0)를 recLen 그대로 통째로
            // 재사용 - 분할하지 않는다(커널의 ext4_insert_dentry와
            // 동일한 분기).
            DirEntry2Header newEntry;
            newEntry.inode = inode;
            newEntry.recLen = newEntryRecLen;
            newEntry.nameLen = nameLen;
            newEntry.fileType = fileType;
            memcpy(dirBlockData + newEntryOffset, &newEntry, sizeof(newEntry));
            memcpy(dirBlockData + newEntryOffset + sizeof(DirEntry2Header), name, nameLen);
            return true;
        }
        offset += entry.recLen;
    }
    return false;
}

bool kExt4RemoveDirEntry(uint8_t* dirBlockData, uint32_t blockSize, const char* name, uint8_t nameLen,
                          uint8_t* outFileType) {
    uint32_t offset = 0;
    while (offset + sizeof(DirEntry2Header) <= blockSize) {
        auto* entry = reinterpret_cast<DirEntry2Header*>(dirBlockData + offset);
        if (entry->recLen < sizeof(DirEntry2Header) || offset + entry->recLen > blockSize) {
            break;  // 손상 방어
        }
        if (entry->inode != 0 && entry->nameLen == nameLen) {
            const char* entryName = reinterpret_cast<const char*>(dirBlockData + offset + sizeof(DirEntry2Header));
            bool matches = true;
            for (uint32_t i = 0; i < nameLen; ++i) {
                if (entryName[i] != name[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                if (outFileType) {
                    *outFileType = entry->fileType;
                }
                entry->inode = 0;
                return true;
            }
        }
        offset += entry->recLen;
    }
    return false;
}

namespace {

inline uint32_t kRol32(uint32_t x, uint32_t s) {
    return (x << s) | (x >> (32 - s));
}

inline uint32_t kHalfMd4F(uint32_t x, uint32_t y, uint32_t z) {
    return z ^ (x & (y ^ z));
}
inline uint32_t kHalfMd4G(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) + ((x ^ y) & z);
}
inline uint32_t kHalfMd4H(uint32_t x, uint32_t y, uint32_t z) {
    return x ^ y ^ z;
}

}  // namespace

uint32_t kExt4HalfMd4Hash(const char* name, uint32_t nameLen, const uint32_t seed[4]) {
    uint32_t buf[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    if (seed && (seed[0] || seed[1] || seed[2] || seed[3])) {
        buf[0] = seed[0];
        buf[1] = seed[1];
        buf[2] = seed[2];
        buf[3] = seed[3];
    }

    constexpr uint32_t kK1 = 0;
    constexpr uint32_t kK2 = 0x5A827999u;  // 013240474631(8진) == 이 값
    constexpr uint32_t kK3 = 0x6ED9EBA1u;  // 015666365641(8진) == 이 값

    uint32_t remaining = nameLen;
    const char* p = name;
    while (remaining > 0) {
        // str2hashbuf_signed(p, remaining, in, 8) - signed char로 부호 확장.
        // [중요] pad는 커널 원본처럼 "클램프 전" 길이(remaining)로
        // 계산해야 한다 - 32바이트 넘는 이름의 두 번째 청크부터는
        // pad 계산에 쓰는 길이(remaining)와 실제 소비 바이트 수(take)
        // 가 달라진다(첫 32바이트 청크 소비 후 remaining은 그 다음
        // 청크의 "남은 길이"로 갱신되고, pad는 매번 그 시점의
        // remaining으로 다시 계산됨 - 이름이 32바이트 이하인 흔한
        // 경우는 remaining==take라 이 구분이 드러나지 않으므로,
        // 실측 검증(5개 짧은 파일명)이 못 잡아낸 경로 - 커널 소스
        // 재대조로 잡음).
        uint32_t in[8];
        const uint32_t take = remaining > 32 ? 32 : remaining;
        uint32_t pad = (remaining & 0xFFu) | ((remaining & 0xFFu) << 8);
        pad = pad | (pad << 16);
        int slotsLeft = 8;
        uint32_t val = pad;
        for (uint32_t i = 0; i < take; ++i) {
            const kernel::int8_t signedByte = static_cast<kernel::int8_t>(p[i]);
            val = static_cast<uint32_t>(static_cast<kernel::int32_t>(signedByte)) + (val << 8);
            if ((i % 4) == 3) {
                in[8 - slotsLeft] = val;
                val = pad;
                --slotsLeft;
            }
        }
        --slotsLeft;
        if (slotsLeft >= 0) {
            in[8 - 1 - slotsLeft] = val;
            --slotsLeft;
        }
        while (slotsLeft >= 0) {
            in[8 - 1 - slotsLeft] = pad;
            --slotsLeft;
        }

        uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
        // Round 1
        a = kRol32(a + kHalfMd4F(b, c, d) + (in[0] + kK1), 3);
        d = kRol32(d + kHalfMd4F(a, b, c) + (in[1] + kK1), 7);
        c = kRol32(c + kHalfMd4F(d, a, b) + (in[2] + kK1), 11);
        b = kRol32(b + kHalfMd4F(c, d, a) + (in[3] + kK1), 19);
        a = kRol32(a + kHalfMd4F(b, c, d) + (in[4] + kK1), 3);
        d = kRol32(d + kHalfMd4F(a, b, c) + (in[5] + kK1), 7);
        c = kRol32(c + kHalfMd4F(d, a, b) + (in[6] + kK1), 11);
        b = kRol32(b + kHalfMd4F(c, d, a) + (in[7] + kK1), 19);
        // Round 2
        a = kRol32(a + kHalfMd4G(b, c, d) + (in[1] + kK2), 3);
        d = kRol32(d + kHalfMd4G(a, b, c) + (in[3] + kK2), 5);
        c = kRol32(c + kHalfMd4G(d, a, b) + (in[5] + kK2), 9);
        b = kRol32(b + kHalfMd4G(c, d, a) + (in[7] + kK2), 13);
        a = kRol32(a + kHalfMd4G(b, c, d) + (in[0] + kK2), 3);
        d = kRol32(d + kHalfMd4G(a, b, c) + (in[2] + kK2), 5);
        c = kRol32(c + kHalfMd4G(d, a, b) + (in[4] + kK2), 9);
        b = kRol32(b + kHalfMd4G(c, d, a) + (in[6] + kK2), 13);
        // Round 3
        a = kRol32(a + kHalfMd4H(b, c, d) + (in[3] + kK3), 3);
        d = kRol32(d + kHalfMd4H(a, b, c) + (in[7] + kK3), 9);
        c = kRol32(c + kHalfMd4H(d, a, b) + (in[2] + kK3), 11);
        b = kRol32(b + kHalfMd4H(c, d, a) + (in[6] + kK3), 15);
        a = kRol32(a + kHalfMd4H(b, c, d) + (in[1] + kK3), 3);
        d = kRol32(d + kHalfMd4H(a, b, c) + (in[5] + kK3), 9);
        c = kRol32(c + kHalfMd4H(d, a, b) + (in[0] + kK3), 11);
        b = kRol32(b + kHalfMd4H(c, d, a) + (in[4] + kK3), 15);

        buf[0] += a;
        buf[1] += b;
        buf[2] += c;
        buf[3] += d;

        remaining -= take;
        p += take;
        if (take < 32) {
            break;  // str2hashbuf가 이미 나머지를 pad로 채워 처리 완료
        }
    }

    uint32_t hash = buf[1];
    hash &= ~1u;
    return hash;
}

bool kExt4DxRootFindLeafBlock(const uint8_t* rootBlockData, uint32_t blockSize, uint32_t hash,
                                uint32_t* outLeafLogicalBlock) {
    // "." 엔트리(12바이트) + ".." 엔트리(12바이트, recLen은 블록
    // 끝까지지만 실제 구조는 여기서 끝남) 뒤에 DxRootInfo가 온다.
    constexpr uint32_t kDotDotFakeEntriesBytes = 24;
    if (blockSize < kDotDotFakeEntriesBytes + sizeof(DxRootInfo) + sizeof(DxEntry)) {
        return false;
    }
    DxRootInfo info;
    memcpy(&info, rootBlockData + kDotDotFakeEntriesBytes, sizeof(info));
    if (info.indirectLevels != 0 || info.infoLength < sizeof(DxRootInfo)) {
        return false;  // dx_node 중간 레벨 존재 또는 알 수 없는 레이아웃 - v1 범위 밖
    }
    const uint32_t entriesOffset = kDotDotFakeEntriesBytes + info.infoLength;
    if (entriesOffset + sizeof(DxEntry) > blockSize) {
        return false;
    }
    // 첫 dx_entry의 hash 4바이트는 실제로 dx_countlimit{limit;count}.
    uint16_t limit = 0;
    uint16_t count = 0;
    memcpy(&limit, rootBlockData + entriesOffset, sizeof(limit));
    memcpy(&count, rootBlockData + entriesOffset + sizeof(limit), sizeof(count));
    if (count == 0 || count > limit) {
        return false;
    }
    if (entriesOffset + static_cast<uint32_t>(count) * sizeof(DxEntry) > blockSize) {
        return false;
    }
    // 첫 원소(index 0)는 block 필드만 유효(해시 0부터 시작하는 첫
    // 리프) - 나머지는 hash 오름차순 정렬을 전제로 순차 탐색해 hash가
    // 속하는 마지막(<=hash인) 엔트리를 고른다.
    DxEntry first;
    memcpy(&first, rootBlockData + entriesOffset, sizeof(first));
    uint32_t chosenBlock = first.block;
    for (uint16_t i = 1; i < count; ++i) {
        DxEntry entry;
        memcpy(&entry, rootBlockData + entriesOffset + static_cast<uint32_t>(i) * sizeof(DxEntry), sizeof(entry));
        if (entry.hash > hash) {
            break;
        }
        chosenBlock = entry.block;
    }
    *outLeafLogicalBlock = chosenBlock;
    return true;
}

uint32_t kExt4ComputeExtentBlockChecksum(const uint8_t uuid[16], uint32_t inodeNum, uint32_t generation,
                                          const void* extentBlockData, uint32_t blockSize) {
    (void)blockSize;
    const auto* raw = static_cast<const uint8_t*>(extentBlockData);
    ExtentHeader header;
    memcpy(&header, raw, sizeof(header));
    const uint32_t tailOffset =
        static_cast<uint32_t>(sizeof(ExtentHeader)) + static_cast<uint32_t>(header.max) * sizeof(Extent);
    uint32_t seed = kCrc32c(kCrc32c(0xFFFFFFFFu, uuid, 16), &inodeNum, sizeof(inodeNum));
    seed = kCrc32c(seed, &generation, sizeof(generation));
    return kCrc32c(seed, raw, tailOffset);
}

bool kExt4GrowExtentTreeToDepth1(uint8_t block60[kExtentInlineBytes], uint8_t* newBlockData, uint32_t blockSize,
                                  uint64_t newBlockAbs) {
    ExtentHeader oldHeader;
    memcpy(&oldHeader, block60, sizeof(oldHeader));
    if (oldHeader.magic != kExtentMagic || oldHeader.depth != 0) {
        return false;
    }

    // 1) 새 블록에 인라인 영역을 통째로 복사 + 나머지는 0.
    memcpy(newBlockData, block60, kExtentInlineBytes);
    memset(newBlockData + kExtentInlineBytes, 0, blockSize - kExtentInlineBytes);

    // 2) 새 블록 헤더의 max를 블록 전체 용량으로 재설정.
    ExtentHeader newBlockHeader;
    memcpy(&newBlockHeader, newBlockData, sizeof(newBlockHeader));
    newBlockHeader.max = static_cast<uint16_t>(kExt4ExtentBlockMaxEntries(blockSize));
    memcpy(newBlockData, &newBlockHeader, sizeof(newBlockHeader));

    // 3) 첫 익스텐트의 ee_block을 새 인덱스 엔트리의 ei_block으로
    //    이어받는다(엔트리가 하나도 없는 빈 파일에서 성장하는 극단
    //    경우는 0으로 - 실무상 거의 없음, 인라인 리프가 꽉 차려면
    //    이미 최소 1개 이상의 엔트리가 있어야 하므로).
    Extent firstExtent{};
    if (oldHeader.entries > 0) {
        memcpy(&firstExtent, block60 + sizeof(ExtentHeader), sizeof(firstExtent));
    }

    ExtentHeader idxHeader{};
    idxHeader.magic = kExtentMagic;
    idxHeader.entries = 1;
    idxHeader.max = kExtentInlineMaxEntries;
    idxHeader.depth = 1;
    idxHeader.generation = oldHeader.generation;
    memcpy(block60, &idxHeader, sizeof(idxHeader));

    ExtentIdx idx{};
    idx.block = firstExtent.block;
    idx.leafLo = static_cast<uint32_t>(newBlockAbs & 0xFFFFFFFFu);
    idx.leafHi = static_cast<uint16_t>(newBlockAbs >> 32);
    idx.unused = 0;
    memcpy(block60 + sizeof(ExtentHeader), &idx, sizeof(idx));
    // 나머지 36바이트(옛 익스텐트 #2~#4가 있던 자리)는 의도적으로
    // 그대로 둔다 - 실제 리눅스 커널도 지우지 않는다(eh_entries=1이
    // 그 바이트들을 무효로 만들 뿐).
    return true;
}

void kExt4InitDirInode(InodeCore* inode, uint32_t blockSize, uint64_t firstBlock, uint32_t epochSeconds,
                        uint16_t uid, uint16_t gid) {
    memset(inode, 0, sizeof(InodeCore));
    inode->mode = static_cast<uint16_t>(kModeDir | kDefaultDirPerm);
    inode->uid = uid;
    inode->sizeLo = blockSize;
    inode->atime = epochSeconds;
    inode->ctime = epochSeconds;
    inode->mtime = epochSeconds;
    inode->gid = gid;
    inode->linksCount = 2;  // 자신의 "." + 부모 안의 새 엔트리(호출자가 부모 dirent를 추가할 때 셈)
    inode->blocksLo = blockSize / 512;
    inode->flags = kExtentsFl;
    kExt4InitInlineExtentLeaf(inode->block);
    kExt4AppendInlineExtent(inode->block, 0, firstBlock, 1);
    inode->extraIsize = 32;  // 실제 mke2fs 이미지의 관례값과 동일(PN-FE718C87 실측 확인)
}

uint32_t kExt4ComputeSuperblockChecksum(const void* rawSuperblock1024Bytes) {
    // 실제 mkfs.ext4 이미지 2개(서로 다른 크기 8MB/64MB, 볼륨 라벨
    // 유무도 다름)의 s_checksum과 대조해 확인(PN-625E2804) - 그룹
    // 디스크립터/비트맵/inode 세 체크섬과 달리 uuid를 별도 seed로
    // 쓰지 않고 seed=0xFFFFFFFF에서 슈퍼블록 원본 바이트를 그대로
    // 이어붙인다. 체크섬 필드 자체가 슈퍼블록의 맨 끝(오프셋
    // kSuperblockChecksumOffset~1024)이라 잘라서 넘기는 것만으로
    // 자연히 제외되므로 별도로 0을 채워 이어붙이는 단계가 없다.
    return kCrc32c(0xFFFFFFFFu, rawSuperblock1024Bytes, kSuperblockChecksumOffset);
}

bool kJbd2ParseSuperblock(const void* rawBlock, uint32_t blockLen, JournalSuperblockV2* out) {
    if (blockLen < sizeof(JournalSuperblockV2)) {
        return false;
    }
    JournalSuperblockV2 raw;
    memcpy(&raw, rawBlock, sizeof(raw));

    const uint32_t magic = kJbd2Be32(raw.header.magic);
    const uint32_t blockType = kJbd2Be32(raw.header.blockType);
    if (magic != kJbd2Magic) {
        return false;
    }
    if (blockType != kJbd2BlockTypeSuperblockV1 && blockType != kJbd2BlockTypeSuperblockV2) {
        return false;
    }

    out->header.magic = magic;
    out->header.blockType = blockType;
    out->header.sequence = kJbd2Be32(raw.header.sequence);
    out->blockSize = kJbd2Be32(raw.blockSize);
    out->maxLen = kJbd2Be32(raw.maxLen);
    out->first = kJbd2Be32(raw.first);
    out->sequence = kJbd2Be32(raw.sequence);
    out->start = kJbd2Be32(raw.start);
    out->errno_ = kJbd2Be32(raw.errno_);
    out->featureCompat = kJbd2Be32(raw.featureCompat);
    out->featureIncompat = kJbd2Be32(raw.featureIncompat);
    out->featureRoCompat = kJbd2Be32(raw.featureRoCompat);
    memcpy(out->uuid, raw.uuid, sizeof(out->uuid));
    out->nrUsers = kJbd2Be32(raw.nrUsers);
    out->dynSuper = kJbd2Be32(raw.dynSuper);
    out->maxTransaction = kJbd2Be32(raw.maxTransaction);
    out->maxTransData = kJbd2Be32(raw.maxTransData);
    out->checksumType = raw.checksumType;
    out->numFcBlks = kJbd2Be32(raw.numFcBlks);
    out->head = kJbd2Be32(raw.head);
    return true;
}

bool kJbd2ParseCommitHeader(const void* rawBlock, uint32_t blockLen, CommitHeader* out) {
    if (blockLen < sizeof(CommitHeader)) {
        return false;
    }
    CommitHeader raw;
    memcpy(&raw, rawBlock, sizeof(raw));

    const uint32_t magic = kJbd2Be32(raw.header.magic);
    const uint32_t blockType = kJbd2Be32(raw.header.blockType);
    if (magic != kJbd2Magic || blockType != kJbd2BlockTypeCommit) {
        return false;
    }

    out->header.magic = magic;
    out->header.blockType = blockType;
    out->header.sequence = kJbd2Be32(raw.header.sequence);
    out->chksumType = raw.chksumType;
    out->chksumSize = raw.chksumSize;
    for (uint32_t i = 0; i < 8; ++i) {
        out->chksum[i] = kJbd2Be32(raw.chksum[i]);
    }
    // h_commit_sec는 __be64지만 raw 필드 자체를 64비트 통째로 바이트
    // 스왑하는 헬퍼가 없다 - 32비트씩 스왑 후 상위/하위를 맞바꿔
    // 합성한다(x86_64 리틀엔디안 전제 그대로).
    const uint32_t secHi = kJbd2Be32(static_cast<uint32_t>(raw.commitSec & 0xFFFFFFFFu));
    const uint32_t secLo = kJbd2Be32(static_cast<uint32_t>(raw.commitSec >> 32));
    out->commitSec = (static_cast<uint64_t>(secHi) << 32) | secLo;
    out->commitNsec = kJbd2Be32(raw.commitNsec);
    return true;
}

uint32_t kJbd2ParseDescriptorTags(const void* rawBlock, uint32_t blockLen, uint32_t featureIncompat,
                                   DescriptorTag* outTags, uint32_t maxTags) {
    if (blockLen < sizeof(JournalHeader) || maxTags == 0) {
        return 0;
    }
    const uint8_t* base = static_cast<const uint8_t*>(rawBlock);
    JournalHeader header;
    memcpy(&header, base, sizeof(header));
    const uint32_t magic = kJbd2Be32(header.magic);
    const uint32_t blockType = kJbd2Be32(header.blockType);
    if (magic != kJbd2Magic || blockType != kJbd2BlockTypeDescriptor) {
        return 0;
    }

    const bool csumV3 = (featureIncompat & kJbd2FeatureIncompatCsumV3) != 0;
    // CSUM_V2 단독/64BIT 단독처럼 실측 이미지가 없는 조합은 거부(위
    // 헤더 주석 참고) - v1 취급은 관련 비트가 전부 꺼져 있을 때만.
    if (!csumV3 && (featureIncompat & (kJbd2FeatureIncompat64Bit | kJbd2FeatureIncompatCsumV2)) != 0) {
        return 0;
    }
    const uint32_t tagSize = csumV3 ? 16u : 8u;

    uint32_t pos = sizeof(JournalHeader);
    uint32_t count = 0;
    while (pos + tagSize <= blockLen && count < maxTags) {
        const uint8_t* tagPtr = base + pos;
        uint32_t blockNrLow;
        uint32_t flags;
        uint32_t blockNrHigh = 0;
        memcpy(&blockNrLow, tagPtr, 4);
        memcpy(&flags, tagPtr + 4, 4);
        blockNrLow = kJbd2Be32(blockNrLow);
        flags = kJbd2Be32(flags);
        uint32_t checksum = 0;
        if (csumV3) {
            memcpy(&blockNrHigh, tagPtr + 8, 4);
            blockNrHigh = kJbd2Be32(blockNrHigh);
            memcpy(&checksum, tagPtr + 12, 4);
            checksum = kJbd2Be32(checksum);
        }
        outTags[count].blockNr = (static_cast<uint64_t>(blockNrHigh) << 32) | blockNrLow;
        outTags[count].flags = flags;
        outTags[count].checksum = checksum;
        ++count;

        pos += tagSize;
        if ((flags & kJbd2TagFlagSameUuid) == 0) {
            pos += 16;  // UUID(16바이트)가 이 태그 뒤에 따라옴
        }
        if (flags & kJbd2TagFlagLastTag) {
            break;
        }
    }
    return count;
}

uint32_t kJbd2ParseRevokeRecords(const void* rawBlock, uint32_t blockLen, uint32_t headerCount, bool is64Bit,
                                   uint64_t* outBlockNrs, uint32_t maxRecords) {
    const uint32_t recordLen = is64Bit ? 8u : 4u;
    if (headerCount > blockLen) {
        headerCount = blockLen;  // 손상 방어 - 절대 블록 경계를 넘어 읽지 않는다
    }
    const uint8_t* base = static_cast<const uint8_t*>(rawBlock);
    uint32_t offset = sizeof(RevokeHeader);
    uint32_t count = 0;
    while (offset + recordLen <= headerCount && count < maxRecords) {
        uint64_t blockNr;
        if (is64Bit) {
            uint32_t hi, lo;
            memcpy(&hi, base + offset, 4);
            memcpy(&lo, base + offset + 4, 4);
            blockNr = (static_cast<uint64_t>(kJbd2Be32(hi)) << 32) | kJbd2Be32(lo);
        } else {
            uint32_t v;
            memcpy(&v, base + offset, 4);
            blockNr = kJbd2Be32(v);
        }
        outBlockNrs[count] = blockNr;
        ++count;
        offset += recordLen;
    }
    return count;
}

bool kJbd2VerifyBlockTailChecksum(uint32_t csumSeed, const void* rawBlock, uint32_t blockLen) {
    if (blockLen < 4) {
        return false;
    }
    // 원본을 훼손하지 않도록 임시 버퍼에 복사해 체크섬 필드만 0으로
    // 만든 뒤 해시한다(ext4의 다른 metadata_csum류와 동일 관례 -
    // "체크섬 필드 자신만 0, 나머지는 실제 값 그대로").
    uint8_t tmp[4096];
    if (blockLen > sizeof(tmp)) {
        return false;  // v1 상한(현실적 저널 블록 크기 - 4096 초과는 없음)
    }
    memcpy(tmp, rawBlock, blockLen);
    uint32_t provided;
    memcpy(&provided, tmp + blockLen - 4, 4);
    provided = kJbd2Be32(provided);
    memset(tmp + blockLen - 4, 0, 4);
    const uint32_t calculated = kCrc32c(csumSeed, tmp, blockLen);
    return provided == calculated;
}

bool kJbd2VerifyCommitChecksum(uint32_t csumSeed, const void* rawBlock, uint32_t blockLen) {
    if (blockLen < sizeof(CommitHeader)) {
        return false;
    }
    uint8_t tmp[4096];
    if (blockLen > sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, rawBlock, blockLen);
    uint32_t provided;
    memcpy(&provided, tmp + offsetof(CommitHeader, chksum), 4);
    provided = kJbd2Be32(provided);
    memset(tmp + offsetof(CommitHeader, chksum), 0, 4);
    const uint32_t calculated = kCrc32c(csumSeed, tmp, blockLen);
    return provided == calculated;
}

bool kJbd2VerifyTagChecksum(uint32_t csumSeed, uint32_t sequence, const void* blockData, uint32_t blockLen,
                             uint32_t providedChecksum) {
    const uint32_t seqBe = kJbd2Be32(sequence);
    uint32_t seed2 = kCrc32c(csumSeed, &seqBe, sizeof(seqBe));
    const uint32_t calculated = kCrc32c(seed2, blockData, blockLen);
    return calculated == providedChecksum;
}

// blockOffset/blockCount는 ext4 자신의 블록 단위(blockSize_) - 장치의
// LBA(device_->blockSize() 단위)로 변환해 읽는다. libswapfs의
// slot->LBA 변환과 같은 관용구.
static bool readExtBlocksImpl(fs::BlockDevice* device, uint32_t extBlockSize, uint64_t extBlockStart,
                               uint32_t extBlockCount, void* buf) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || extBlockSize % devBlockSize != 0) {
        return false;  // v1은 ext4 블록 크기가 장치 블록 크기의 정확한 배수인 경우만 지원
    }
    const kernel::uint32_t devBlocksPerExtBlock = extBlockSize / devBlockSize;
    const kernel::uint64_t lba = extBlockStart * devBlocksPerExtBlock;
    const kernel::uint32_t count = extBlockCount * devBlocksPerExtBlock;
    return device->readBlocks(lba, count, buf);
}

// [추가, 2026-09-26, PN-BC3A2F5F] readExtBlocksImpl의 쓰기 버전 - 저널
// 리플레이가 복구된 블록을 실제 파일시스템 위치에 되돌려 쓰는 데 쓴다
// (지금까지 Ext4Volume은 읽기 전용이라 이 방향이 필요 없었다).
static bool writeExtBlocksImpl(fs::BlockDevice* device, uint32_t extBlockSize, uint64_t extBlockStart,
                                uint32_t extBlockCount, const void* buf) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || extBlockSize % devBlockSize != 0) {
        return false;
    }
    const kernel::uint32_t devBlocksPerExtBlock = extBlockSize / devBlockSize;
    const kernel::uint64_t lba = extBlockStart * devBlocksPerExtBlock;
    const kernel::uint32_t count = extBlockCount * devBlocksPerExtBlock;
    return device->writeBlocks(lba, count, buf);
}

// slab 버퍼 RAII - libext4/libvfat의 다른 드라이버 파일들과 동일한
// 관례(각 파일에 독립적으로 둔다 - ext4_driver.cpp의 동명 클래스
// 문서 주석 참고, 작은 유틸리티라 공유 헤더로 뽑지 않는 이 코드베이스
// 전반의 관행 그대로). Ext4Volume::mount()는 코루틴이 아니라 평범한
// 함수라 "co_return 어느 경로로도" 같은 이유는 없지만, 이 리플레이
// 함수처럼 여러 곳에서 break/continue로 빠져나가는 긴 루프 안에서
// 수동 alloc/free 쌍을 매번 정확히 맞추는 실수를 막기 위해 그대로
// 재사용한다.
class SlabBuf {
public:
    explicit SlabBuf(uint32_t size)
        : _size(size), _ptr(static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(size))) {}
    ~SlabBuf() {
        if (_ptr) {
            kernel::GenericSlabAllocator::free(_ptr, _size);
        }
    }
    SlabBuf(const SlabBuf&) = delete;
    SlabBuf& operator=(const SlabBuf&) = delete;
    uint8_t* get() const { return _ptr; }
    explicit operator bool() const { return _ptr != nullptr; }

private:
    uint32_t _size;
    uint8_t* _ptr;
};

// [추가, 2026-09-26, PN-BC3A2F5F] 저널 inode(sb.journalInum)의
// InodeCore를 읽는다 - ext4_driver.cpp의 kLocateInode(오프셋 계산)와
// 동일한 산수를 그대로 재현(이 파일은 그 파일과 별개 번역 단위라
// 공유 못 함 - 이 코드베이스의 작은 순수 함수 중복 관례 그대로).
static bool kJbd2LocateJournalInode(fs::BlockDevice* device, const SuperblockCore& sb, const Ext4Volume& volume,
                                     uint32_t groupCount, uint32_t blockSize, InodeCore* outInode) {
    if (sb.journalInum == 0 || sb.inodesPerGroup == 0) {
        return false;
    }
    const uint32_t group = (sb.journalInum - 1) / sb.inodesPerGroup;
    if (group >= groupCount) {
        return false;
    }
    const uint32_t indexInGroup = (sb.journalInum - 1) % sb.inodesPerGroup;
    const uint64_t byteOffsetInTable = static_cast<uint64_t>(indexInGroup) * sb.inodeSize;
    const uint64_t blockOffset = volume.groupInodeTableBlock(group) + byteOffsetInTable / blockSize;
    const uint32_t byteOffsetInBlock = static_cast<uint32_t>(byteOffsetInTable % blockSize);
    const uint32_t blocksNeeded = static_cast<uint32_t>(kCeilDiv(byteOffsetInBlock + sizeof(InodeCore), blockSize));
    SlabBuf buf(blocksNeeded * blockSize);
    if (!buf) {
        return false;
    }
    if (!readExtBlocksImpl(device, blockSize, blockOffset, blocksNeeded, buf.get())) {
        return false;
    }
    memcpy(outInode, buf.get() + byteOffsetInBlock, sizeof(InodeCore));
    return true;
}

// 저널 논리 블록 하나(0..journalSuperblock.maxLen-1, journal inode
// 자신의 파일 안에서의 블록 인덱스)를 읽는다 - depth==0 인라인
// 익스텐트(최대 4개, kExt4ResolveInlineExtent)로만 저널 inode를
// 해석한다(이 v1의 정직한 범위 제한 - 위 ext4.h 문서 주석 참고).
static bool kJbd2ReadJournalBlock(fs::BlockDevice* device, uint32_t blockSize, const uint8_t journalBlock60[60],
                                   uint32_t journalLogicalBlock, void* outBuf) {
    uint64_t physicalBlock = 0;
    if (!kExt4ResolveInlineExtent(journalBlock60, journalLogicalBlock, &physicalBlock)) {
        return false;
    }
    return readExtBlocksImpl(device, blockSize, physicalBlock, 1, outBuf);
}

// [추가, 2026-09-26, PN-BC3A2F5F] 리보크 테이블 - Linux
// jbd2_journal_set_revoke/test_revoke(해시 테이블)와 같은 의미론을
// 훨씬 단순한 "선형 스캔 배열"로 구현한다(항목 수가 저널 전체
// 블록 수(maxLen)를 절대 넘을 수 없어 현실적으로 작다는 전제 -
// libswapfs badPages류 v1 단순화와 동일한 판단, RM-23F4B687 §4).
struct RevokeEntry {
    uint64_t blockNr;
    uint32_t sequence;
};

// tid_gt(sequence, record->sequence) 판정 - Linux는 32비트 순환
// 비교(tid_t wraparound)를 쓰지만, 이 v1은 단일 마운트 세션 안의
// 작은 시퀀스 범위만 다루므로 평범한 정수 비교로 충분하다(현실적인
// 저널 트랜잭션 수가 2^31을 넘을 일이 없음).
static void kJbd2RevokeSet(RevokeEntry* table, uint32_t* count, uint32_t capacity, uint64_t blockNr,
                            uint32_t sequence) {
    for (uint32_t i = 0; i < *count; ++i) {
        if (table[i].blockNr == blockNr) {
            if (sequence > table[i].sequence) {
                table[i].sequence = sequence;
            }
            return;
        }
    }
    if (*count < capacity) {
        table[*count].blockNr = blockNr;
        table[*count].sequence = sequence;
        ++(*count);
    }
}

// jbd2_journal_test_revoke와 동일한 의미론: sequence(이 블록을 다시
// 쓰려는 트랜잭션 번호)가 리보크 기록의 sequence보다 크면 리보크
// 이후에 다시 쓰인 것이므로 리플레이해야 한다(리보크 아님) - 작거나
// 같으면 리보크된 것으로 취급해 버린다.
static bool kJbd2RevokeTest(const RevokeEntry* table, uint32_t count, uint64_t blockNr, uint32_t sequence) {
    for (uint32_t i = 0; i < count; ++i) {
        if (table[i].blockNr == blockNr) {
            return !(sequence > table[i].sequence);
        }
    }
    return false;
}

// [추가, 2026-09-26, PN-BC3A2F5F] jbd2 저널 리플레이 본체 - Linux
// fs/jbd2/recovery.c의 do_one_pass()를 그대로 재현한 3-pass 구조
// (SCAN → REVOKE → REPLAY, 위 ext4.h의 jbd2 절 문서 주석 참고).
// `Ext4Volume::mount()`가 그룹 디스크립터 테이블까지 다 읽어 자기
// 상태(groupInodeTableBlock() 등)가 완전히 준비된 뒤 호출한다(저널
// inode 자체를 그 그룹 디스크립터로 찾아야 하므로) - 진짜
// kernel::Task 컨텍스트의 1회 준비 단계라 동기 I/O가 안전하다는
// 근거는 mount() 자신과 동일. 성공(리플레이할 게 없었거나 끝까지
// 무사히 마침)이면 true, 저널이 없거나 읽는 도중 실패하거나
// CSUM_V2/64BIT 단독처럼 이 v1이 모르는 조합을 만나면 false(호출자인
// mount()가 안전하게 마운트를 거부한다).
static bool kJbd2ReplayJournal(fs::BlockDevice* device, const SuperblockCore& sb, const Ext4Volume& volume,
                                uint32_t groupCount, uint32_t fsBlockSize) {
    if ((sb.featureCompat & kCompatHasJournal) == 0) {
        return false;  // 저널 자체가 없음 - 리플레이 불가능, 정직하게 거부
    }
    InodeCore journalInode;
    if (!kJbd2LocateJournalInode(device, sb, volume, groupCount, fsBlockSize, &journalInode)) {
        return false;
    }
    if ((journalInode.flags & kExtentsFl) == 0) {
        return false;  // v1: 레거시 간접 블록 저널 미지원(정직한 실패)
    }

    SlabBuf sbBuf(fsBlockSize);
    if (!sbBuf || !kJbd2ReadJournalBlock(device, fsBlockSize, journalInode.block, 0, sbBuf.get())) {
        return false;
    }
    JournalSuperblockV2 js;
    if (!kJbd2ParseSuperblock(sbBuf.get(), fsBlockSize, &js)) {
        return false;
    }
    if (js.blockSize != fsBlockSize) {
        return false;  // v1 단순화 - 저널 자신의 블록 크기가 fs 블록 크기와 다른 조합은 실측 안 함
    }
    if (js.start == 0) {
        return true;  // 저널이 비어 있음(클린 상태) - 리플레이할 트랜잭션 없음
    }
    const bool csumV3 = (js.featureIncompat & kJbd2FeatureIncompatCsumV3) != 0;
    if (!csumV3 && (js.featureIncompat & (kJbd2FeatureIncompat64Bit | kJbd2FeatureIncompatCsumV2)) != 0) {
        return false;  // kJbd2ParseDescriptorTags와 동일한 이유의 정직한 실패
    }
    const bool is64Bit = (js.featureIncompat & kJbd2FeatureIncompat64Bit) != 0;
    const uint32_t csumSeed = csumV3 ? kJbd2ComputeCsumSeed(js.uuid) : 0;

    const uint32_t maxTagsPerBlock = fsBlockSize / 8u;      // 최소 태그 크기(v1, 8바이트) 기준 상한
    const uint32_t maxRevokesPerBlock = fsBlockSize / 4u;   // 최소 레코드 크기(4바이트) 기준 상한
    SlabBuf blockBuf(fsBlockSize);
    SlabBuf dataBuf(fsBlockSize);
    auto* tags = static_cast<DescriptorTag*>(kernel::GenericSlabAllocator::alloc(maxTagsPerBlock * sizeof(DescriptorTag)));
    auto* revokeBlockNrs =
        static_cast<uint64_t*>(kernel::GenericSlabAllocator::alloc(maxRevokesPerBlock * sizeof(uint64_t)));
    auto* revokeTable =
        static_cast<RevokeEntry*>(kernel::GenericSlabAllocator::alloc(js.maxLen * sizeof(RevokeEntry)));
    bool allocOk = blockBuf && dataBuf && tags && revokeBlockNrs && revokeTable;
    uint32_t revokeCount = 0;

    uint32_t endTransaction = 0;
    bool ok = allocOk;

    auto wrap = [&](uint32_t& b) {
        if (b >= js.maxLen) {
            b -= (js.maxLen - js.first);
        }
    };

    for (int passIndex = 0; passIndex < 3 && ok; ++passIndex) {
        const bool isScan = (passIndex == 0);
        const bool isReplay = (passIndex == 2);

        uint32_t nextLogBlock = js.start;
        uint32_t nextCommitId = js.sequence;

        while (true) {
            if (!isScan && nextCommitId >= endTransaction) {
                break;
            }
            if (!kJbd2ReadJournalBlock(device, fsBlockSize, journalInode.block, nextLogBlock, blockBuf.get())) {
                ok = false;
                break;
            }
            JournalHeader hdr;
            memcpy(&hdr, blockBuf.get(), sizeof(hdr));
            if (kJbd2Be32(hdr.magic) != kJbd2Magic) {
                break;  // 로그의 실제 끝
            }
            const uint32_t blockType = kJbd2Be32(hdr.blockType);
            const uint32_t sequence = kJbd2Be32(hdr.sequence);
            if (sequence != nextCommitId) {
                break;  // 기대한 트랜잭션 번호가 아님 - 여기서 로그가 끝난 것으로 간주
            }

            ++nextLogBlock;
            wrap(nextLogBlock);

            if (blockType == kJbd2BlockTypeDescriptor) {
                if (isScan && csumV3 && !kJbd2VerifyBlockTailChecksum(csumSeed, blockBuf.get(), fsBlockSize)) {
                    break;  // 손상된 디스크립터 블록 - 로그 끝으로 간주(v1 단순화)
                }
                const uint32_t tagCount =
                    kJbd2ParseDescriptorTags(blockBuf.get(), fsBlockSize, js.featureIncompat, tags, maxTagsPerBlock);
                if (!isReplay) {
                    nextLogBlock += tagCount;
                    wrap(nextLogBlock);
                    continue;
                }
                for (uint32_t t = 0; t < tagCount && ok; ++t) {
                    if (!kJbd2ReadJournalBlock(device, fsBlockSize, journalInode.block, nextLogBlock, dataBuf.get())) {
                        ok = false;
                        break;
                    }
                    ++nextLogBlock;
                    wrap(nextLogBlock);
                    const uint64_t targetBlockNr = tags[t].blockNr;
                    if (kJbd2RevokeTest(revokeTable, revokeCount, targetBlockNr, nextCommitId)) {
                        continue;  // 리보크됨 - 이 사본은 버리고 다음 태그로
                    }
                    if (csumV3 && !kJbd2VerifyTagChecksum(csumSeed, nextCommitId, dataBuf.get(), fsBlockSize,
                                                           tags[t].checksum)) {
                        ok = false;  // 데이터 블록 손상 - 안전하게 전체 리플레이 중단
                        break;
                    }
                    if (tags[t].flags & kJbd2TagFlagEscape) {
                        const uint32_t realMagicBe = kJbd2Be32(kJbd2Magic);
                        memcpy(dataBuf.get(), &realMagicBe, sizeof(realMagicBe));
                    }
                    if (!writeExtBlocksImpl(device, fsBlockSize, targetBlockNr, 1, dataBuf.get())) {
                        ok = false;
                        break;
                    }
                }
                continue;
            }

            if (blockType == kJbd2BlockTypeCommit) {
                if (isScan) {
                    if (csumV3 && !kJbd2VerifyCommitChecksum(csumSeed, blockBuf.get(), fsBlockSize)) {
                        break;  // 커밋 체크섬 불일치 - 이 트랜잭션은 미완결로 간주
                    }
                }
                ++nextCommitId;
                continue;
            }

            if (blockType == kJbd2BlockTypeRevoke) {
                if (isReplay) {
                    continue;  // REPLAY 패스는 리보크 블록 자체를 건너뜀(REVOKE 패스가 이미 처리)
                }
                if (isScan) {
                    if (csumV3 && !kJbd2VerifyBlockTailChecksum(csumSeed, blockBuf.get(), fsBlockSize)) {
                        break;  // 손상된 리보크 블록 - 로그 끝으로 간주
                    }
                    continue;  // SCAN은 태그 파싱까지 필요 없음 - REVOKE 패스에서 실제로 채운다
                }
                // REVOKE 패스
                RevokeHeader rh;
                memcpy(&rh, blockBuf.get(), sizeof(rh));
                const uint32_t byteCount = kJbd2Be32(rh.count);
                const uint32_t n = kJbd2ParseRevokeRecords(blockBuf.get(), fsBlockSize, byteCount, is64Bit,
                                                            revokeBlockNrs, maxRevokesPerBlock);
                for (uint32_t i = 0; i < n; ++i) {
                    kJbd2RevokeSet(revokeTable, &revokeCount, js.maxLen, revokeBlockNrs[i], nextCommitId);
                }
                continue;
            }

            break;  // 인식 못 한 블록 타입 - 로그 끝
        }

        if (isScan) {
            endTransaction = nextCommitId;
        }
    }

    // [추가, 2026-09-26, PN-BC3A2F5F] 성공적으로 리플레이를 마쳤으면
    // Linux의 jbd2_journal_recover() 마지막 단계(저널 슈퍼블록 리셋 +
    // ext4 슈퍼블록의 INCOMPAT_RECOVER 비트 해제)를 그대로 재현한다 -
    // 이걸 안 하면 다음 마운트가 "여전히 리플레이 필요"로 보고 또
    // 리플레이를 시도하는데, 이미 정상 파일시스템 위로 다시
    // 리플레이하면 오히려 최신 데이터를 옛 저널 사본으로 덮어써
    // 손상시킬 위험이 있다(실측으로 재확인: 클리어 안 하면 e2fsck -n/
    // 읽기전용 재마운트가 계속 "recovering journal"로 본다).
    if (ok) {
        js.start = 0;
        js.sequence = endTransaction + 1;
        uint32_t rawStart = kJbd2Be32(js.start);
        uint32_t rawSequence = kJbd2Be32(js.sequence);
        memcpy(sbBuf.get() + offsetof(JournalSuperblockV2, start), &rawStart, sizeof(rawStart));
        memcpy(sbBuf.get() + offsetof(JournalSuperblockV2, sequence), &rawSequence, sizeof(rawSequence));
        // [실측으로 발견, 2026-09-26] CSUM_V3 저널은 저널 슈퍼블록
        // 자신도 체크섬으로 보호된다(`journal_superblock_t::s_checksum`,
        // 오프셋 0xFC=252, Linux `jbd2_superblock_csum()` - 그 필드
        // 자신만 0으로 두고 정확히 1024바이트(sizeof(journal_superblock_t),
        // 저널 블록 크기가 더 커도 이 1024바이트만) crc32c) - 이걸
        // 안 고치면 s_start/s_sequence를 바꾼 다음 e2fsck가 즉시
        // "journal superblock is corrupt"로 거부한다(실측 재현).
        if (csumV3 && fsBlockSize >= 1024) {
            constexpr uint32_t kJournalSbChecksumOffset = 252;
            constexpr uint32_t kJournalSbChecksumSize = 1024;
            uint32_t zero = 0;
            memcpy(sbBuf.get() + kJournalSbChecksumOffset, &zero, sizeof(zero));
            const uint32_t newChecksum = kCrc32c(0xFFFFFFFFu, sbBuf.get(), kJournalSbChecksumSize);
            const uint32_t newChecksumBe = kJbd2Be32(newChecksum);
            memcpy(sbBuf.get() + kJournalSbChecksumOffset, &newChecksumBe, sizeof(newChecksumBe));
        }
        uint64_t journalSbPhysical = 0;
        if (kExt4ResolveInlineExtent(journalInode.block, 0, &journalSbPhysical)) {
            ok = writeExtBlocksImpl(device, fsBlockSize, journalSbPhysical, 1, sbBuf.get());
        } else {
            ok = false;
        }
    }
    if (ok) {
        const kernel::uint32_t devBlockSize = device->blockSize();
        const kernel::uint64_t sbStartBlock = kSuperblockOffset / devBlockSize;
        const kernel::uint32_t sbByteOffsetInBlock = static_cast<kernel::uint32_t>(kSuperblockOffset % devBlockSize);
        const kernel::uint32_t sbBlocksNeeded =
            static_cast<kernel::uint32_t>(kCeilDiv(sbByteOffsetInBlock + sizeof(SuperblockCore), devBlockSize));
        SlabBuf esbBuf(sbBlocksNeeded * devBlockSize);
        if (!esbBuf || !device->readBlocks(sbStartBlock, sbBlocksNeeded, esbBuf.get())) {
            ok = false;
        } else {
            uint32_t featureIncompat;
            const uint32_t fiOffset = sbByteOffsetInBlock + static_cast<uint32_t>(offsetof(SuperblockCore, featureIncompat));
            memcpy(&featureIncompat, esbBuf.get() + fiOffset, sizeof(featureIncompat));
            featureIncompat &= ~kIncompatRecover;
            memcpy(esbBuf.get() + fiOffset, &featureIncompat, sizeof(featureIncompat));
            // [실측으로 발견, 2026-09-26] RO_COMPAT_METADATA_CSUM
            // 볼륨은 슈퍼블록 자신도 체크섬으로 보호된다 - featureIncompat
            // 만 고치고 s_checksum을 그대로 두면 "Superblock checksum
            // does not match superblock"로 그 즉시 손상 취급된다(다음
            // 마운트 시도로 바로 재현됨). featureRoCompat도 슈퍼블록
            // 안에 있으므로 이 esbBuf에서 그대로 다시 읽는다.
            uint32_t featureRoCompat;
            const uint32_t frOffset = sbByteOffsetInBlock + static_cast<uint32_t>(offsetof(SuperblockCore, featureRoCompat));
            memcpy(&featureRoCompat, esbBuf.get() + frOffset, sizeof(featureRoCompat));
            if (featureRoCompat & kRoCompatMetadataCsum) {
                const uint32_t newChecksum = kExt4ComputeSuperblockChecksum(esbBuf.get() + sbByteOffsetInBlock);
                memcpy(esbBuf.get() + sbByteOffsetInBlock + kSuperblockChecksumOffset, &newChecksum,
                       sizeof(newChecksum));
            }
            ok = device->writeBlocks(sbStartBlock, sbBlocksNeeded, esbBuf.get());
        }
    }

    if (tags) {
        kernel::GenericSlabAllocator::free(tags, maxTagsPerBlock * sizeof(DescriptorTag));
    }
    if (revokeBlockNrs) {
        kernel::GenericSlabAllocator::free(revokeBlockNrs, maxRevokesPerBlock * sizeof(uint64_t));
    }
    if (revokeTable) {
        kernel::GenericSlabAllocator::free(revokeTable, js.maxLen * sizeof(RevokeEntry));
    }
    return ok;
}

bool Ext4Volume::mount(fs::BlockDevice* device) {
    if (!device) {
        return false;
    }

    // 1) 슈퍼블록(오프셋 1024, sizeof(SuperblockCore)바이트) - 장치
    // 블록 크기가 뭐든 이 오프셋을 담는 만큼만 슬랩에서 확보한다.
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0) {
        return false;
    }
    const kernel::uint64_t sbStartBlock = kSuperblockOffset / devBlockSize;
    const kernel::uint32_t sbByteOffsetInBlock = static_cast<kernel::uint32_t>(kSuperblockOffset % devBlockSize);
    const kernel::uint32_t sbBlocksNeeded =
        static_cast<kernel::uint32_t>(kCeilDiv(sbByteOffsetInBlock + sizeof(SuperblockCore), devBlockSize));
    auto* sbBuf = static_cast<kernel::uint8_t*>(kernel::GenericSlabAllocator::alloc(sbBlocksNeeded * devBlockSize));
    if (!sbBuf) {
        return false;
    }
    if (!device->readBlocks(sbStartBlock, sbBlocksNeeded, sbBuf)) {
        kernel::GenericSlabAllocator::free(sbBuf, sbBlocksNeeded * devBlockSize);
        return false;
    }
    memcpy(&sb_, sbBuf + sbByteOffsetInBlock, sizeof(sb_));
    kernel::GenericSlabAllocator::free(sbBuf, sbBlocksNeeded * devBlockSize);

    // 2) 매직/기능 플래그 판별 - 불일치는 "이 포맷 아님"(다음 드라이버
    // 시도 경로), 필수 incompat 미충족은 "이 포맷이지만 지원 안 함".
    if (sb_.magic != kMagic) {
        return false;
    }
    if (sb_.revLevel != 1) {
        return false;  // EXT4_DYNAMIC_REV 미만 - firstIno/inodeSize 등 확장 필드 무효, v1 미지원
    }
    // [갱신, 2026-09-23, PN-E3629BE9] §2.1은 익스텐트도 볼륨 레벨에서
    // 필수로 요구했으나(레거시 간접 블록 이미지는 이 필드 주석이 이미
    // "§2.2 후속"으로 예고해 둔 대로), 이제 Ext4Driver::onExec()이
    // inode 개별 EXTENTS_FL 유무를 그때그때 판별해 두 형식을 모두
    // 읽을 수 있으므로 볼륨 레벨에서는 파일타입만 필수로 남긴다 -
    // 순수 ext2/ext3 이미지(INCOMPAT_EXTENTS 비트 자체가 없는 볼륨)
    // 도 이제 마운트 가능.
    constexpr uint32_t kRequiredIncompat = kIncompatFiletype;
    if ((sb_.featureIncompat & kRequiredIncompat) != kRequiredIncompat) {
        return false;  // 파일타입(dirent 안의 fileType 바이트) 필수(§2.1)
    }
    if ((sb_.featureIncompat & ~kSupportedIncompatMask) != 0) {
        return false;  // 이 v1이 모르는 incompat 비트 - 안전하게 마운트 거부
    }
    // [갱신, 2026-09-26, PN-BC3A2F5F] "비정상 언마운트면 무조건 거부"
    // 하던 v1 안전장치를 저널 리플레이로 대체한다 - 저널 inode를
    // 찾으려면 그룹 디스크립터 테이블(아래 3단계)까지 다 읽어야 하므로,
    // 여기서는 플래그만 세우고 계속 진행한다(실제 리플레이 시도는
    // 이 함수 끝, GDT 설정이 끝난 뒤). **[정정, 2026-09-26]** 신호는
    // `state`의 kStateValidFs가 아니라 `featureIncompat`의
    // kIncompatRecover다 - 위 ext4.h 문서 주석 참고(실측으로 바로잡음).
    const bool needsRecovery = (sb_.featureIncompat & kIncompatRecover) != 0;
    if (sb_.inodesPerGroup == 0 || sb_.blocksPerGroup == 0 || sb_.inodeSize == 0) {
        return false;
    }

    blockSize_ = 1024u << sb_.logBlockSize;
    if (blockSize_ % devBlockSize != 0) {
        return false;  // v1 제약(위 readExtBlocksImpl과 동일한 전제)
    }
    // [갱신, PN-59C253E9] blocksCountLo만 쓰면 INCOMPAT_64BIT 볼륨의
    // 실제 그룹 수를 과소평가해(그 결과 위 3)단계의 hi 필드 안전장치가
    // 못 보는 그룹이 생겨) 위험하므로, kExt4Combine64()로 합성한 실제
    // 64비트 블록 수를 쓴다. 결과 그룹 수 자체가 이 커널의 현실적
    // 볼륨 규모를 벗어나면(예: 손상된 슈퍼블록이 터무니없는 값을
    // 담은 경우) 거대한 슬랩 할당을 시도하지 않고 안전하게 거부한다.
    const kernel::uint64_t totalBlocks64 = kExt4Combine64(sb_.featureIncompat, sb_.blocksCountLo, sb_.blocksCountHi);
    const kernel::uint64_t groupCount64 = kCeilDiv(totalBlocks64, sb_.blocksPerGroup);
    constexpr kernel::uint64_t kMaxSaneGroupCount = 1u << 20;  // 넉넉한 상한(libswapfs의 "현실적 볼륨 규모" 전제와 동일한 취지)
    if (groupCount64 == 0 || groupCount64 > kMaxSaneGroupCount) {
        return false;
    }
    groupCount_ = static_cast<kernel::uint32_t>(groupCount64);

    // 3) 그룹 디스크립터 테이블 - firstDataBlock+1 블록부터 groupCount_
    // 개 연속 배치. 온디스크 stride는 INCOMPAT_64BIT 여부에 따라
    // 32바이트(GroupDesc32) 또는 64바이트(GroupDesc64).
    // [갱신, 2026-09-25, PN-36747363] 이전엔(PN-59C253E9) 64바이트로
    // 읽은 각 엔트리의 앞 32바이트만 뽑아 촘촘한 GroupDesc32[]로
    // 압축해 저장했다 - hi 필드를 버리는 손실 변환이라 4G 블록을
    // 실제로 초과하는 그룹은 하나라도 있으면 안전을 위해 마운트 자체를
    // 거부해야 했다. 이제 원본 stride 그대로(압축 없이) 보관하고,
    // `groupInodeTableBlock()`이 필요할 때마다 hi 필드까지 합성한
    // 진짜 64비트 주소를 계산해 주므로 그 안전장치가 필요 없어졌다.
    const kernel::uint32_t onDiskDescSize =
        (sb_.featureIncompat & kIncompat64Bit) ? sizeof(GroupDesc64) : sizeof(GroupDesc32);
    const kernel::uint64_t gdtStartBlock = sb_.firstDataBlock + 1;
    const kernel::uint64_t gdtBytes = static_cast<kernel::uint64_t>(groupCount_) * onDiskDescSize;
    const kernel::uint32_t gdtBlocks = static_cast<kernel::uint32_t>(kCeilDiv(gdtBytes, blockSize_));
    auto* gdtRawBuf = static_cast<kernel::uint8_t*>(kernel::GenericSlabAllocator::alloc(gdtBlocks * blockSize_));
    if (!gdtRawBuf) {
        return false;
    }
    if (!readExtBlocksImpl(device, blockSize_, gdtStartBlock, gdtBlocks, gdtRawBuf)) {
        kernel::GenericSlabAllocator::free(gdtRawBuf, gdtBlocks * blockSize_);
        return false;
    }
    groupDescsRaw_ = gdtRawBuf;
    groupDescStride_ = onDiskDescSize;
    is64Bit_ = (onDiskDescSize == sizeof(GroupDesc64));

    device_ = device;

    // 4) [추가, 2026-09-26, PN-BC3A2F5F] 비정상 언마운트였다면 이제
    // (그룹 디스크립터까지 다 준비된 뒤) 저널 리플레이를 실제로
    // 시도한다 - 저널이 없거나(HAS_JOURNAL 미설정), 리플레이 도중
    // 손상/미지원 조합을 만나면 정직하게 마운트를 거부한다(예전
    // v1 안전장치와 동일한 실패 경로, 다만 이제 "시도라도 해 봤다"는
    // 차이). 성공하면(리플레이할 게 없었던 경우 포함) 계속 진행 -
    // 이 시점 이후 mount()의 나머지 호출자(Ext4Driver 등)는 파일시스템이
    // 이미 클린 상태라고 가정해도 된다.
    if (needsRecovery && !kJbd2ReplayJournal(device, sb_, *this, groupCount_, blockSize_)) {
        kernel::GenericSlabAllocator::free(groupDescsRaw_, gdtBlocks * blockSize_);
        groupDescsRaw_ = nullptr;
        device_ = nullptr;
        return false;
    }
    return true;
}

uint64_t Ext4Volume::groupInodeTableBlock(uint32_t group) const {
    if (group >= groupCount_ || !groupDescsRaw_) {
        return 0;
    }
    // GroupDesc64의 앞 32바이트가 GroupDesc32와 완전히 동일한 레이아웃
    // 임이 이미 실측 확인됐으므로(ext4.h 주석 참고), stride가 뭐든
    // inodeTableLo는 항상 같은 오프셋에서 읽을 수 있다.
    const uint8_t* descPtr = groupDescsRaw_ + static_cast<uint64_t>(group) * groupDescStride_;
    uint32_t lo;
    memcpy(&lo, descPtr + offsetof(GroupDesc32, inodeTableLo), sizeof(lo));
    uint32_t hi = 0;
    if (is64Bit_) {
        memcpy(&hi, descPtr + offsetof(GroupDesc64, inodeTableHi), sizeof(hi));
    }
    return kExt4Combine64(sb_.featureIncompat, lo, hi);
}

uint64_t Ext4Volume::groupBlockBitmapBlock(uint32_t group) const {
    if (group >= groupCount_ || !groupDescsRaw_) {
        return 0;
    }
    const uint8_t* descPtr = groupDescsRaw_ + static_cast<uint64_t>(group) * groupDescStride_;
    uint32_t lo;
    memcpy(&lo, descPtr + offsetof(GroupDesc32, blockBitmapLo), sizeof(lo));
    uint32_t hi = 0;
    if (is64Bit_) {
        memcpy(&hi, descPtr + offsetof(GroupDesc64, blockBitmapHi), sizeof(hi));
    }
    return kExt4Combine64(sb_.featureIncompat, lo, hi);
}

uint64_t Ext4Volume::groupInodeBitmapBlock(uint32_t group) const {
    if (group >= groupCount_ || !groupDescsRaw_) {
        return 0;
    }
    const uint8_t* descPtr = groupDescsRaw_ + static_cast<uint64_t>(group) * groupDescStride_;
    uint32_t lo;
    memcpy(&lo, descPtr + offsetof(GroupDesc32, inodeBitmapLo), sizeof(lo));
    uint32_t hi = 0;
    if (is64Bit_) {
        memcpy(&hi, descPtr + offsetof(GroupDesc64, inodeBitmapHi), sizeof(hi));
    }
    return kExt4Combine64(sb_.featureIncompat, lo, hi);
}

}  // namespace ext4
