#include "ntfs.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace ntfs {

namespace {

constexpr uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// slab 버퍼 RAII - ext4_driver.cpp의 SlabBuf와 동일한 관례.
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

bool readSectorsImpl(fs::BlockDevice* device, uint32_t sectorSize, uint64_t sectorStart, uint32_t sectorCount,
                      void* buf) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || sectorSize % devBlockSize != 0) {
        return false;
    }
    const kernel::uint32_t devBlocksPerSector = sectorSize / devBlockSize;
    return device->readBlocks(sectorStart * devBlocksPerSector, sectorCount * devBlocksPerSector, buf);
}

}  // namespace

bool NtfsVolume::mount(fs::BlockDevice* device) {
    if (!device) {
        return false;
    }
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0) {
        return false;
    }

    const kernel::uint32_t bootSectorBytes = 512;
    const kernel::uint32_t blocksNeeded = static_cast<kernel::uint32_t>(kCeilDiv(bootSectorBytes, devBlockSize));
    SlabBuf bootBuf(blocksNeeded * devBlockSize);
    if (!bootBuf) {
        return false;
    }
    if (!device->readBlocks(0, blocksNeeded, bootBuf.get())) {
        return false;
    }

    uint16_t signature = 0;
    memcpy(&signature, bootBuf.get() + kBootSectorSignatureOffset, sizeof(signature));
    if (signature != kBootSectorSignature) {
        return false;  // 이 포맷 아님
    }
    memcpy(&bs_, bootBuf.get(), sizeof(bs_));
    for (uint32_t i = 0; i < 8; ++i) {
        if (bs_.oemId[i] != kNtfsOemId[i]) {
            return false;  // 이 포맷 아님
        }
    }

    if (bs_.bytesPerSector == 0 || bs_.sectorsPerCluster == 0) {
        return false;
    }
    clusterSize_ = static_cast<uint32_t>(bs_.bytesPerSector) * bs_.sectorsPerCluster;
    if (clusterSize_ % devBlockSize != 0) {
        return false;  // v1 제약 - libext4/libvfat/libexfat과 동일
    }
    mftRecordSize_ = kNtfsClustersOrPow2Size(bs_.clustersPerMftRecord, clusterSize_);
    if (mftRecordSize_ == 0 || mftRecordSize_ % bs_.bytesPerSector != 0) {
        return false;
    }
    mftStartLcn_ = bs_.mftClusterNumber;

    device_ = device;

    // 루트(레코드 5)가 실제로 열리는지 최소한으로 확인 - 못 열면
    // 유효한 NTFS 볼륨이 아닌 것으로 간주.
    SlabBuf rootBuf(mftRecordSize_);
    if (!rootBuf) {
        device_ = nullptr;
        return false;
    }
    if (!readMftRecord(kNtfsRootDirectoryRecord, rootBuf.get())) {
        device_ = nullptr;
        return false;
    }
    return true;
}

bool NtfsVolume::readMftRecord(uint64_t recordNumber, uint8_t* outBuf) {
    const uint64_t recordByteOffset =
        mftStartLcn_ * clusterSize_ + recordNumber * static_cast<uint64_t>(mftRecordSize_);
    const uint64_t sector = recordByteOffset / bs_.bytesPerSector;
    const uint32_t sectorCount = mftRecordSize_ / bs_.bytesPerSector;
    if (!readSectorsImpl(device_, bs_.bytesPerSector, sector, sectorCount, outBuf)) {
        return false;
    }

    NtfsFileRecordHeader header;
    memcpy(&header, outBuf, sizeof(header));
    if (memcmp(header.magic, "FILE", 4) != 0) {
        return false;  // 손상/미사용 레코드
    }

    // fixup(수정 시퀀스) 검증 + 복원(§3.2 - 모든 레코드 읽기의 필수
    // 전제 조건, 이 단계를 빠뜨리면 각 섹터 끝 2바이트가 USN 쓰레기로
    // 오염된 채 해석된다).
    if (header.updateSequenceSize == 0 ||
        static_cast<uint32_t>(header.updateSequenceOffset) +
                static_cast<uint32_t>(header.updateSequenceSize) * 2 >
            mftRecordSize_) {
        return false;  // 손상된 fixup 배열 위치
    }
    const uint8_t* usa = outBuf + header.updateSequenceOffset;
    uint16_t usn = 0;
    memcpy(&usn, usa, sizeof(usn));
    const uint32_t sectorsInRecord = header.updateSequenceSize - 1;
    for (uint32_t i = 0; i < sectorsInRecord; ++i) {
        const uint32_t sectorEndOffset = (i + 1) * bs_.bytesPerSector - 2;
        if (sectorEndOffset + 2 > mftRecordSize_) {
            return false;
        }
        uint16_t sectorTail = 0;
        memcpy(&sectorTail, outBuf + sectorEndOffset, sizeof(sectorTail));
        if (sectorTail != usn) {
            return false;  // 손상(전원 손실 등) - 레코드 거부
        }
        memcpy(outBuf + sectorEndOffset, usa + (i + 1) * 2, sizeof(uint16_t));
    }
    return true;
}

}  // namespace ntfs
