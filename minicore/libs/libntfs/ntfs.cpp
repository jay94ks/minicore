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

uint16_t kWidenAscii(char c) { return static_cast<uint16_t>(static_cast<uint8_t>(c)); }

uint64_t kReadUnsignedLe(const uint8_t* bytes, uint32_t size) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < size; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

// 오프셋 필드는 2의 보수 부호 있는 정수(§3.4) - size바이트 폭 기준으로
// 부호 확장한다.
int64_t kReadSignedLe(const uint8_t* bytes, uint32_t size) {
    uint64_t raw = kReadUnsignedLe(bytes, size);
    if (size < 8 && size > 0 && (raw & (1ull << (8 * size - 1)))) {
        raw |= ~0ull << (8 * size);  // 부호 비트가 서 있으면 상위 비트를 1로 채움
    }
    return static_cast<int64_t>(raw);
}

// 데이터 런 목록에서 targetVcn을 담당하는 런을 찾는다(순수 계산, I/O
// 없음) - §3.4. sparse 런(오프셋 필드 폭 0)이면 *outSparse=true.
bool kResolveVcnToLcn(const uint8_t* dataRuns, uint32_t dataRunsMaxLen, uint64_t targetVcn, uint64_t* outLcn,
                      bool* outSparse, uint64_t* outRunRemainingVcns) {
    uint64_t currentVcn = 0;
    int64_t currentLcn = 0;
    uint32_t pos = 0;
    while (pos < dataRunsMaxLen) {
        const uint8_t header = dataRuns[pos];
        if (header == 0) {
            break;  // 데이터 런 시퀀스 종료
        }
        const uint32_t lengthSize = header & 0x0F;
        const uint32_t offsetSize = (header >> 4) & 0x0F;
        ++pos;
        if (pos + lengthSize + offsetSize > dataRunsMaxLen) {
            break;  // 손상된 데이터 런 - 방어적으로 중단
        }
        const uint64_t runLength = kReadUnsignedLe(dataRuns + pos, lengthSize);
        pos += lengthSize;
        const bool isSparse = (offsetSize == 0);
        if (!isSparse) {
            currentLcn += kReadSignedLe(dataRuns + pos, offsetSize);
        }
        pos += offsetSize;

        if (targetVcn >= currentVcn && targetVcn < currentVcn + runLength) {
            *outSparse = isSparse;
            *outRunRemainingVcns = currentVcn + runLength - targetVcn;
            if (!isSparse) {
                *outLcn = static_cast<uint64_t>(currentLcn) + (targetVcn - currentVcn);
            }
            return true;
        }
        currentVcn += runLength;
    }
    return false;
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

bool NtfsVolume::clusterToSector(uint64_t lcn, uint64_t* outSector) const {
    *outSector = lcn * bs_.sectorsPerCluster;
    return true;
}

bool NtfsVolume::readClusters(uint64_t lcn, uint32_t count, void* buf) {
    uint64_t sector = 0;
    clusterToSector(lcn, &sector);
    return readSectorsImpl(device_, bs_.bytesPerSector, sector, count * bs_.sectorsPerCluster, buf);
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

bool NtfsVolume::findAttribute(const uint8_t* recordBuf, uint32_t attrType, bool matchUnnamedOnly,
                                const uint8_t** outAttr, uint32_t* outAttrLen) {
    NtfsFileRecordHeader header;
    memcpy(&header, recordBuf, sizeof(header));
    uint32_t pos = header.firstAttributeOffset;
    while (pos + sizeof(NtfsAttributeHeader) <= header.usedSize && pos + sizeof(NtfsAttributeHeader) <= mftRecordSize_) {
        NtfsAttributeHeader attrHeader;
        memcpy(&attrHeader, recordBuf + pos, sizeof(attrHeader));
        if (attrHeader.type == kNtfsAttrTypeEnd) {
            break;
        }
        if (attrHeader.length == 0 || pos + attrHeader.length > mftRecordSize_) {
            break;  // 손상 - 방어적으로 중단
        }
        if (attrHeader.type == attrType && (!matchUnnamedOnly || attrHeader.nameLength == 0)) {
            *outAttr = recordBuf + pos;
            *outAttrLen = attrHeader.length;
            return true;
        }
        pos += attrHeader.length;
    }
    return false;
}

uint32_t NtfsVolume::readData(uint64_t mftRecordNumber, uint64_t offset, void* buf, uint32_t len, bool* outOk) {
    SlabBuf recordBuf(mftRecordSize_);
    if (!recordBuf) {
        *outOk = false;
        return 0;
    }
    if (!readMftRecord(mftRecordNumber, recordBuf.get())) {
        *outOk = false;
        return 0;
    }
    const uint8_t* attr = nullptr;
    uint32_t attrLen = 0;
    if (!findAttribute(recordBuf.get(), kNtfsAttrTypeData, /*matchUnnamedOnly=*/true, &attr, &attrLen)) {
        *outOk = false;
        return 0;
    }
    NtfsAttributeHeader attrHeader;
    memcpy(&attrHeader, attr, sizeof(attrHeader));

    if (attrHeader.nonResident == 0) {
        NtfsResidentAttrTail tail;
        memcpy(&tail, attr + sizeof(NtfsAttributeHeader), sizeof(tail));
        if (offset >= tail.contentLength) {
            *outOk = true;
            return 0;  // EOF
        }
        uint64_t remaining = tail.contentLength - offset;
        if (remaining > len) {
            remaining = len;
        }
        memcpy(buf, attr + tail.contentOffset + offset, remaining);
        *outOk = true;
        return static_cast<uint32_t>(remaining);
    }

    NtfsNonResidentAttrTail tail;
    memcpy(&tail, attr + sizeof(NtfsAttributeHeader), sizeof(tail));
    if (offset >= tail.realSize) {
        *outOk = true;
        return 0;  // EOF
    }
    uint64_t remaining = tail.realSize - offset;
    if (remaining > len) {
        remaining = len;
    }
    const uint8_t* dataRuns = attr + tail.dataRunsOffset;
    const uint32_t dataRunsMaxLen = attrHeader.length - tail.dataRunsOffset;

    SlabBuf clusterBuf(clusterSize_);
    if (!clusterBuf) {
        *outOk = false;
        return 0;
    }
    uint32_t totalCopied = 0;
    auto* out = static_cast<uint8_t*>(buf);
    while (remaining > 0) {
        const uint64_t curOffset = offset + totalCopied;
        const uint64_t targetVcn = curOffset / clusterSize_;
        const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset % clusterSize_);
        uint64_t lcn = 0;
        bool sparse = false;
        uint64_t runRemainingVcns = 0;
        if (!kResolveVcnToLcn(dataRuns, dataRunsMaxLen, targetVcn, &lcn, &sparse, &runRemainingVcns)) {
            break;  // 손상되었거나 realSize보다 짧은 런 목록 - 방어적으로 중단
        }
        const uint32_t chunk = static_cast<uint32_t>(
            remaining < (clusterSize_ - offsetInCluster) ? remaining : (clusterSize_ - offsetInCluster));
        if (sparse) {
            memset(out + totalCopied, 0, chunk);  // sparse 런 - 논리적으로 0(§3.4)
        } else {
            if (!readClusters(lcn, 1, clusterBuf.get())) {
                break;
            }
            memcpy(out + totalCopied, clusterBuf.get() + offsetInCluster, chunk);
        }
        totalCopied += chunk;
        remaining -= chunk;
    }
    *outOk = true;
    return totalCopied;
}

bool NtfsVolume::scanIndexRoot(uint64_t dirRecordNumber, const uint16_t* nameUtf16, uint32_t nameLen,
                                uint64_t targetIndex, ResolvedEntry* out, char* nameOut, uint32_t nameOutCap,
                                uint32_t* outNameLen, uint64_t* outChildRecord) {
    SlabBuf recordBuf(mftRecordSize_);
    if (!recordBuf) {
        return false;
    }
    if (!readMftRecord(dirRecordNumber, recordBuf.get())) {
        return false;
    }
    const uint8_t* attr = nullptr;
    uint32_t attrLen = 0;
    if (!findAttribute(recordBuf.get(), kNtfsAttrTypeIndexRoot, /*matchUnnamedOnly=*/false, &attr, &attrLen)) {
        return false;
    }
    // $INDEX_ROOT는 항상 상주(§3.6).
    NtfsResidentAttrTail tail;
    memcpy(&tail, attr + sizeof(NtfsAttributeHeader), sizeof(tail));
    const uint8_t* content = attr + tail.contentOffset;

    NtfsIndexRootHeader rootHeader;
    memcpy(&rootHeader, content, sizeof(rootHeader));
    const uint8_t* idxHdrPtr = content + sizeof(NtfsIndexRootHeader);
    NtfsIndexHeader idxHeader;
    memcpy(&idxHeader, idxHdrPtr, sizeof(idxHeader));

    const uint8_t* entriesStart = idxHdrPtr + idxHeader.entriesOffset;
    const uint8_t* entriesEnd = idxHdrPtr + idxHeader.usedSize;

    uint64_t seen = 0;
    const uint8_t* pos = entriesStart;
    while (pos + sizeof(NtfsIndexEntry) <= entriesEnd) {
        NtfsIndexEntry entry;
        memcpy(&entry, pos, sizeof(entry));
        if (entry.flags & kIndexEntryLast) {
            break;  // 더 이상 유효 엔트리 없음(키 없는 종결 엔트리)
        }
        if (entry.entryLength < sizeof(NtfsIndexEntry) || pos + entry.entryLength > entriesEnd) {
            break;  // 손상 - 방어적으로 중단
        }
        if (entry.flags & kIndexEntryHasSubnode) {
            return false;  // $INDEX_ALLOCATION 하위 노드 필요 - 1차 증분 미지원(명시적 거부)
        }

        const uint8_t* key = pos + sizeof(NtfsIndexEntry);
        NtfsFileNameContent fileName;
        memcpy(&fileName, key, sizeof(fileName));
        const auto* nameChars = reinterpret_cast<const uint16_t*>(key + sizeof(NtfsFileNameContent));

        // DOS 전용 8.3 별칭(namespace==2)은 "대표 이름"이 아니므로
        // 건너뛴다(SP-AA6DF406 §3.5 - Win32/Win32&Dos만 채택).
        if (fileName.nameNamespace == 2) {
            pos += entry.entryLength;
            continue;
        }

        if (nameUtf16 != nullptr) {
            if (fileName.nameLength == nameLen) {
                bool matches = true;
                for (uint32_t i = 0; i < nameLen; ++i) {
                    if (nameChars[i] != nameUtf16[i]) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    out->mftRecordNumber = kNtfsMftReferenceRecordNumber(entry.mftReference);
                    out->fileSize = fileName.realSize;
                    out->isDir = (fileName.fileAttributes & kFileAttrDirectory) != 0;
                    if (outChildRecord) {
                        *outChildRecord = out->mftRecordNumber;
                    }
                    return true;
                }
            }
        } else {
            if (seen == targetIndex) {
                out->mftRecordNumber = kNtfsMftReferenceRecordNumber(entry.mftReference);
                out->fileSize = fileName.realSize;
                out->isDir = (fileName.fileAttributes & kFileAttrDirectory) != 0;
                if (outChildRecord) {
                    *outChildRecord = out->mftRecordNumber;
                }
                uint32_t written = 0;
                for (uint32_t i = 0; i < fileName.nameLength && written < nameOutCap; ++i, ++written) {
                    nameOut[written] = static_cast<char>(nameChars[i] & 0xFF);  // v1 ASCII 범위만
                }
                *outNameLen = written;
                return true;
            }
            ++seen;
        }
        pos += entry.entryLength;
    }
    return false;
}

bool NtfsVolume::resolvePath(const char* path, uint32_t pathLen, ResolvedEntry* out) {
    uint64_t currentRecord = kNtfsRootDirectoryRecord;
    bool currentIsDir = true;

    ResolvedEntry entry;
    entry.mftRecordNumber = currentRecord;
    entry.isDir = true;
    entry.fileSize = 0;

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
        if (segLen > 256) {
            return false;
        }
        uint16_t query[256];
        for (uint32_t i = 0; i < segLen; ++i) {
            query[i] = kWidenAscii(path[segStart + i]);
        }

        uint64_t childRecord = 0;
        if (!scanIndexRoot(currentRecord, query, segLen, 0, &entry, nullptr, 0, nullptr, &childRecord)) {
            return false;
        }
        currentRecord = entry.mftRecordNumber;
        currentIsDir = entry.isDir;
    }

    *out = entry;
    return true;
}

bool NtfsVolume::readdirAt(uint64_t dirRecordNumber, uint64_t index, char* nameOut, uint32_t nameOutCap,
                            uint32_t* outNameLen, bool* outIsDir, uint64_t* outChildRecord) {
    ResolvedEntry entry;
    if (!scanIndexRoot(dirRecordNumber, nullptr, 0, index, &entry, nameOut, nameOutCap, outNameLen, outChildRecord)) {
        return false;
    }
    *outIsDir = entry.isDir;
    return true;
}

}  // namespace ntfs
