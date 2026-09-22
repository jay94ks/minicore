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

// ASCII 바이트 하나를 UTF-16 코드유닛으로 폭 확장 - v1은 ASCII 경로만
// 지원(exfat.h의 resolvePath 문서 주석 참고).
uint16_t kWidenAscii(char c) { return static_cast<uint16_t>(static_cast<uint8_t>(c)); }

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

void ExfatVolume::upcaseInPlace(uint16_t* codeUnits, uint32_t count) const {
    for (uint32_t i = 0; i < count; ++i) {
        codeUnits[i] = upcaseTable_[codeUnits[i]];
    }
}

// 32바이트 슬롯을 클러스터 체인에서 순서대로 읽어 주는 커서 - 클러스터
// 경계를 넘어가는 엔트리 집합(0x85+0xC0+0xC1×N)도 투명하게 처리한다.
// 중첩 클래스라 ExfatVolume의 private 멤버에 직접 접근한다(C++11
// 중첩 클래스는 바깥 클래스의 멤버 접근 권한을 그대로 가짐).
class ExfatVolume::DirSlotCursor {
public:
    DirSlotCursor(ExfatVolume* volume, uint32_t clusterSize)
        : _volume(volume), _clusterSize(clusterSize), _buf(clusterSize) {}

    bool valid() const { return static_cast<bool>(_buf); }

    // dirFirstCluster/dirNoFatChain/dirDataLength를 설정한다. noFatChain
    // 이면 dirDataLength로 클러스터 수 상한을 계산(FAT 미참조) - 아니면
    // EOC를 만날 때까지 무제한.
    bool init(uint32_t dirFirstCluster, bool dirNoFatChain, uint64_t dirDataLength) {
        _noFatChain = dirNoFatChain;
        _clusterLimit = dirNoFatChain ? kCeilDiv(dirDataLength, _clusterSize) : ~static_cast<uint64_t>(0);
        _cluster = dirFirstCluster;
        _clusterIndex = 0;
        _posInCluster = _clusterSize;  // 다음 nextSlot() 호출이 즉시 첫 클러스터를 읽게 함
        return true;
    }

    // 32바이트 슬롯 하나를 꺼낸다 - 디렉터리 끝(체인 끝/한도 도달)이면 false.
    bool nextSlot(uint8_t out[32]) {
        if (_posInCluster >= _clusterSize) {
            if (!advanceCluster()) {
                return false;
            }
        }
        memcpy(out, _buf.get() + _posInCluster, 32);
        _posInCluster += 32;
        return true;
    }

private:
    bool advanceCluster() {
        if (_clusterIndex != 0) {
            if (_noFatChain) {
                ++_cluster;  // NoFatChain - 물리적으로 연속(§3.2)
            } else {
                uint32_t next = 0;
                if (!_volume->nextCluster(_cluster, &next)) {
                    return false;
                }
                _cluster = next;
            }
        }
        ++_clusterIndex;
        if (_clusterIndex > _clusterLimit) {
            return false;
        }
        uint32_t sector = 0;
        if (!_volume->clusterToSector(_cluster, &sector)) {
            return false;
        }
        if (!readSectorsImpl(_volume->device_, _volume->sectorSize_, sector, _volume->sectorsPerCluster_,
                              _buf.get())) {
            return false;
        }
        _posInCluster = 0;
        return true;
    }

    ExfatVolume* _volume;
    uint32_t _clusterSize;
    SlabBuf _buf;
    bool _noFatChain = false;
    uint64_t _clusterLimit = 0;
    uint32_t _cluster = 0;
    uint64_t _clusterIndex = 0;
    uint32_t _posInCluster = 0;
};

bool ExfatVolume::scanDirectory(uint32_t dirFirstCluster, bool dirNoFatChain, uint64_t dirDataLength,
                                 const uint16_t* nameUtf16Upper, uint32_t nameLen, uint64_t targetIndex,
                                 ResolvedEntry* out, char* nameOut, uint32_t nameOutCap, uint32_t* outNameLen) {
    DirSlotCursor cursor(this, clusterSize_);
    if (!cursor.valid()) {
        return false;
    }
    cursor.init(dirFirstCluster, dirNoFatChain, dirDataLength);

    // 엔트리 집합 하나(Primary 1개 + Secondary 최대 18개)를 통째로
    // 담는 스크래치 - 스펙상 secondaryCount 상한이 있어 고정 크기로
    // 충분하다(SlabBuf 대신 스택 - 크기가 작고 코루틴 프레임이 아님).
    constexpr uint32_t kMaxEntrySetSlots = 19;
    uint8_t entrySet[kMaxEntrySetSlots * 32];
    uint16_t nameBuf[256];  // secondaryCount<=18이면 이름 조각 최대 17개*15자=255자로 충분

    uint64_t seenFiles = 0;
    uint8_t slot[32];
    while (cursor.nextSlot(slot)) {
        const uint8_t entryType = slot[0];
        if (entryType == kExfatEntryTypeEndOfDirectory) {
            return false;  // 디렉터리 끝 - 더 이상 유효 엔트리 없음
        }
        if (entryType != kExfatEntryTypeFile) {
            continue;  // 삭제됨/특수 엔트리(0x81/0x82/0x83)/보조 엔트리 낙오분 - 건너뜀
        }

        ExfatFileDirEntry primary;
        memcpy(&primary, slot, sizeof(primary));
        memcpy(entrySet, slot, 32);
        const uint32_t secondaryCount = primary.secondaryCount;
        if (secondaryCount >= kMaxEntrySetSlots) {
            return false;  // 손상된 엔트리 집합 - 방어적으로 중단
        }
        bool setOk = true;
        for (uint32_t i = 0; i < secondaryCount; ++i) {
            if (!cursor.nextSlot(entrySet + (i + 1) * 32)) {
                setOk = false;
                break;
            }
        }
        if (!setOk) {
            return false;
        }

        // Stream Extension(0xC0)은 항상 첫 번째 Secondary(entrySet+32).
        if (secondaryCount < 1 || entrySet[32] != kExfatEntryTypeStreamExt) {
            continue;  // 형식이 예상과 다름 - 이 엔트리 집합은 건너뜀
        }
        ExfatStreamExtEntry stream;
        memcpy(&stream, entrySet + 32, sizeof(stream));
        const bool isDir = (primary.fileAttributes & kFileAttrDirectory) != 0;
        const bool noFatChain = (stream.generalSecondaryFlags & kNoFatChainBit) != 0;

        // 이름 조각(0xC1×N) 조립 - Secondary 2번째부터.
        uint32_t assembledLen = 0;
        for (uint32_t i = 1; i < secondaryCount && assembledLen < stream.nameLength; ++i) {
            const uint8_t* sec = entrySet + (i + 1) * 32;
            if (sec[0] != kExfatEntryTypeFileName) {
                continue;
            }
            ExfatFileNameEntry nameEntry;
            memcpy(&nameEntry, sec, sizeof(nameEntry));
            for (uint32_t k = 0; k < 15 && assembledLen < stream.nameLength && assembledLen < 256; ++k) {
                nameBuf[assembledLen++] = nameEntry.fileName[k];
            }
        }

        if (nameUtf16Upper != nullptr) {
            // "이름 일치 탐색" 모드 - Up-case 정규화 후 비교.
            if (assembledLen == nameLen) {
                uint16_t normalized[256];
                for (uint32_t i = 0; i < assembledLen; ++i) {
                    normalized[i] = nameBuf[i];
                }
                upcaseInPlace(normalized, assembledLen);
                bool matches = true;
                for (uint32_t i = 0; i < assembledLen; ++i) {
                    if (normalized[i] != nameUtf16Upper[i]) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    out->firstCluster = stream.firstCluster;
                    out->fileSize = stream.dataLength;
                    out->isDir = isDir;
                    out->noFatChain = noFatChain;
                    return true;
                }
            }
        } else {
            // "0-based 인덱스 조회" 모드.
            if (seenFiles == targetIndex) {
                out->firstCluster = stream.firstCluster;
                out->fileSize = stream.dataLength;
                out->isDir = isDir;
                out->noFatChain = noFatChain;
                uint32_t written = 0;
                for (uint32_t i = 0; i < assembledLen && written < nameOutCap; ++i, ++written) {
                    nameOut[written] = static_cast<char>(nameBuf[i] & 0xFF);  // v1 ASCII 범위만(§3.6 - 비ASCII는 후속)
                }
                *outNameLen = written;
                return true;
            }
            ++seenFiles;
        }
    }
    return false;
}

bool ExfatVolume::resolvePath(const char* path, uint32_t pathLen, ResolvedEntry* out) {
    uint32_t currentCluster = rootFirstCluster();
    bool currentNoFatChain = false;
    uint64_t currentDataLength = 0;  // 루트는 §3.2대로 항상 일반 FAT 체인(EOC까지) - 한도 불필요
    bool currentIsDir = true;

    ResolvedEntry entry;
    entry.firstCluster = currentCluster;
    entry.isDir = true;
    entry.fileSize = 0;
    entry.noFatChain = false;

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
            return false;
        }

        uint16_t queryUpper[256];
        if (segLen > 256) {
            return false;
        }
        for (uint32_t i = 0; i < segLen; ++i) {
            queryUpper[i] = kWidenAscii(path[segStart + i]);
        }
        upcaseInPlace(queryUpper, segLen);

        if (!scanDirectory(currentCluster, currentNoFatChain, currentDataLength, queryUpper, segLen, 0, &entry,
                            nullptr, 0, nullptr)) {
            return false;
        }
        currentCluster = entry.firstCluster;
        currentNoFatChain = entry.noFatChain;
        currentDataLength = entry.fileSize;
        currentIsDir = entry.isDir;
    }

    *out = entry;
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

bool ExfatVolume::readdirAt(uint32_t dirFirstCluster, uint64_t index, char* nameOut, uint32_t nameOutCap,
                             uint32_t* outNameLen, bool* outIsDir, uint32_t* outFirstCluster, uint64_t* outFileSize,
                             bool* outNoFatChain) {
    // readdirAt()은 이미 Open()으로 얻은 디렉터리 핸들(firstCluster)만
    // 받으므로, 이 디렉터리 자신이 NoFatChain인지/크기가 얼마인지는
    // 호출부가 별도로 들고 있어야 한다 - v1의 무상태 API 한계(§4의
    // ExfatDriver가 실제로는 handle에 이 정보까지 인코딩해 넘길 것,
    // libvfat의 FileHandle 인코딩과 동일한 필요성). 이 함수 자체는
    // 항상 일반 FAT 체인(EOC까지)으로 순회한다 - NoFatChain 최적화가
    // 필요한 대용량 디렉터리는 드물고, 후속 ExfatDriver가 handle에
    // 인코딩된 실제 noFatChain/dataLength를 scanDirectory에 그대로
    // 전달하도록 재구현할 것.
    ResolvedEntry entry;
    if (!scanDirectory(dirFirstCluster, /*dirNoFatChain=*/false, /*dirDataLength=*/0, nullptr, 0, index, &entry,
                        nameOut, nameOutCap, outNameLen)) {
        return false;
    }
    *outIsDir = entry.isDir;
    *outFirstCluster = entry.firstCluster;
    *outFileSize = entry.fileSize;
    *outNoFatChain = entry.noFatChain;
    return true;
}

}  // namespace exfat
