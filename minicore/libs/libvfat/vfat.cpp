#include "vfat.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace vfat {

namespace {

constexpr uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// 경로 세그먼트(정규화 안 된 원문)를 8.3 형식(대문자, 11바이트: 이름
// 8자+확장자 3자, 공백 패딩)으로 정규화한다 - 스펙 자체가 8.3 이름을
// 항상 대문자로 저장(§5, NT 소문자 확장은 후속 증분). "." 하나로
// 이름/확장자를 나눈다 - 그 이상의 "."은 이름 쪽에 흡수(스펙 관례).
void kNormalizeTo83(const char* seg, uint32_t segLen, char out11[11]) {
    for (uint32_t i = 0; i < 11; ++i) {
        out11[i] = ' ';
    }
    uint32_t dot = segLen;
    for (uint32_t i = 0; i < segLen; ++i) {
        if (seg[i] == '.') {
            dot = i;  // 마지막 '.'을 확장자 구분자로(연속 여러 개면 뒤쪽이 이김 - 스펙 관례)
        }
    }
    auto toUpper = [](char c) -> char { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; };
    uint32_t nameLen = (dot < segLen) ? dot : segLen;
    if (nameLen > 8) {
        nameLen = 8;
    }
    for (uint32_t i = 0; i < nameLen; ++i) {
        out11[i] = toUpper(seg[i]);
    }
    if (dot < segLen) {
        uint32_t extLen = segLen - dot - 1;
        if (extLen > 3) {
            extLen = 3;
        }
        for (uint32_t i = 0; i < extLen; ++i) {
            out11[8 + i] = toUpper(seg[dot + 1 + i]);
        }
    }
}

// DirEntry::name+ext(11바이트)를 정규화된 11바이트와 그대로 비교 -
// 0x05 이스케이프(실제 첫 글자가 0xE5)를 원래 값으로 되돌린 뒤 비교.
bool kNameMatches(const DirEntry& e, const char normalized11[11]) {
    char raw[11];
    memcpy(raw, e.name, 8);
    memcpy(raw + 8, e.ext, 3);
    if (static_cast<uint8_t>(raw[0]) == kNameEscapedE5) {
        raw[0] = static_cast<char>(kNameDeletedMarker);
    }
    for (uint32_t i = 0; i < 11; ++i) {
        if (raw[i] != normalized11[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool Fat32Volume::mount(fs::BlockDevice* device) {
    if (!device) {
        return false;
    }
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0) {
        return false;
    }

    // 1) 부트 섹터(LBA 0, 512바이트) - BpbCommon+Fat32Extended를 담을
    // 만큼만 읽는다(장치 블록 크기가 512보다 크면 그 한 블록 안에
    // 이미 다 들어있다).
    const kernel::uint32_t bootSectorBytes = 512;
    const kernel::uint32_t blocksNeeded =
        static_cast<kernel::uint32_t>(kCeilDiv(bootSectorBytes, devBlockSize));
    auto* buf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(blocksNeeded * devBlockSize));
    if (!buf) {
        return false;
    }
    if (!device->readBlocks(0, blocksNeeded, buf)) {
        kernel::GenericSlabAllocator::free(buf, blocksNeeded * devBlockSize);
        return false;
    }

    uint16_t signature = 0;
    memcpy(&signature, buf + kBootSectorSignatureOffset, sizeof(signature));
    if (signature != kBootSectorSignature) {
        kernel::GenericSlabAllocator::free(buf, blocksNeeded * devBlockSize);
        return false;  // 이 포맷 아님
    }
    memcpy(&bpb_, buf, sizeof(bpb_));
    memcpy(&ext32_, buf + kFat32ExtendedOffset, sizeof(ext32_));
    kernel::GenericSlabAllocator::free(buf, blocksNeeded * devBlockSize);

    // 2) BPB 기본 유효성 - 2의 거듭제곱 검사(스펙 위반이면 실제 FAT
    // 이미지가 아닐 가능성).
    if (bpb_.bytesPerSector == 0 || (bpb_.bytesPerSector & (bpb_.bytesPerSector - 1)) != 0) {
        return false;
    }
    if (bpb_.sectorsPerCluster == 0 || (bpb_.sectorsPerCluster & (bpb_.sectorsPerCluster - 1)) != 0) {
        return false;
    }
    if (bpb_.numFats == 0 || ext32_.fatSize32 == 0) {
        return false;
    }
    if (ext32_.fsVersion != 0) {
        return false;  // 이 스펙 버전만 지원
    }

    // 3) §3.2 클러스터 수 기반 포맷 재확인 - fileSystemType 문자열은
    // 신뢰하지 않는다(스펙 원문 경고). v1은 FAT32만 지원.
    const uint32_t totalSectors = (bpb_.totalSectors32 != 0) ? bpb_.totalSectors32 : bpb_.totalSectors16;
    const uint32_t fatSectors = static_cast<uint32_t>(bpb_.numFats) * ext32_.fatSize32;
    if (totalSectors <= bpb_.reservedSectorCount + fatSectors) {
        return false;
    }
    const uint32_t dataSectors = totalSectors - (bpb_.reservedSectorCount + fatSectors);
    const uint32_t clusterCount = dataSectors / bpb_.sectorsPerCluster;
    if (clusterCount < 65525) {
        return false;  // FAT12/FAT16 - v1 미지원(§2 스코프 컷)
    }

    // 4) 장치 블록 크기와의 정합성 - 이 v1은 섹터 크기가 장치 블록
    // 크기의 정확한 배수인 경우만 지원(libext4/libswapfs와 동일 제약).
    if (bpb_.bytesPerSector % devBlockSize != 0) {
        return false;
    }

    fatStartSector_ = bpb_.reservedSectorCount;
    dataStartSector_ = bpb_.reservedSectorCount + fatSectors;  // FAT32는 고정 루트 영역이 없음(§3.3)
    bytesPerCluster_ = static_cast<uint32_t>(bpb_.sectorsPerCluster) * bpb_.bytesPerSector;
    device_ = device;
    return true;
}

bool Fat32Volume::clusterToSector(uint32_t cluster, uint32_t* outSector) const {
    if (cluster < kFirstDataCluster) {
        return false;
    }
    *outSector = dataStartSector_ + (cluster - kFirstDataCluster) * bpb_.sectorsPerCluster;
    return true;
}

namespace {
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

bool Fat32Volume::nextCluster(uint32_t cluster, uint32_t* outNext) {
    const uint64_t fatByteOffset = static_cast<uint64_t>(cluster) * 4;
    const uint32_t fatSectorOffset = static_cast<uint32_t>(fatByteOffset / bpb_.bytesPerSector);
    const uint32_t byteOffsetInSector = static_cast<uint32_t>(fatByteOffset % bpb_.bytesPerSector);

    auto* sectorBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(bpb_.bytesPerSector));
    if (!sectorBuf) {
        return false;
    }
    const bool ok = readSectorsImpl(device_, bpb_.bytesPerSector, fatStartSector_ + fatSectorOffset, 1, sectorBuf);
    uint32_t raw = 0;
    if (ok) {
        memcpy(&raw, sectorBuf + byteOffsetInSector, sizeof(raw));
    }
    kernel::GenericSlabAllocator::free(sectorBuf, bpb_.bytesPerSector);
    if (!ok) {
        return false;
    }

    const uint32_t entry = raw & kFatEntryMask;
    if (entry == 0 || entry == kFatBadCluster || entry >= kFatEocMin) {
        return false;  // free/bad/EOC - 호출부가 "체인 끝"으로 처리
    }
    *outNext = entry;
    return true;
}

bool Fat32Volume::findDirEntry(uint32_t dirFirstCluster, const char* name, uint32_t nameLen, ResolvedEntry* out) {
    char normalized[11];
    kNormalizeTo83(name, nameLen, normalized);

    auto* clusterBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(bytesPerCluster_));
    if (!clusterBuf) {
        return false;
    }

    bool found = false;
    bool stop = false;
    uint32_t cluster = dirFirstCluster;
    while (!stop) {
        uint32_t sector = 0;
        if (!clusterToSector(cluster, &sector) ||
            !readSectorsImpl(device_, bpb_.bytesPerSector, sector, bpb_.sectorsPerCluster, clusterBuf)) {
            break;
        }
        const uint32_t entriesPerCluster = bytesPerCluster_ / sizeof(DirEntry);
        const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf);
        for (uint32_t i = 0; i < entriesPerCluster; ++i) {
            const DirEntry& e = entries[i];
            const uint8_t firstByte = static_cast<uint8_t>(e.name[0]);
            if (firstByte == kNameFreeRestMarker) {
                stop = true;
                break;
            }
            if (firstByte == kNameDeletedMarker) {
                continue;
            }
            if (e.attr == kAttrLongName || (e.attr & kAttrVolumeId) != 0) {
                continue;  // LFN/볼륨 라벨 - §2 스코프 컷(짧은 이름만 지원)
            }
            if (kNameMatches(e, normalized)) {
                out->firstCluster = kFatFirstCluster(e);
                out->isDir = (e.attr & kAttrDirectory) != 0;
                out->fileSize = out->isDir ? 0 : e.fileSize;
                found = true;
                stop = true;
                break;
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

    kernel::GenericSlabAllocator::free(clusterBuf, bytesPerCluster_);
    return found;
}

bool Fat32Volume::resolvePath(const char* path, uint32_t pathLen, ResolvedEntry* out) {
    uint32_t currentCluster = rootFirstCluster();
    bool currentIsDir = true;
    ResolvedEntry entry;
    entry.firstCluster = currentCluster;
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
        if (!findDirEntry(currentCluster, path + segStart, segLen, &entry)) {
            return false;
        }
        currentCluster = entry.firstCluster;
        currentIsDir = entry.isDir;
    }

    *out = entry;
    return true;
}

uint32_t Fat32Volume::readData(uint32_t firstCluster, uint64_t fileSize, uint64_t offset, void* buf, uint32_t len,
                                bool* outOk) {
    if (offset >= fileSize) {
        *outOk = true;
        return 0;  // EOF
    }
    uint64_t remaining = fileSize - offset;
    if (remaining > len) {
        remaining = len;
    }

    auto* clusterBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(bytesPerCluster_));
    if (!clusterBuf) {
        *outOk = false;
        return 0;
    }

    // 목표 오프셋이 속한 클러스터까지 체인을 선형으로 따라간다(v1은
    // 캐시 없음 - §4.2가 이미 "순차 접근 최적화는 구현 세션 재량"으로
    // 열어 둔 부분, 정직하게 단순한 버전).
    uint32_t cluster = firstCluster;
    uint64_t clusterStartOffset = 0;
    while (clusterStartOffset + bytesPerCluster_ <= offset) {
        uint32_t next = 0;
        if (!nextCluster(cluster, &next)) {
            kernel::GenericSlabAllocator::free(clusterBuf, bytesPerCluster_);
            *outOk = true;
            return 0;  // 파일 크기보다 짧은 체인 - 손상되었거나 예상 밖이나, 방어적으로 0 반환
        }
        cluster = next;
        clusterStartOffset += bytesPerCluster_;
    }

    uint32_t totalCopied = 0;
    auto* out = static_cast<uint8_t*>(buf);
    while (remaining > 0) {
        uint32_t sector = 0;
        if (!clusterToSector(cluster, &sector) ||
            !readSectorsImpl(device_, bpb_.bytesPerSector, sector, bpb_.sectorsPerCluster, clusterBuf)) {
            break;
        }
        const uint64_t curOffset = offset + totalCopied;
        const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset - clusterStartOffset);
        const uint32_t chunk = static_cast<uint32_t>(
            remaining < (bytesPerCluster_ - offsetInCluster) ? remaining : (bytesPerCluster_ - offsetInCluster));
        memcpy(out + totalCopied, clusterBuf + offsetInCluster, chunk);
        totalCopied += chunk;
        remaining -= chunk;

        if (remaining > 0) {
            uint32_t next = 0;
            if (!nextCluster(cluster, &next)) {
                break;
            }
            cluster = next;
            clusterStartOffset += bytesPerCluster_;
        }
    }

    kernel::GenericSlabAllocator::free(clusterBuf, bytesPerCluster_);
    *outOk = true;
    return totalCopied;
}

bool Fat32Volume::readdirAt(uint32_t dirFirstCluster, uint64_t index, char* nameOut, uint32_t nameOutCap,
                             uint32_t* outNameLen, bool* outIsDir, uint32_t* outFirstCluster) {
    auto* clusterBuf = static_cast<uint8_t*>(kernel::GenericSlabAllocator::alloc(bytesPerCluster_));
    if (!clusterBuf) {
        return false;
    }

    uint64_t seen = 0;
    bool found = false;
    bool stop = false;
    uint32_t cluster = dirFirstCluster;
    while (!stop) {
        uint32_t sector = 0;
        if (!clusterToSector(cluster, &sector) ||
            !readSectorsImpl(device_, bpb_.bytesPerSector, sector, bpb_.sectorsPerCluster, clusterBuf)) {
            break;
        }
        const uint32_t entriesPerCluster = bytesPerCluster_ / sizeof(DirEntry);
        const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf);
        for (uint32_t i = 0; i < entriesPerCluster; ++i) {
            const DirEntry& e = entries[i];
            const uint8_t firstByte = static_cast<uint8_t>(e.name[0]);
            if (firstByte == kNameFreeRestMarker) {
                stop = true;
                break;
            }
            if (firstByte == kNameDeletedMarker) {
                continue;
            }
            if (e.attr == kAttrLongName || (e.attr & kAttrVolumeId) != 0) {
                continue;
            }
            if (seen == index) {
                // 8.3 이름을 "NAME.EXT" 형태로 되돌려 보여준다(공백 트림, 확장자 없으면 '.' 생략).
                uint32_t nameLen = 8;
                while (nameLen > 0 && e.name[nameLen - 1] == ' ') {
                    --nameLen;
                }
                uint32_t extLen = 3;
                while (extLen > 0 && e.ext[extLen - 1] == ' ') {
                    --extLen;
                }
                uint32_t written = 0;
                for (uint32_t k = 0; k < nameLen && written < nameOutCap; ++k, ++written) {
                    nameOut[written] = (static_cast<uint8_t>(e.name[0]) == kNameEscapedE5 && k == 0)
                                            ? static_cast<char>(kNameDeletedMarker)
                                            : e.name[k];
                }
                if (extLen > 0 && written < nameOutCap) {
                    nameOut[written++] = '.';
                    for (uint32_t k = 0; k < extLen && written < nameOutCap; ++k, ++written) {
                        nameOut[written] = e.ext[k];
                    }
                }
                *outNameLen = written;
                *outIsDir = (e.attr & kAttrDirectory) != 0;
                *outFirstCluster = kFatFirstCluster(e);
                found = true;
                stop = true;
                break;
            }
            ++seen;
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

    kernel::GenericSlabAllocator::free(clusterBuf, bytesPerCluster_);
    return found;
}

}  // namespace vfat
