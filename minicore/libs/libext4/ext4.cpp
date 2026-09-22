#include "ext4.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace ext4 {

namespace {

constexpr uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

bool isDirMode(uint16_t mode) { return (mode & 0xF000u) == 0x4000u; }

}  // namespace

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
    constexpr uint32_t kRequiredIncompat = kIncompatExtents | kIncompatFiletype;
    if ((sb_.featureIncompat & kRequiredIncompat) != kRequiredIncompat) {
        return false;  // 익스텐트/파일타입 필수(§2.1) - 레거시 간접 블록 이미지는 §2.2 후속
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

bool Ext4Volume::readInodeStruct(uint32_t inodeNum, InodeCore* out) {
    if (inodeNum == 0) {
        return false;
    }
    const kernel::uint32_t group = (inodeNum - 1) / sb_.inodesPerGroup;
    if (group >= groupCount_) {
        return false;
    }
    const kernel::uint32_t indexInGroup = (inodeNum - 1) % sb_.inodesPerGroup;
    const kernel::uint64_t byteOffsetInTable = static_cast<kernel::uint64_t>(indexInGroup) * sb_.inodeSize;
    const kernel::uint64_t blockOffset = groupDescs_[group].inodeTableLo + byteOffsetInTable / blockSize_;
    const kernel::uint32_t byteOffsetInBlock = static_cast<kernel::uint32_t>(byteOffsetInTable % blockSize_);
    const kernel::uint32_t blocksNeeded =
        static_cast<kernel::uint32_t>(kCeilDiv(byteOffsetInBlock + sizeof(InodeCore), blockSize_));

    auto* buf = static_cast<kernel::uint8_t*>(kernel::GenericSlabAllocator::alloc(blocksNeeded * blockSize_));
    if (!buf) {
        return false;
    }
    if (!readExtBlocksImpl(device_, blockSize_, blockOffset, blocksNeeded, buf)) {
        kernel::GenericSlabAllocator::free(buf, blocksNeeded * blockSize_);
        return false;
    }
    memcpy(out, buf + byteOffsetInBlock, sizeof(InodeCore));
    kernel::GenericSlabAllocator::free(buf, blocksNeeded * blockSize_);
    return true;
}

bool Ext4Volume::resolveExtentNode(const uint8_t* nodeBytes, uint32_t logicalBlock, uint32_t depthBudget,
                                    uint64_t* outPhysicalBlock) {
    const auto* header = reinterpret_cast<const ExtentHeader*>(nodeBytes);
    if (header->magic != kExtentMagic) {
        return false;  // 손상됐거나 EXTENTS_FL 없이 잘못 해석한 경우
    }
    const uint8_t* entries = nodeBytes + sizeof(ExtentHeader);

    if (header->depth == 0) {
        for (uint16_t i = 0; i < header->entries; ++i) {
            const auto* ext = reinterpret_cast<const Extent*>(entries + i * sizeof(Extent));
            const uint32_t len = ext->len & ~kExtentUninitLenBit;  // [단순화] uninitialized 여부는 v1이 구분 안 함 - §5 미결
            if (logicalBlock >= ext->block && logicalBlock < ext->block + len) {
                const uint64_t physicalStart =
                    (static_cast<uint64_t>(ext->startHi) << 32) | ext->startLo;
                *outPhysicalBlock = physicalStart + (logicalBlock - ext->block);
                return true;
            }
        }
        return false;  // 매핑된 익스텐트 없음 - 파일 구멍(hole)으로 취급(호출부가 0으로 채움)
    }

    if (depthBudget == 0) {
        return false;  // 손상된 트리로부터의 무한 재귀 방지(순수 방어적 상한, 정상 트리는 절대 도달 안 함)
    }
    // 내부 노드 - ei_block <= logicalBlock인 마지막 엔트리(오름차순 정렬 전제, ext4 표준)로 내려간다.
    const ExtentIdx* chosen = nullptr;
    for (uint16_t i = 0; i < header->entries; ++i) {
        const auto* idx = reinterpret_cast<const ExtentIdx*>(entries + i * sizeof(ExtentIdx));
        if (idx->block <= logicalBlock) {
            chosen = idx;
        } else {
            break;
        }
    }
    if (!chosen) {
        return false;
    }
    const uint64_t childBlock = (static_cast<uint64_t>(chosen->leafHi) << 32) | chosen->leafLo;
    auto* childBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(blockSize_));
    if (!childBuf) {
        return false;
    }
    bool ok = readExtBlocksImpl(device_, blockSize_, childBlock, 1, childBuf) &&
              resolveExtentNode(childBuf, logicalBlock, depthBudget - 1, outPhysicalBlock);
    kernel::GenericSlabAllocator::free(childBuf, blockSize_);
    return ok;
}

bool Ext4Volume::resolveExtent(const InodeCore& inode, uint32_t logicalBlock, uint64_t* outPhysicalBlock) {
    if ((inode.flags & kExtentsFl) == 0) {
        return false;  // 레거시 간접 블록 매핑 - §2.2 후속(1차 증분은 EXTENTS_FL 필수, mount()에서 이미 강제)
    }
    constexpr uint32_t kMaxExtentTreeDepth = 5;  // 표준 ext4 트리는 이보다 훨씬 얕음 - 순수 방어적 상한
    return resolveExtentNode(inode.block, logicalBlock, kMaxExtentTreeDepth, outPhysicalBlock);
}

bool Ext4Volume::statInode(uint32_t inodeNum, uint64_t* outSize, bool* outIsDir) {
    InodeCore inode;
    if (!readInodeStruct(inodeNum, &inode)) {
        return false;
    }
    *outSize = inode.sizeLo | (static_cast<uint64_t>(inode.sizeHigh) << 32);
    *outIsDir = isDirMode(inode.mode);
    return true;
}

uint32_t Ext4Volume::readInode(uint32_t inodeNum, uint64_t offset, void* buf, uint32_t len, bool* outOk) {
    InodeCore inode;
    if (!readInodeStruct(inodeNum, &inode)) {
        *outOk = false;
        return 0;
    }
    const uint64_t fileSize = inode.sizeLo | (static_cast<uint64_t>(inode.sizeHigh) << 32);
    if (offset >= fileSize) {
        *outOk = true;
        return 0;  // EOF - livefs/procfs와 동일한 관례(ReadResult{0, VfsError::None})
    }
    uint64_t remaining = fileSize - offset;
    if (remaining > len) {
        remaining = len;
    }

    auto* blockBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(blockSize_));
    if (!blockBuf) {
        *outOk = false;
        return 0;
    }

    uint32_t totalCopied = 0;
    auto* out = static_cast<uint8_t*>(buf);
    while (remaining > 0) {
        const uint64_t curOffset = offset + totalCopied;
        const uint32_t logicalBlock = static_cast<uint32_t>(curOffset / blockSize_);
        const uint32_t offsetInBlock = static_cast<uint32_t>(curOffset % blockSize_);
        const uint32_t chunk =
            static_cast<uint32_t>(remaining < (blockSize_ - offsetInBlock) ? remaining : (blockSize_ - offsetInBlock));

        uint64_t physicalBlock = 0;
        if (resolveExtent(inode, logicalBlock, &physicalBlock)) {
            if (!readExtBlocksImpl(device_, blockSize_, physicalBlock, 1, blockBuf)) {
                break;  // 실제 I/O 실패 - 지금까지 읽은 만큼만 반환
            }
            memcpy(out + totalCopied, blockBuf + offsetInBlock, chunk);
        } else {
            // 파일 구멍(hole) - 매핑된 익스텐트가 없는 논리 블록은 0으로 읽힌다(POSIX sparse file 관례).
            memset(out + totalCopied, 0, chunk);
        }

        totalCopied += chunk;
        remaining -= chunk;
    }

    kernel::GenericSlabAllocator::free(blockBuf, blockSize_);
    *outOk = true;
    return totalCopied;
}

namespace {

// 디렉터리 데이터 블록 하나(blockSize바이트) 안의 DirEntry2 레코드를
// 순회하며 콜백에 (inode, fileType, name, nameLen)을 넘긴다 - true를
// 반환하면 그 자리에서 순회를 멈춘다(findDirEntry의 "찾음" 신호).
template <typename Callback>
bool forEachDirEntryInBlock(const uint8_t* block, uint32_t blockSize, Callback&& cb) {
    uint32_t pos = 0;
    while (pos + sizeof(DirEntry2Header) <= blockSize) {
        const auto* hdr = reinterpret_cast<const DirEntry2Header*>(block + pos);
        if (hdr->recLen == 0 || pos + hdr->recLen > blockSize) {
            break;  // 손상됐거나 블록 끝 - 더 진행할 수 없음
        }
        if (hdr->inode != 0) {
            const char* name = reinterpret_cast<const char*>(block + pos + sizeof(DirEntry2Header));
            if (cb(hdr->inode, hdr->fileType, name, hdr->nameLen)) {
                return true;
            }
        }
        pos += hdr->recLen;
    }
    return false;
}

}  // namespace

bool Ext4Volume::findDirEntry(uint32_t dirInodeNum, const char* name, uint32_t nameLen, uint32_t* outInode,
                               uint8_t* outFileType) {
    InodeCore dirInode;
    if (!readInodeStruct(dirInodeNum, &dirInode) || !isDirMode(dirInode.mode)) {
        return false;
    }
    const uint64_t dirSize = dirInode.sizeLo | (static_cast<uint64_t>(dirInode.sizeHigh) << 32);
    const uint32_t blockCount = static_cast<uint32_t>(kCeilDiv(dirSize, blockSize_));

    auto* blockBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(blockSize_));
    if (!blockBuf) {
        return false;
    }

    bool found = false;
    for (uint32_t logicalBlock = 0; logicalBlock < blockCount; ++logicalBlock) {
        uint64_t physicalBlock = 0;
        if (!resolveExtent(dirInode, logicalBlock, &physicalBlock)) {
            continue;  // 디렉터리 데이터에 구멍은 정상적으로 생기지 않음 - 방어적으로 건너뜀
        }
        if (!readExtBlocksImpl(device_, blockSize_, physicalBlock, 1, blockBuf)) {
            break;
        }
        uint32_t matchedInode = 0;
        uint8_t matchedType = 0;
        const bool hit = forEachDirEntryInBlock(blockBuf, blockSize_,
            [&](uint32_t inode, uint8_t fileType, const char* entryName, uint32_t entryNameLen) {
                if (entryNameLen != nameLen) {
                    return false;
                }
                for (uint32_t i = 0; i < nameLen; ++i) {
                    if (entryName[i] != name[i]) {
                        return false;
                    }
                }
                matchedInode = inode;
                matchedType = fileType;
                return true;
            });
        if (hit) {
            *outInode = matchedInode;
            *outFileType = matchedType;
            found = true;
            break;
        }
    }

    kernel::GenericSlabAllocator::free(blockBuf, blockSize_);
    return found;
}

bool Ext4Volume::resolvePath(const char* path, uint32_t pathLen, uint32_t* outInode, bool* outIsDir) {
    uint32_t currentInode = kRootInodeNumber;
    bool currentIsDir = true;

    uint32_t pos = 0;
    while (pos < pathLen) {
        while (pos < pathLen && path[pos] == '/') {
            ++pos;
        }
        if (pos >= pathLen) {
            break;
        }
        uint32_t segStart = pos;
        while (pos < pathLen && path[pos] != '/') {
            ++pos;
        }
        const uint32_t segLen = pos - segStart;
        if (!currentIsDir) {
            return false;  // 파일을 디렉터리처럼 더 파고들려 함
        }
        uint32_t nextInode = 0;
        uint8_t fileType = 0;
        if (!findDirEntry(currentInode, path + segStart, segLen, &nextInode, &fileType)) {
            return false;
        }
        currentInode = nextInode;
        currentIsDir = (fileType == kFtDir);
    }

    *outInode = currentInode;
    *outIsDir = currentIsDir;
    return true;
}

bool Ext4Volume::readdirAt(uint32_t dirInodeNum, uint64_t index, char* nameOut, uint32_t nameOutCap,
                            uint32_t* outNameLen, bool* outIsDir, uint32_t* outEntryInode) {
    InodeCore dirInode;
    if (!readInodeStruct(dirInodeNum, &dirInode) || !isDirMode(dirInode.mode)) {
        return false;
    }
    const uint64_t dirSize = dirInode.sizeLo | (static_cast<uint64_t>(dirInode.sizeHigh) << 32);
    const uint32_t blockCount = static_cast<uint32_t>(kCeilDiv(dirSize, blockSize_));

    auto* blockBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(blockSize_));
    if (!blockBuf) {
        return false;
    }

    uint64_t seen = 0;
    bool found = false;
    for (uint32_t logicalBlock = 0; logicalBlock < blockCount && !found; ++logicalBlock) {
        uint64_t physicalBlock = 0;
        if (!resolveExtent(dirInode, logicalBlock, &physicalBlock)) {
            continue;
        }
        if (!readExtBlocksImpl(device_, blockSize_, physicalBlock, 1, blockBuf)) {
            break;
        }
        forEachDirEntryInBlock(blockBuf, blockSize_,
            [&](uint32_t inode, uint8_t fileType, const char* entryName, uint32_t entryNameLen) {
                if (seen == index) {
                    const uint32_t toCopy = entryNameLen < nameOutCap ? entryNameLen : nameOutCap;
                    memcpy(nameOut, entryName, toCopy);
                    *outNameLen = toCopy;
                    *outIsDir = (fileType == kFtDir);
                    *outEntryInode = inode;
                    found = true;
                    return true;  // forEachDirEntryInBlock 순회 중단
                }
                ++seen;
                return false;
            });
    }

    kernel::GenericSlabAllocator::free(blockBuf, blockSize_);
    return found;
}

}  // namespace ext4
