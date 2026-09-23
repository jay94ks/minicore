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

uint16_t kExt4ComputeBitmapChecksum(const uint8_t uuid[16], const void* bitmapData, uint32_t bitCount) {
    // 실제 mkfs.ext4 -O metadata_csum 이미지(단일 그룹 + "마지막 그룹이
    // blocksPerGroup보다 작은" 3그룹 258MB 구성 둘 다)의 bg_block_bitmap_
    // csum_lo/bg_inode_bitmap_csum_lo와 1바이트씩 대조해 확인(PN-625E2804) -
    // 그룹 디스크립터 체크섬과 달리 그룹 번호를 이어붙이지 않고, uuid
    // 시드 바로 다음에 비트맵 바이트를 이어붙인다. 해시 길이는 호출자가
    // 넘긴 bitCount(항상 볼륨 전체의 명목상 blocksPerGroup/inodesPerGroup -
    // 그 그룹의 실제 유효 비트 수가 아님, 위 ext4.h 선언부 주석 참고)를
    // 8로 나눠 올림한 바이트 수.
    const uint32_t byteLen = (bitCount + 7u) / 8u;
    const uint32_t crc = kCrc32c(kCrc32c(0xFFFFFFFFu, uuid, 16), bitmapData, byteLen);
    return static_cast<uint16_t>(crc & 0xFFFFu);
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
    groupCount_ = static_cast<kernel::uint32_t>(kCeilDiv(sb_.blocksCountLo, sb_.blocksPerGroup));
    if (groupCount_ == 0) {
        return false;
    }

    // 3) 그룹 디스크립터 테이블 - firstDataBlock+1 블록부터 groupCount_
    // 개(32바이트 고정, INCOMPAT_64BIT 미지원 - §2.2 후속) 연속 배치.
    const kernel::uint64_t gdtStartBlock = sb_.firstDataBlock + 1;
    const kernel::uint64_t gdtBytes = static_cast<kernel::uint64_t>(groupCount_) * sizeof(GroupDesc32);
    const kernel::uint32_t gdtBlocks = static_cast<kernel::uint32_t>(kCeilDiv(gdtBytes, blockSize_));
    auto* gdtBuf = static_cast<kernel::uint8_t*>(kernel::GenericSlabAllocator::alloc(gdtBlocks * blockSize_));
    if (!gdtBuf) {
        return false;
    }
    if (!readExtBlocksImpl(device, blockSize_, gdtStartBlock, gdtBlocks, gdtBuf)) {
        kernel::GenericSlabAllocator::free(gdtBuf, gdtBlocks * blockSize_);
        return false;
    }
    groupDescs_ = reinterpret_cast<GroupDesc32*>(gdtBuf);

    device_ = device;
    return true;
}

}  // namespace ext4
