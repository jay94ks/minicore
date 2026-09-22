#include "exfat.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace exfat {

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

bool ExfatVolume::mount(fs::BlockDevice* device) {
    if (!device) {
        return false;
    }
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0) {
        return false;
    }

    // 1) 부트 섹터(LBA 0, 512바이트).
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
        if (bs_.fileSystemName[i] != kExfatFsName[i]) {
            return false;  // 이 포맷 아님
        }
    }

    // 2) 기본 유효성 - v1은 NumberOfFats==1(일반 exFAT)만 지원(TexFAT은
    // 후속, SP-F1987EF8 §2).
    if (bs_.numberOfFats != 1) {
        return false;
    }
    if (bs_.bytesPerSectorShift < 9 || bs_.bytesPerSectorShift > 12) {
        return false;
    }
    if (bs_.clusterCount == 0 || bs_.firstClusterOfRootDirectory < kExfatFirstCluster) {
        return false;
    }

    sectorSize_ = 1u << bs_.bytesPerSectorShift;
    sectorsPerCluster_ = 1u << bs_.sectorsPerClusterShift;
    clusterSize_ = sectorSize_ * sectorsPerCluster_;
    if (sectorSize_ % devBlockSize != 0) {
        return false;  // v1 제약 - libext4/libvfat과 동일(장치 블록 크기의 정확한 배수만 지원)
    }

    device_ = device;

    // 3) 루트 디렉터리를 스캔해 할당 비트맵(0x81)/Up-case 테이블(0x82)
    // 특수 엔트리를 찾는다 - 둘 다 보조 엔트리 없는 단독 Primary
    // 엔트리라 일반 0x85 엔트리 집합 파서(scanDirectory)를 쓰지 않고
    // 이 자리에서 직접 훑는다. 루트 자신은 부모가 없어 NoFatChain
    // 정보가 없으므로 항상 일반 FAT 체인으로 순회한다(EOC까지).
    uint32_t bitmapFirstCluster = 0;
    uint64_t bitmapDataLength = 0;
    uint32_t upcaseFirstCluster = 0;
    uint64_t upcaseDataLength = 0;
    bool foundBitmap = false;
    bool foundUpcase = false;

    {
        SlabBuf clusterBuf(clusterSize_);
        if (!clusterBuf) {
            return false;
        }
        uint32_t cluster = bs_.firstClusterOfRootDirectory;
        bool stop = false;
        while (!stop && cluster != 0 && (foundBitmap == false || foundUpcase == false)) {
            uint32_t sector = 0;
            if (!clusterToSector(cluster, &sector) ||
                !readSectorsImpl(device_, sectorSize_, sector, sectorsPerCluster_, clusterBuf.get())) {
                break;
            }
            const uint32_t slotsPerCluster = clusterSize_ / 32;
            for (uint32_t i = 0; i < slotsPerCluster; ++i) {
                const uint8_t* slot = clusterBuf.get() + i * 32;
                const uint8_t entryType = slot[0];
                if (entryType == kExfatEntryTypeEndOfDirectory) {
                    stop = true;
                    break;
                }
                if (entryType == kExfatEntryTypeAllocBitmap) {
                    ExfatBitmapEntry e;
                    memcpy(&e, slot, sizeof(e));
                    bitmapFirstCluster = e.firstCluster;
                    bitmapDataLength = e.dataLength;
                    foundBitmap = true;
                } else if (entryType == kExfatEntryTypeUpcaseTable) {
                    ExfatUpcaseEntry e;
                    memcpy(&e, slot, sizeof(e));
                    upcaseFirstCluster = e.firstCluster;
                    upcaseDataLength = e.dataLength;
                    foundUpcase = true;
                }
            }
            if (stop) {
                break;
            }
            uint32_t next = 0;
            if (!nextCluster(cluster, &next)) {
                break;
            }
            cluster = next;
        }
    }

    if (!foundBitmap || !foundUpcase || bitmapDataLength == 0 || upcaseDataLength == 0) {
        return false;  // 필수 특수 파일 - 없으면 유효한 exFAT 볼륨이 아님
    }

    // 4) 할당 비트맵 전체를 캐싱(§3.3) - 클러스터 사용 여부는 이번
    // 증분(읽기 전용)에선 직접 쓰이지 않지만, 설계(SP-F1987EF8 §3.3/§4)
    // 가 mount() 시 캐싱을 확정해 뒀고 후속(쓰기) 증분이 그대로 재사용할
    // 수 있게 여기서 이미 로드해 둔다.
    allocBitmapBytes_ = static_cast<uint32_t>(bitmapDataLength);
    allocBitmap_ = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(allocBitmapBytes_));
    if (!allocBitmap_) {
        return false;
    }
    {
        bool ok = false;
        const uint32_t got =
            readData(bitmapFirstCluster, bitmapDataLength, /*noFatChain=*/false, 0, allocBitmap_, allocBitmapBytes_, &ok);
        if (!ok || got != allocBitmapBytes_) {
            kernel::GenericSlabAllocator::free(allocBitmap_, allocBitmapBytes_);
            allocBitmap_ = nullptr;
            return false;
        }
    }

    // 5) Up-case 테이블 - 온디스크 압축(0xFFFF+런렝스, §3.6)을 풀어
    // 65536개 uint16 항목(코드포인트 0~0xFFFF 전부)으로 캐싱한다 -
    // 압축 안 된 영역은 미리 항등 매핑으로 채워 둔 뒤 실제 데이터로
    // 덮어써서 별도 "범위 밖이면 항등" 분기 없이 항상 정확하다.
    upcaseTableEntries_ = 0x10000;
    upcaseTable_ =
        static_cast<uint16_t*>(kernel::GenericSlabAllocator::alloc(upcaseTableEntries_ * sizeof(uint16_t)));
    if (!upcaseTable_) {
        kernel::GenericSlabAllocator::free(allocBitmap_, allocBitmapBytes_);
        allocBitmap_ = nullptr;
        return false;
    }
    for (uint32_t i = 0; i < upcaseTableEntries_; ++i) {
        upcaseTable_[i] = static_cast<uint16_t>(i);
    }
    {
        SlabBuf rawBuf(static_cast<uint32_t>(upcaseDataLength));
        if (!rawBuf) {
            kernel::GenericSlabAllocator::free(allocBitmap_, allocBitmapBytes_);
            allocBitmap_ = nullptr;
            kernel::GenericSlabAllocator::free(upcaseTable_, upcaseTableEntries_ * sizeof(uint16_t));
            upcaseTable_ = nullptr;
            return false;
        }
        bool ok = false;
        const uint32_t got = readData(upcaseFirstCluster, upcaseDataLength, /*noFatChain=*/false, 0, rawBuf.get(),
                                       static_cast<uint32_t>(upcaseDataLength), &ok);
        if (!ok || got != upcaseDataLength) {
            kernel::GenericSlabAllocator::free(allocBitmap_, allocBitmapBytes_);
            allocBitmap_ = nullptr;
            kernel::GenericSlabAllocator::free(upcaseTable_, upcaseTableEntries_ * sizeof(uint16_t));
            upcaseTable_ = nullptr;
            return false;
        }
        const uint32_t rawCount = static_cast<uint32_t>(upcaseDataLength / sizeof(uint16_t));
        const auto* raw = reinterpret_cast<const uint16_t*>(rawBuf.get());
        uint32_t index = 0;
        for (uint32_t i = 0; i < rawCount && index < upcaseTableEntries_; ++i) {
            const uint16_t value = raw[i];
            if (value == 0xFFFFu) {
                if (i + 1 >= rawCount) {
                    break;  // 손상된 테이블 - 방어적으로 중단(이미 항등으로 채워진 나머지는 그대로 유효)
                }
                const uint16_t runLength = raw[i + 1];
                ++i;
                index += runLength;  // 항등 구간 - 이미 기본값으로 채워져 있어 쓸 것 없음
            } else {
                upcaseTable_[index] = value;
                ++index;
            }
        }
    }

    return true;
}

bool ExfatVolume::clusterToSector(uint32_t cluster, uint32_t* outSector) const {
    if (cluster < kExfatFirstCluster) {
        return false;
    }
    *outSector = bs_.clusterHeapOffset + (cluster - kExfatFirstCluster) * sectorsPerCluster_;
    return true;
}

bool ExfatVolume::nextCluster(uint32_t cluster, uint32_t* outNext) {
    const uint64_t fatByteOffset = static_cast<uint64_t>(cluster) * 4;
    const uint32_t fatSectorOffset = static_cast<uint32_t>(fatByteOffset / sectorSize_);
    const uint32_t byteOffsetInSector = static_cast<uint32_t>(fatByteOffset % sectorSize_);

    SlabBuf sectorBuf(sectorSize_);
    if (!sectorBuf) {
        return false;
    }
    if (!readSectorsImpl(device_, sectorSize_, bs_.fatOffset + fatSectorOffset, 1, sectorBuf.get())) {
        return false;
    }
    uint32_t entry = 0;
    memcpy(&entry, sectorBuf.get() + byteOffsetInSector, sizeof(entry));

    if (entry == kExfatFatFree || entry == kExfatBadCluster || entry == kExfatEoc) {
        return false;  // free/bad/EOC - 호출부가 "체인 끝"으로 처리
    }
    *outNext = entry;
    return true;
}

uint32_t ExfatVolume::readData(uint32_t firstCluster, uint64_t fileSize, bool noFatChain, uint64_t offset, void* buf,
                                uint32_t len, bool* outOk) {
    if (offset >= fileSize) {
        *outOk = true;
        return 0;  // EOF
    }
    uint64_t remaining = fileSize - offset;
    if (remaining > len) {
        remaining = len;
    }

    SlabBuf clusterBuf(clusterSize_);
    if (!clusterBuf) {
        *outOk = false;
        return 0;
    }

    // 목표 오프셋이 속한 클러스터까지 이동한다 - noFatChain이면 산술만
    // (§3.2 핵심 최적화), 아니면 FAT 체인을 선형으로 따라간다.
    uint32_t cluster = firstCluster;
    uint64_t clusterStartOffset = 0;
    if (noFatChain) {
        const uint64_t clustersToSkip = offset / clusterSize_;
        cluster = firstCluster + static_cast<uint32_t>(clustersToSkip);
        clusterStartOffset = clustersToSkip * clusterSize_;
    } else {
        while (clusterStartOffset + clusterSize_ <= offset) {
            uint32_t next = 0;
            if (!nextCluster(cluster, &next)) {
                *outOk = true;
                return 0;  // 파일 크기보다 짧은 체인 - 방어적으로 0 반환
            }
            cluster = next;
            clusterStartOffset += clusterSize_;
        }
    }

    uint32_t totalCopied = 0;
    auto* out = static_cast<uint8_t*>(buf);
    while (remaining > 0) {
        uint32_t sector = 0;
        if (!clusterToSector(cluster, &sector) ||
            !readSectorsImpl(device_, sectorSize_, sector, sectorsPerCluster_, clusterBuf.get())) {
            break;
        }
        const uint64_t curOffset = offset + totalCopied;
        const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset - clusterStartOffset);
        const uint32_t chunk = static_cast<uint32_t>(
            remaining < (clusterSize_ - offsetInCluster) ? remaining : (clusterSize_ - offsetInCluster));
        memcpy(out + totalCopied, clusterBuf.get() + offsetInCluster, chunk);
        totalCopied += chunk;
        remaining -= chunk;

        if (remaining > 0) {
            if (noFatChain) {
                ++cluster;
            } else {
                uint32_t next = 0;
                if (!nextCluster(cluster, &next)) {
                    break;
                }
                cluster = next;
            }
            clusterStartOffset += clusterSize_;
        }
    }

    *outOk = true;
    return totalCopied;
}

}  // namespace exfat
