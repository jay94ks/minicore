#include "ntfs_driver.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

// [실측으로 확인된 제약, ext4_driver.cpp/vfat_driver.cpp/exfat_driver.cpp와
// 동일 - PN-9AE5BFE4가 먼저 발견] `NtfsVolume`의 옛 무상태 메서드
// (resolvePath/readData/readdirAt + private 헬퍼 findAttribute/
// scanIndexRoot/clusterToSector/readClusters)는 재사용하지 않는다 -
// 전부 `fs::BlockDevice::readBlocks()`(Task 레벨 블로킹)를 쓰는데,
// 이건 `onExec()`(코루틴) 안에서 못 쓴다(PN-6EDED542 문서 주석 참고).
// 이 파일도 동일하게 I/O 지점마다 `co_await kernel::
// AsyncTaskCoroAwaiter(...)`를 onExec 자신의 몸체 안에 직접 박아
// 넣는 평탄화(flatten)된 버전으로 구현한다 - 순수 계산(fixup 검증/
// 속성 탐색/데이터 런 디코딩/인덱스 엔트리 파싱) 부분만 별도 함수로
// 뽑고, 실제 I/O는 onExec 한 함수 안에 있다. NTFS는 ext4와 달리
// 디렉터리 열거($INDEX_ROOT)가 MFT 레코드 하나(항상 상주) 안에서
// 완결돼 클러스터 체인을 별도로 순회할 필요가 없다 - 그래서 "레코드를
// 하나 읽고 그 안을 파싱"하는 패턴이 Open/Stat/Readdir 전부에 거의
// 그대로 반복되는데(합성 불가 제약 때문에 함수로 뽑아 공유 못 함),
// 반복되는 I/O 자체는 딱 한 번의 co_await 호출뿐이라 ext4/vfat/exfat
// 만큼 장황하지는 않다.
namespace ntfs {

namespace {

constexpr uint32_t kMaxNameUtf16 = 256;  // NTFS 파일명 최대 255 UTF-16 코드유닛 + 여유

// slab 버퍼 RAII - 다른 드라이버들과 동일한 관례.
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

kernel::AsyncTask* kSubmitReadSectors(fs::BlockDevice* device, uint32_t sectorSize, uint64_t sectorStart,
                                       uint32_t sectorCount, void* buf, fs::BlockIoResult* outResult) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || sectorSize % devBlockSize != 0) {
        return nullptr;
    }
    const kernel::uint32_t devBlocksPerSector = sectorSize / devBlockSize;
    return device->submitReadBlocks(sectorStart * devBlocksPerSector, buf, sectorCount * devBlocksPerSector,
                                     outResult);
}

// [신규, 2026-09-29, QU-9F8AD7A8 답변(A), PN-2A0981B7 항목2]
// kSubmitReadSectors와 동일한 LBA 변환의 쓰기 짝 - exfat_driver.cpp의
// kSubmitWriteSectors와 동일한 패턴. 이 드라이버 최초의 실제 온디스크
// 쓰기(Chmod의 $STANDARD_INFORMATION.fileAttributes 갱신)에 쓰인다.
kernel::AsyncTask* kSubmitWriteSectors(fs::BlockDevice* device, uint32_t sectorSize, uint64_t sectorStart,
                                        uint32_t sectorCount, const void* buf, fs::BlockIoResult* outResult) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || sectorSize % devBlockSize != 0) {
        return nullptr;
    }
    const kernel::uint32_t devBlocksPerSector = sectorSize / devBlockSize;
    return device->submitWriteBlocks(sectorStart * devBlocksPerSector, buf, sectorCount * devBlocksPerSector,
                                      outResult);
}

uint16_t kWidenAscii(char c) { return static_cast<uint16_t>(static_cast<uint8_t>(c)); }

uint64_t kReadUnsignedLe(const uint8_t* bytes, uint32_t size) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < size; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

// 오프셋 필드는 2의 보수 부호 있는 정수(§3.4) - ntfs.cpp에 있던 옛
// kReadSignedLe와 동일.
int64_t kReadSignedLe(const uint8_t* bytes, uint32_t size) {
    uint64_t raw = kReadUnsignedLe(bytes, size);
    if (size < 8 && size > 0 && (raw & (1ull << (8 * size - 1)))) {
        raw |= ~0ull << (8 * size);
    }
    return static_cast<int64_t>(raw);
}

// 데이터 런 목록에서 targetVcn을 담당하는 런을 찾는다(순수 계산, I/O
// 없음) - ntfs.cpp에 있던 옛 kResolveVcnToLcn과 동일.
bool kResolveVcnToLcn(const uint8_t* dataRuns, uint32_t dataRunsMaxLen, uint64_t targetVcn, uint64_t* outLcn,
                       bool* outSparse) {
    uint64_t currentVcn = 0;
    int64_t currentLcn = 0;
    uint32_t pos = 0;
    while (pos < dataRunsMaxLen) {
        const uint8_t header = dataRuns[pos];
        if (header == 0) {
            break;
        }
        const uint32_t lengthSize = header & 0x0F;
        const uint32_t offsetSize = (header >> 4) & 0x0F;
        ++pos;
        if (pos + lengthSize + offsetSize > dataRunsMaxLen) {
            break;
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
            if (!isSparse) {
                *outLcn = static_cast<uint64_t>(currentLcn) + (targetVcn - currentVcn);
            }
            return true;
        }
        currentVcn += runLength;
    }
    return false;
}

// recordBuf(이미 디스크에서 읽어 온 그대로, fixup 미적용)에 fixup을
// 검증+복원한다(순수 계산, I/O 없음) - ntfs.cpp에 있던 옛
// NtfsVolume::readMftRecord의 fixup 부분만 뽑은 버전. magic!="FILE"
// 이거나 fixup 배열이 손상됐으면 false.
bool kApplyFixup(uint8_t* buf, uint32_t mftRecordSize, uint32_t bytesPerSector) {
    NtfsFileRecordHeader header;
    memcpy(&header, buf, sizeof(header));
    if (memcmp(header.magic, "FILE", 4) != 0) {
        return false;
    }
    if (header.updateSequenceSize == 0 ||
        static_cast<uint32_t>(header.updateSequenceOffset) + static_cast<uint32_t>(header.updateSequenceSize) * 2 >
            mftRecordSize) {
        return false;
    }
    const uint8_t* usa = buf + header.updateSequenceOffset;
    uint16_t usn = 0;
    memcpy(&usn, usa, sizeof(usn));
    const uint32_t sectorsInRecord = header.updateSequenceSize - 1;
    for (uint32_t i = 0; i < sectorsInRecord; ++i) {
        const uint32_t sectorEndOffset = (i + 1) * bytesPerSector - 2;
        if (sectorEndOffset + 2 > mftRecordSize) {
            return false;
        }
        uint16_t sectorTail = 0;
        memcpy(&sectorTail, buf + sectorEndOffset, sizeof(sectorTail));
        if (sectorTail != usn) {
            return false;
        }
        memcpy(buf + sectorEndOffset, usa + (i + 1) * 2, sizeof(uint16_t));
    }
    return true;
}

// [신규, 2026-09-29, QU-9F8AD7A8 답변(A)] kApplyFixup의 역방향 - 이미
// fixup이 풀려(진짜 데이터가 그대로 있는) buf를 디스크에 다시 쓰기 전에
// fixup을 다시 씌운다. USN을 1 증가시켜(0은 피함 - 일부 문헌이 예약값으로
// 다루는 관례를 그대로 따름) USA[0]에 쓰고, 각 섹터의 현재(=진짜) 꼬리
// 2바이트를 USA[i+1]에 보존한 뒤 그 꼬리를 새 USN으로 덮어쓴다 -
// kApplyFixup과 정확히 반대 방향의 같은 루프.
void kInstallFixup(uint8_t* buf, uint32_t mftRecordSize, uint32_t bytesPerSector) {
    (void)mftRecordSize;  // kApplyFixup과 시그니처를 맞춰 둠(대칭) - 이미 검증된 buf라 경계 재검사는 불필요
    NtfsFileRecordHeader header;
    memcpy(&header, buf, sizeof(header));
    uint8_t* usa = buf + header.updateSequenceOffset;
    uint16_t oldUsn = 0;
    memcpy(&oldUsn, usa, sizeof(oldUsn));
    uint16_t newUsn = static_cast<uint16_t>(oldUsn + 1);
    if (newUsn == 0) {
        newUsn = 1;
    }
    memcpy(usa, &newUsn, sizeof(newUsn));
    const uint32_t sectorsInRecord = header.updateSequenceSize - 1;
    for (uint32_t i = 0; i < sectorsInRecord; ++i) {
        const uint32_t sectorEndOffset = (i + 1) * bytesPerSector - 2;
        memcpy(usa + (i + 1) * 2, buf + sectorEndOffset, sizeof(uint16_t));
        memcpy(buf + sectorEndOffset, &newUsn, sizeof(newUsn));
    }
}

// recordBuf(fixup 적용됨) 안에서 attrType과 일치하는 첫 속성을
// 찾는다(순수 계산, I/O 없음) - ntfs.cpp에 있던 옛
// NtfsVolume::findAttribute와 동일한 판별.
bool kFindAttribute(const uint8_t* recordBuf, uint32_t mftRecordSize, uint32_t attrType, bool matchUnnamedOnly,
                     const uint8_t** outAttr, uint32_t* outAttrLen) {
    NtfsFileRecordHeader header;
    memcpy(&header, recordBuf, sizeof(header));
    uint32_t pos = header.firstAttributeOffset;
    while (pos + sizeof(NtfsAttributeHeader) <= header.usedSize && pos + sizeof(NtfsAttributeHeader) <= mftRecordSize) {
        NtfsAttributeHeader attrHeader;
        memcpy(&attrHeader, recordBuf + pos, sizeof(attrHeader));
        if (attrHeader.type == kNtfsAttrTypeEnd) {
            break;
        }
        if (attrHeader.length == 0 || pos + attrHeader.length > mftRecordSize) {
            break;
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

struct NtfsParsedEntry {
    bool valid = false;
    uint64_t mftRecordNumber = 0;
    uint64_t fileSize = 0;
    bool isDir = false;
};

// 디렉터리 레코드(fixup 적용된 recordBuf, 이미 메모리에 있음 - 추가
// I/O 없음)의 $INDEX_ROOT를 훑는다(순수 계산) - ntfs.cpp에 있던 옛
// NtfsVolume::scanIndexRoot와 동일한 판별(대용량 디렉터리는 여전히
// false로 명시 거부). nameUtf16이 null이 아니면 "이름 일치 탐색",
// null이면 "index-th 조회"(targetIndex 사용, nameOut/nameOutCap에
// 채움).
bool kScanIndexRootBuf(const uint8_t* recordBuf, uint32_t mftRecordSize, const uint16_t* nameUtf16, uint32_t nameLen,
                       uint64_t targetIndex, NtfsParsedEntry* out, char* nameOut, uint32_t nameOutCap,
                       uint32_t* outNameLen) {
    const uint8_t* attr = nullptr;
    uint32_t attrLen = 0;
    if (!kFindAttribute(recordBuf, mftRecordSize, kNtfsAttrTypeIndexRoot, /*matchUnnamedOnly=*/false, &attr,
                         &attrLen)) {
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
            break;
        }
        if (entry.entryLength < sizeof(NtfsIndexEntry) || pos + entry.entryLength > entriesEnd) {
            break;
        }
        if (entry.flags & kIndexEntryHasSubnode) {
            return false;  // $INDEX_ALLOCATION 하위 노드 필요 - 1차 증분 미지원(명시적 거부)
        }

        const uint8_t* key = pos + sizeof(NtfsIndexEntry);
        NtfsFileNameContent fileName;
        memcpy(&fileName, key, sizeof(fileName));
        const auto* nameChars = reinterpret_cast<const uint16_t*>(key + sizeof(NtfsFileNameContent));

        if (fileName.nameNamespace == 2) {  // DOS 전용 8.3 별칭 - 건너뜀
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
                    out->valid = true;
                    return true;
                }
            }
        } else {
            if (seen == targetIndex) {
                out->mftRecordNumber = kNtfsMftReferenceRecordNumber(entry.mftReference);
                out->fileSize = fileName.realSize;
                out->isDir = (fileName.fileAttributes & kFileAttrDirectory) != 0;
                out->valid = true;
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

}  // namespace

bool NtfsDriver::mount(fs::BlockDevice* device, bool readOnly) {
    if (!volume_.mount(device)) {
        return false;
    }
    mounted_ = true;
    // [갱신, 2026-09-29, QU-9F8AD7A8 답변(A)] Chmod만 readOnly_ 게이트
    // 뒤에서 실제로 쓴다(Ext4Driver/ExfatDriver와 동일한 관례) - Write/
    // Mkdir/Rmdir/Unlink는 이 값과 무관하게 여전히 항상 거부(아래 각
    // case 참고).
    readOnly_ = readOnly;
    return true;
}

bool NtfsDriver::remount(bool writable) {
    // [갱신, 2026-09-29, QU-9F8AD7A8 답변(A)] Ext4Driver/ExfatDriver와
    // 동일한 관례로 전환 자체는 항상 허용한다 - Chmod만 이 플래그를
    // 소비하고, 그 외 쓰기 연산은 여전히 무조건 거부라 writable=true여도
    // 실질적 위험이 늘지 않는다.
    readOnly_ = !writable;
    return true;
}

kernel::AsyncExecCoro NtfsDriver::onExec(kernel::AsyncTask*, void* argsRaw) {
    const auto op = *static_cast<const kernel::KernelFsOpCode*>(argsRaw);
    fs::BlockDevice* device = volume_.device();
    const uint32_t bytesPerSector = volume_.bytesPerSectorValue();
    const uint32_t sectorsPerCluster = volume_.sectorsPerClusterValue();
    const uint32_t clusterSize = volume_.clusterSizeValue();
    const uint32_t mftRecordSize = volume_.mftRecordSizeValue();
    const uint64_t mftStartLcn = volume_.mftStartLcnValue();

    switch (op) {
        case kernel::KernelFsOpCode::Open: {
            auto* args = static_cast<kernel::KernelFsOpenArgs*>(argsRaw);
            uint64_t currentRecord = kNtfsRootDirectoryRecord;
            bool currentIsDir = true;
            bool failed = false;

            uint32_t pos = 0;
            while (pos < args->relPathLen && !failed) {
                while (pos < args->relPathLen && args->relPath[pos] == '/') {
                    ++pos;
                }
                if (pos >= args->relPathLen) {
                    break;
                }
                const uint32_t segStart = pos;
                while (pos < args->relPathLen && args->relPath[pos] != '/') {
                    ++pos;
                }
                const uint32_t segLen = pos - segStart;
                if (!currentIsDir || segLen > kMaxNameUtf16) {
                    failed = true;
                    break;
                }
                uint16_t query[kMaxNameUtf16];
                for (uint32_t i = 0; i < segLen; ++i) {
                    query[i] = kWidenAscii(args->relPath[segStart + i]);
                }

                SlabBuf recordBuf(mftRecordSize);
                if (!recordBuf) {
                    failed = true;
                    break;
                }
                bool ioFailed = false;
                {
                    const uint64_t recordByteOffset =
                        mftStartLcn * clusterSize + currentRecord * static_cast<uint64_t>(mftRecordSize);
                    const uint64_t sector = recordByteOffset / bytesPerSector;
                    const uint32_t sectorCount = mftRecordSize / bytesPerSector;
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorCount, recordBuf.get(), &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                    } else {
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            ioFailed = true;
                        }
                    }
                }
                if (ioFailed || !kApplyFixup(recordBuf.get(), mftRecordSize, bytesPerSector)) {
                    failed = true;
                    break;
                }

                NtfsParsedEntry matched;
                if (!kScanIndexRootBuf(recordBuf.get(), mftRecordSize, query, segLen, 0, &matched, nullptr, 0,
                                        nullptr)) {
                    failed = true;
                    break;
                }
                currentRecord = matched.mftRecordNumber;
                currentIsDir = matched.isDir;
            }

            if (failed) {
                args->result = kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
            } else {
                args->result =
                    kernel::OpenResult{kernel::FileHandle{currentRecord}, currentIsDir, kernel::VfsError::None};
            }
            break;
        }

        case kernel::KernelFsOpCode::Close: {
            // 무상태(FileHandle 자체가 MFT 레코드 번호) - 다른 드라이버와
            // 동일하게 따로 정리할 자원이 없다.
            break;
        }

        case kernel::KernelFsOpCode::Read: {
            auto* args = static_cast<kernel::KernelFsReadArgs*>(argsRaw);
            const uint64_t mftRecordNumber = args->handle.value;

            SlabBuf recordBuf(mftRecordSize);
            if (!recordBuf) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            bool ioFailed = false;
            {
                const uint64_t recordByteOffset =
                    mftStartLcn * clusterSize + mftRecordNumber * static_cast<uint64_t>(mftRecordSize);
                const uint64_t sector = recordByteOffset / bytesPerSector;
                const uint32_t sectorCount = mftRecordSize / bytesPerSector;
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, bytesPerSector, sector, sectorCount, recordBuf.get(), &ioResult);
                if (!ioTask) {
                    ioFailed = true;
                } else {
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                    }
                }
            }
            if (ioFailed || !kApplyFixup(recordBuf.get(), mftRecordSize, bytesPerSector)) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }

            const uint8_t* attr = nullptr;
            uint32_t attrLen = 0;
            if (!kFindAttribute(recordBuf.get(), mftRecordSize, kNtfsAttrTypeData, /*matchUnnamedOnly=*/true, &attr,
                                 &attrLen)) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            NtfsAttributeHeader attrHeader;
            memcpy(&attrHeader, attr, sizeof(attrHeader));

            if (attrHeader.nonResident == 0) {
                NtfsResidentAttrTail tail;
                memcpy(&tail, attr + sizeof(NtfsAttributeHeader), sizeof(tail));
                if (args->offset >= tail.contentLength) {
                    args->result = kernel::ReadResult{0, kernel::VfsError::None};  // EOF
                    break;
                }
                uint64_t remaining = tail.contentLength - args->offset;
                if (remaining > args->len) {
                    remaining = args->len;
                }
                memcpy(args->buf, attr + tail.contentOffset + args->offset, remaining);
                args->result = kernel::ReadResult{static_cast<uint32_t>(remaining), kernel::VfsError::None};
                break;
            }

            NtfsNonResidentAttrTail tail;
            memcpy(&tail, attr + sizeof(NtfsAttributeHeader), sizeof(tail));
            if (args->offset >= tail.realSize) {
                args->result = kernel::ReadResult{0, kernel::VfsError::None};  // EOF
                break;
            }
            uint64_t remaining = tail.realSize - args->offset;
            if (remaining > args->len) {
                remaining = args->len;
            }
            const uint8_t* dataRuns = attr + tail.dataRunsOffset;
            const uint32_t dataRunsMaxLen = attrHeader.length - tail.dataRunsOffset;

            SlabBuf clusterBuf(clusterSize);
            if (!clusterBuf) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            uint32_t totalCopied = 0;
            auto* out = static_cast<uint8_t*>(args->buf);
            bool dataIoFailed = false;
            while (remaining > 0 && !dataIoFailed) {
                const uint64_t curOffset = args->offset + totalCopied;
                const uint64_t targetVcn = curOffset / clusterSize;
                const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset % clusterSize);
                uint64_t lcn = 0;
                bool sparse = false;
                if (!kResolveVcnToLcn(dataRuns, dataRunsMaxLen, targetVcn, &lcn, &sparse)) {
                    break;  // 손상되었거나 realSize보다 짧은 런 목록 - 지금까지 복사된 만큼만 반환
                }
                const uint32_t chunk = static_cast<uint32_t>(
                    remaining < (clusterSize - offsetInCluster) ? remaining : (clusterSize - offsetInCluster));
                if (sparse) {
                    memset(out + totalCopied, 0, chunk);  // sparse 런 - 논리적으로 0(§3.4)
                } else {
                    const uint64_t sector = lcn * sectorsPerCluster;
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask = kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster,
                                                                    clusterBuf.get(), &ioResult);
                    if (!ioTask) {
                        dataIoFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        dataIoFailed = true;
                        break;
                    }
                    memcpy(out + totalCopied, clusterBuf.get() + offsetInCluster, chunk);
                }
                totalCopied += chunk;
                remaining -= chunk;
            }
            args->result = kernel::ReadResult{totalCopied,
                                               dataIoFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None};
            break;
        }

        case kernel::KernelFsOpCode::Write: {
            // libntfs 1차 증분은 읽기 전용이라 쓰기 경로가 없다 -
            // 조용히 무시하지 않고 명시적으로 거부한다.
            auto* args = static_cast<kernel::KernelFsWriteArgs*>(argsRaw);
            args->bytesWritten = 0;
            args->error = kernel::VfsError::PermissionDenied;
            break;
        }

        case kernel::KernelFsOpCode::Stat: {
            auto* args = static_cast<kernel::KernelFsStatArgs*>(argsRaw);
            uint64_t currentRecord = kNtfsRootDirectoryRecord;
            bool currentIsDir = true;
            uint64_t currentFileSize = 0;
            bool failed = false;

            uint32_t pos = 0;
            while (pos < args->relPathLen && !failed) {
                while (pos < args->relPathLen && args->relPath[pos] == '/') {
                    ++pos;
                }
                if (pos >= args->relPathLen) {
                    break;
                }
                const uint32_t segStart = pos;
                while (pos < args->relPathLen && args->relPath[pos] != '/') {
                    ++pos;
                }
                const uint32_t segLen = pos - segStart;
                if (!currentIsDir || segLen > kMaxNameUtf16) {
                    failed = true;
                    break;
                }
                uint16_t query[kMaxNameUtf16];
                for (uint32_t i = 0; i < segLen; ++i) {
                    query[i] = kWidenAscii(args->relPath[segStart + i]);
                }

                SlabBuf recordBuf(mftRecordSize);
                if (!recordBuf) {
                    failed = true;
                    break;
                }
                bool ioFailed = false;
                {
                    const uint64_t recordByteOffset =
                        mftStartLcn * clusterSize + currentRecord * static_cast<uint64_t>(mftRecordSize);
                    const uint64_t sector = recordByteOffset / bytesPerSector;
                    const uint32_t sectorCount = mftRecordSize / bytesPerSector;
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorCount, recordBuf.get(), &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                    } else {
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            ioFailed = true;
                        }
                    }
                }
                if (ioFailed || !kApplyFixup(recordBuf.get(), mftRecordSize, bytesPerSector)) {
                    failed = true;
                    break;
                }

                NtfsParsedEntry matched;
                if (!kScanIndexRootBuf(recordBuf.get(), mftRecordSize, query, segLen, 0, &matched, nullptr, 0,
                                        nullptr)) {
                    failed = true;
                    break;
                }
                currentRecord = matched.mftRecordNumber;
                currentIsDir = matched.isDir;
                currentFileSize = matched.fileSize;
            }

            if (failed) {
                args->error = kernel::VfsError::NotFound;
            } else {
                // NTFS도 FAT/exFAT과 마찬가지로 크기/디렉터리 여부가
                // 부모 디렉터리 인덱스 엔트리($FILE_NAME) 자신에 이미
                // 있어 ext4의 Stat과 달리 별도 "타깃 재조회"가 필요 없다.
                args->size = currentFileSize;
                args->type = currentIsDir ? kernel::FileType::Directory : kernel::FileType::Regular;
                // [신규, 2026-09-28, SP-9039F955 §3.2] NTFS는 ACL 파싱이
                // 전혀 없다(§1) - FAT류와 동일하게 마운트한 유저 소유로
                // 취급한다.
                args->uid = args->mountUid;
                args->gid = args->mountGid;

                // [신규, 2026-09-29, QU-9F8AD7A8 답변(A), PN-2A0981B7 항목2]
                // Chmod와 대칭 - 타깃 자신의 MFT 레코드를 열어
                // $STANDARD_INFORMATION.fileAttributes의 READONLY 비트를
                // 읽어 owner-write 여부에 반영한다(FAT의 kAttrReadOnly와
                // 동일한 취급). 이 재조회가 실패해도(포맷 이상 등) Stat
                // 자체를 실패시키지 않고 "쓰기 가능"으로 안전하게 폴백한다 -
                // 어차피 §1대로 Write류는 항상 거부되므로 잘못된 폴백이
                // 실제 쓰기로 이어지지 않는다.
                bool readOnlyFlag = false;
                {
                    SlabBuf targetBuf(mftRecordSize);
                    bool targetIoFailed = !targetBuf;
                    if (!targetIoFailed) {
                        const uint64_t recordByteOffset =
                            mftStartLcn * clusterSize + currentRecord * static_cast<uint64_t>(mftRecordSize);
                        const uint64_t sector = recordByteOffset / bytesPerSector;
                        const uint32_t sectorCount = mftRecordSize / bytesPerSector;
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask =
                            kSubmitReadSectors(device, bytesPerSector, sector, sectorCount, targetBuf.get(), &ioResult);
                        if (!ioTask) {
                            targetIoFailed = true;
                        } else {
                            co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                            if (!ioResult.ok) {
                                targetIoFailed = true;
                            }
                        }
                    }
                    if (!targetIoFailed && kApplyFixup(targetBuf.get(), mftRecordSize, bytesPerSector)) {
                        const uint8_t* attr = nullptr;
                        uint32_t attrLen = 0;
                        if (kFindAttribute(targetBuf.get(), mftRecordSize, kNtfsAttrTypeStandardInformation,
                                            /*matchUnnamedOnly=*/true, &attr, &attrLen)) {
                            NtfsResidentAttrTail tail;
                            memcpy(&tail, attr + sizeof(NtfsAttributeHeader), sizeof(tail));
                            uint32_t fileAttributes = 0;
                            memcpy(&fileAttributes, attr + tail.contentOffset + offsetof(NtfsStandardInfoContent, fileAttributes),
                                   sizeof(fileAttributes));
                            readOnlyFlag = (fileAttributes & kFileAttrReadOnly) != 0;
                        }
                    }
                }

                const uint16_t ownerWriteBit = readOnlyFlag ? 0 : kernel::kPermOwnerWrite;
                args->mode = currentIsDir
                                 ? (kernel::kPermOwnerRead | ownerWriteBit | kernel::kPermOwnerExec |
                                    kernel::kPermGroupRead | kernel::kPermGroupExec | kernel::kPermOtherRead |
                                    kernel::kPermOtherExec)   // 0755/0555
                                 : (kernel::kPermOwnerRead | ownerWriteBit | kernel::kPermGroupRead |
                                    kernel::kPermOtherRead);  // 0644/0444
                args->error = kernel::VfsError::None;
            }
            break;
        }

        case kernel::KernelFsOpCode::Mkdir: {
            static_cast<kernel::KernelFsMkdirArgs*>(argsRaw)->error = kernel::VfsError::PermissionDenied;
            break;
        }
        case kernel::KernelFsOpCode::Rmdir: {
            static_cast<kernel::KernelFsRmdirArgs*>(argsRaw)->error = kernel::VfsError::PermissionDenied;
            break;
        }
        case kernel::KernelFsOpCode::Unlink: {
            static_cast<kernel::KernelFsUnlinkArgs*>(argsRaw)->error = kernel::VfsError::PermissionDenied;
            break;
        }
        // [구현, 2026-09-29, QU-9F8AD7A8 답변(A), PN-2A0981B7 항목2]
        // Chmod - exFAT의 Chmod와 정확히 같은 성격("이미 존재하는 상주
        // 속성의 제자리 갱신")이라 SP-AA6DF406 §1의 읽기전용 경계에서
        // 예외로 인정됐다(§1이 우려한 MFT 비트맵 할당/속성 상주->비상주
        // 확장/B+ 트리 재조정 중 어디에도 해당하지 않음). 타깃 자신의
        // MFT 레코드를 열어 $STANDARD_INFORMATION.fileAttributes의
        // READONLY 비트만 고쳐 쓰고 fixup을 재계산해 같은 레코드 크기로
        // 그대로 다시 쓴다 - 이 드라이버 역사상 첫 온디스크 쓰기.
        case kernel::KernelFsOpCode::Chmod: {
            auto* args = static_cast<kernel::KernelFsChmodArgs*>(argsRaw);
            if (readOnly_) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            if (args->mode & kernel::kPermSpecialS) {
                args->error = kernel::VfsError::NotSupported;
                break;
            }
            if (args->callerUid != kernel::kRootUid && args->callerUid != args->mountUid) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }

            // 경로를 걸어 타깃 자신의 MFT 레코드 번호를 찾는다(Open/Stat과
            // 동일한 순회 - 코루틴 합성 불가 제약으로 다시 반복).
            uint64_t currentRecord = kNtfsRootDirectoryRecord;
            bool currentIsDir = true;
            bool failed = false;
            bool haveTarget = false;
            uint32_t pos = 0;
            while (pos < args->relPathLen && !failed) {
                while (pos < args->relPathLen && args->relPath[pos] == '/') {
                    ++pos;
                }
                if (pos >= args->relPathLen) {
                    break;
                }
                const uint32_t segStart = pos;
                while (pos < args->relPathLen && args->relPath[pos] != '/') {
                    ++pos;
                }
                const uint32_t segLen = pos - segStart;
                if (!currentIsDir || segLen > kMaxNameUtf16) {
                    failed = true;
                    break;
                }
                uint16_t query[kMaxNameUtf16];
                for (uint32_t i = 0; i < segLen; ++i) {
                    query[i] = kWidenAscii(args->relPath[segStart + i]);
                }

                SlabBuf recordBuf(mftRecordSize);
                if (!recordBuf) {
                    failed = true;
                    break;
                }
                bool ioFailed = false;
                {
                    const uint64_t recordByteOffset =
                        mftStartLcn * clusterSize + currentRecord * static_cast<uint64_t>(mftRecordSize);
                    const uint64_t sector = recordByteOffset / bytesPerSector;
                    const uint32_t sectorCount = mftRecordSize / bytesPerSector;
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorCount, recordBuf.get(), &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                    } else {
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            ioFailed = true;
                        }
                    }
                }
                if (ioFailed || !kApplyFixup(recordBuf.get(), mftRecordSize, bytesPerSector)) {
                    failed = true;
                    break;
                }

                NtfsParsedEntry matched;
                if (!kScanIndexRootBuf(recordBuf.get(), mftRecordSize, query, segLen, 0, &matched, nullptr, 0,
                                        nullptr)) {
                    failed = true;
                    break;
                }
                currentRecord = matched.mftRecordNumber;
                currentIsDir = matched.isDir;
                haveTarget = true;
            }

            if (failed || !haveTarget) {
                // haveTarget==false - 빈 경로(루트 자신)는 exFAT과 동일하게
                // 대상 없음으로 취급(1차 증분은 루트 자체의 Chmod를 다루지
                // 않음).
                args->error = kernel::VfsError::NotFound;
                break;
            }

            SlabBuf targetBuf(mftRecordSize);
            if (!targetBuf) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            bool ioFailed = false;
            {
                const uint64_t recordByteOffset =
                    mftStartLcn * clusterSize + currentRecord * static_cast<uint64_t>(mftRecordSize);
                const uint64_t sector = recordByteOffset / bytesPerSector;
                const uint32_t sectorCount = mftRecordSize / bytesPerSector;
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, bytesPerSector, sector, sectorCount, targetBuf.get(), &ioResult);
                if (!ioTask) {
                    ioFailed = true;
                } else {
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                    }
                }
            }
            if (ioFailed || !kApplyFixup(targetBuf.get(), mftRecordSize, bytesPerSector)) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            const uint8_t* attr = nullptr;
            uint32_t attrLen = 0;
            if (!kFindAttribute(targetBuf.get(), mftRecordSize, kNtfsAttrTypeStandardInformation,
                                 /*matchUnnamedOnly=*/true, &attr, &attrLen)) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            const uint32_t attrOffset = static_cast<uint32_t>(attr - targetBuf.get());
            NtfsResidentAttrTail tail;
            memcpy(&tail, targetBuf.get() + attrOffset + sizeof(NtfsAttributeHeader), sizeof(tail));
            const uint32_t fileAttributesOffset =
                attrOffset + tail.contentOffset + static_cast<uint32_t>(offsetof(NtfsStandardInfoContent, fileAttributes));
            if (fileAttributesOffset + sizeof(uint32_t) > mftRecordSize) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            uint32_t fileAttributes = 0;
            memcpy(&fileAttributes, targetBuf.get() + fileAttributesOffset, sizeof(fileAttributes));
            if (args->mode & kernel::kPermOwnerWrite) {
                fileAttributes &= ~kFileAttrReadOnly;
            } else {
                fileAttributes |= kFileAttrReadOnly;
            }
            memcpy(targetBuf.get() + fileAttributesOffset, &fileAttributes, sizeof(fileAttributes));

            kInstallFixup(targetBuf.get(), mftRecordSize, bytesPerSector);

            const uint64_t recordByteOffset =
                mftStartLcn * clusterSize + currentRecord * static_cast<uint64_t>(mftRecordSize);
            const uint64_t sector = recordByteOffset / bytesPerSector;
            const uint32_t sectorCount = mftRecordSize / bytesPerSector;
            fs::BlockIoResult writeIo;
            kernel::AsyncTask* writeTask =
                kSubmitWriteSectors(device, bytesPerSector, sector, sectorCount, targetBuf.get(), &writeIo);
            if (!writeTask) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            co_await kernel::AsyncTaskCoroAwaiter(writeTask);
            if (!writeIo.ok) {
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            args->error = kernel::VfsError::None;
            break;
        }
        case kernel::KernelFsOpCode::Chown: {
            static_cast<kernel::KernelFsChownArgs*>(argsRaw)->error = kernel::VfsError::NotSupported;
            break;
        }

        case kernel::KernelFsOpCode::Readdir: {
            auto* args = static_cast<kernel::KernelFsReaddirArgs*>(argsRaw);
            const uint64_t dirRecordNumber = args->dirHandle.value;

            SlabBuf recordBuf(mftRecordSize);
            if (!recordBuf) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            bool ioFailed = false;
            {
                const uint64_t recordByteOffset =
                    mftStartLcn * clusterSize + dirRecordNumber * static_cast<uint64_t>(mftRecordSize);
                const uint64_t sector = recordByteOffset / bytesPerSector;
                const uint32_t sectorCount = mftRecordSize / bytesPerSector;
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, bytesPerSector, sector, sectorCount, recordBuf.get(), &ioResult);
                if (!ioTask) {
                    ioFailed = true;
                } else {
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                    }
                }
            }
            if (ioFailed || !kApplyFixup(recordBuf.get(), mftRecordSize, bytesPerSector)) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            NtfsParsedEntry parsed;
            const bool found = kScanIndexRootBuf(recordBuf.get(), mftRecordSize, nullptr, 0, args->index, &parsed,
                                                  args->entry.name, sizeof(args->entry.name), &args->entry.nameLength);
            if (found) {
                args->entry.isDirectory = parsed.isDir;
            }
            args->hasMore = found;
            args->error = kernel::VfsError::None;
            break;
        }
    }
    co_return;
}

}  // namespace ntfs
