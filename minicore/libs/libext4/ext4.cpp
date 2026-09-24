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
    return kExt4ToggleBitInGroup(uuid, groupNum, inodesPerGroup, featureRoCompat, is64Bit, bitmapBuf,
                                  groupDescBuf, offsetof(GroupDesc32, freeInodesCountLo),
                                  offsetof(GroupDesc64, freeInodesCountHi), offsetof(GroupDesc32, inodeBitmapCsumLo),
                                  offsetof(GroupDesc64, inodeBitmapCsumHi), /*allocating=*/true, outRelIndex);
}

bool kExt4FreeInodeInGroup(const uint8_t uuid[16], uint32_t groupNum, uint32_t inodesPerGroup,
                            uint32_t featureRoCompat, bool is64Bit, uint8_t* bitmapBuf,
                            uint8_t* groupDescBuf, uint32_t relIndex) {
    return kExt4ToggleBitInGroup(uuid, groupNum, inodesPerGroup, featureRoCompat, is64Bit, bitmapBuf,
                                  groupDescBuf, offsetof(GroupDesc32, freeInodesCountLo),
                                  offsetof(GroupDesc64, freeInodesCountHi), offsetof(GroupDesc32, inodeBitmapCsumLo),
                                  offsetof(GroupDesc64, inodeBitmapCsumHi), /*allocating=*/false, &relIndex);
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
        if (csumV3) {
            memcpy(&blockNrHigh, tagPtr + 8, 4);
            blockNrHigh = kJbd2Be32(blockNrHigh);
            // tagPtr+12의 4바이트 checksum(crc32c)은 아직 검증하지
            // 않는다 - 순수 태그 목록 추출 범위 밖(리플레이 세션 몫).
        }
        outTags[count].blockNr = (static_cast<uint64_t>(blockNrHigh) << 32) | blockNrLow;
        outTags[count].flags = flags;
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
    if ((sb_.state & kStateValidFs) == 0) {
        return false;  // 비정상 언마운트 - 저널 리플레이는 §2.2 후속, v1은 항상 거부
    }
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
