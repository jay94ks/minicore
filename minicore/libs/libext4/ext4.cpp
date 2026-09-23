#include "ext4.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace ext4 {

namespace {

constexpr uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

}  // namespace

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
