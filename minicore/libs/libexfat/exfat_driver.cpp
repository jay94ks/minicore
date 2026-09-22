#include "exfat_driver.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

// [실측으로 확인된 제약, ext4_driver.cpp/vfat_driver.cpp와 동일 -
// PN-9AE5BFE4/PN-EBAEA67B가 먼저 발견] `ExfatVolume`의 옛 무상태
// 메서드(resolvePath/readdirAt + public readData)는 재사용하지 않는다
// - 전부 `fs::BlockDevice::readBlocks()`(Task 레벨 블로킹)를 쓰는데,
// 이건 `onExec()`(코루틴) 안에서 못 쓴다(PN-6EDED542 문서 주석 참고).
// `kernel::AsyncTaskCoroAwaiter`로 바꿔도 별도 코루틴 함수로 감싼
// I/O 헬퍼를 onExec이 다시 co_await하는 합성은 안전하게 재개되지
// 않아(SP-F682B889 §9.5 항목3), 이 파일도 동일하게 I/O 지점마다
// `co_await kernel::AsyncTaskCoroAwaiter(...)`를 onExec 자신의 몸체
// 안에 직접 박아 넣는 평탄화(flatten)된 버전으로 구현한다 - 순수 계산
// (버퍼 해석) 부분만 별도 함수로 뽑고, 실제 I/O 오케스트레이션은 전부
// onExec 한 함수 안에 있다. `Open`/`Stat`/`Readdir` 셋 다 디렉터리
// 스캔(엔트리 집합 조립 + 클러스터/FAT 체인 순회)이 필요해 그 로직이
// 세 곳에 거의 그대로 중복되는데, 위 제약(합성 불가) 때문에 함수로
// 뽑아 공유할 수 없어 의도적으로 감수한 중복이다(ext4_driver.cpp/
// vfat_driver.cpp와 동일한 관례).
namespace exfat {

namespace {

constexpr uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

constexpr uint32_t kMaxEntrySetSlots = 19;  // Primary 1 + Secondary 최대 18(스펙상 secondaryCount 상한)
constexpr uint32_t kMaxNameUtf16 = 256;     // secondaryCount<=18이면 이름 조각 최대 17개*15자=255자로 충분

// slab 버퍼 RAII - ext4_driver.cpp/vfat_driver.cpp의 SlabBuf와 동일한 관례.
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

// 장치 블록 크기가 뭐든 섹터 단위로 co_await 가능한 읽기 AsyncTask를
// 제출한다 - ext4_driver.cpp의 kSubmitReadExtBlocks/vfat_driver.cpp의
// kSubmitReadSectors와 동일한 LBA 변환.
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

// 클러스터 번호를 장치 섹터 번호로 바꾼다(순수 계산) - exfat.cpp에
// 있던 ExfatVolume::clusterToSector와 동일한 판별.
bool kClusterToSector(uint32_t clusterHeapOffset, uint32_t sectorsPerCluster, uint32_t cluster, uint32_t* outSector) {
    if (cluster < kExfatFirstCluster) {
        return false;
    }
    *outSector = clusterHeapOffset + (cluster - kExfatFirstCluster) * sectorsPerCluster;
    return true;
}

// cluster번째 FAT 엔트리(32비트)가 있는 섹터/그 섹터 안 바이트
// 오프셋을 계산한다(순수 계산) - 실제로 그 섹터를 읽는 건 호출부
// (onExec)의 몫(합성 불가 제약, 파일 상단 문서 주석 참고).
void kFatEntryLocation(uint32_t fatOffset, uint32_t sectorSize, uint32_t cluster, uint32_t* outSector,
                        uint32_t* outByteOffset) {
    const uint64_t fatByteOffset = static_cast<uint64_t>(cluster) * 4;
    *outSector = fatOffset + static_cast<uint32_t>(fatByteOffset / sectorSize);
    *outByteOffset = static_cast<uint32_t>(fatByteOffset % sectorSize);
}

// raw 32비트 FAT 엔트리를 해석한다(순수 계산, I/O 없음) - exfat.cpp에
// 있던 ExfatVolume::nextCluster와 동일한 판별(free/bad/EOC를 구분하지
// 않고 전부 "체인 끝"으로 통일 - 원본과 동일한 관례).
bool kInterpretFatEntry(uint32_t raw, uint32_t* outNext) {
    if (raw == kExfatFatFree || raw == kExfatBadCluster || raw == kExfatEoc) {
        return false;
    }
    *outNext = raw;
    return true;
}

void kUpcaseCodeUnits(const uint16_t* upcaseTable, uint16_t* codeUnits, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        codeUnits[i] = upcaseTable[codeUnits[i]];
    }
}

// ASCII 바이트 하나를 UTF-16 코드유닛으로 폭 확장 - v1은 ASCII 경로만
// 지원(exfat_driver.h 문서 주석 참고, 옛 ExfatVolume::resolvePath와
// 동일한 전제).
uint16_t kWidenAscii(char c) { return static_cast<uint16_t>(static_cast<uint8_t>(c)); }

// 엔트리 집합 하나(이미 조립된 연속 바이트, 클러스터 경계를 넘었어도
// 호출부가 이미 이어붙여 둔 상태)를 해석한다(순수 계산, I/O 없음) -
// exfat.cpp에 있던 옛 ExfatVolume::scanDirectory의 파싱 부분만 뽑은
// 버전. valid=false면 entrySet[32]가 Stream Extension이 아닌 손상된
// 집합(호출부가 건너뜀).
struct ExfatParsedEntry {
    bool valid = false;
    bool isDir = false;
    bool noFatChain = false;
    uint32_t firstCluster = 0;
    uint64_t fileSize = 0;
    uint16_t nameUtf16[kMaxNameUtf16];
    uint32_t nameLen = 0;
};

// [신규, 2026-09-23, RM-F2DAFF66 §3 점검 중 발견 - SP-F1987EF8 §3.5가
// "읽기 시 검증"을 확정된 설계로 명시했으나 이번 증분(PN-09970F05)이
// 놓쳤던 부분] exFAT 디렉터리 엔트리 집합 체크섬 - 이 프로젝트가 새로
// 고안한 알고리즘이 아니다. Microsoft exFAT 스펙/Linux 커널
// `fs/exfat/exfat_fs.h`의 `exfat_calc_chksum16` 관례 그대로: 집합
// 전체(Primary+Secondary, totalEntries*32바이트)를 순서대로 훑으며
// 매 바이트마다 `((sum&1)?0x8000:0)+(sum>>1)+byte`로 누적하되, **첫
// 엔트리(Primary) 안의 바이트 2-3(setChecksum 필드 자신)만 건너뛴다**
// (그 뒤 엔트리들의 바이트 2-3은 건너뛰지 않음 - 스킵은 오직 자기
// 자신을 참조하는 최초 필드 자리에만 적용).
uint16_t kExfatEntrySetChecksum(const uint8_t* entrySet, uint32_t totalEntries) {
    uint16_t sum = 0;
    for (uint32_t entryIdx = 0; entryIdx < totalEntries; ++entryIdx) {
        const uint8_t* e = entrySet + entryIdx * 32;
        for (uint32_t b = 0; b < 32; ++b) {
            if (entryIdx == 0 && (b == 2 || b == 3)) {
                continue;
            }
            sum = static_cast<uint16_t>(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + e[b]);
        }
    }
    return sum;
}

ExfatParsedEntry kParseFileEntrySet(const uint8_t* entrySet, uint32_t secondaryCount) {
    ExfatParsedEntry result;
    if (secondaryCount < 1 || entrySet[32] != kExfatEntryTypeStreamExt) {
        return result;
    }
    ExfatFileDirEntry primary;
    memcpy(&primary, entrySet, sizeof(primary));
    // [신규, RM-F2DAFF66 §3] §3.5가 명시한 무결성 검증 - 계산값이
    // 저장된 setChecksum과 다르면 손상된 엔트리 집합으로 취급하고
    // 거부한다(호출부가 이미 "invalid면 건너뛴다"는 관례를 갖고
    // 있어 별도 에러 코드 없이 자연스럽게 합류).
    if (kExfatEntrySetChecksum(entrySet, secondaryCount + 1) != primary.setChecksum) {
        return result;
    }
    ExfatStreamExtEntry stream;
    memcpy(&stream, entrySet + 32, sizeof(stream));
    result.isDir = (primary.fileAttributes & kFileAttrDirectory) != 0;
    result.noFatChain = (stream.generalSecondaryFlags & kNoFatChainBit) != 0;
    result.firstCluster = stream.firstCluster;
    result.fileSize = stream.dataLength;

    uint32_t assembledLen = 0;
    for (uint32_t i = 1; i < secondaryCount && assembledLen < stream.nameLength; ++i) {
        const uint8_t* sec = entrySet + (i + 1) * 32;
        if (sec[0] != kExfatEntryTypeFileName) {
            continue;
        }
        ExfatFileNameEntry nameEntry;
        memcpy(&nameEntry, sec, sizeof(nameEntry));
        for (uint32_t k = 0; k < 15 && assembledLen < stream.nameLength && assembledLen < kMaxNameUtf16; ++k) {
            result.nameUtf16[assembledLen++] = nameEntry.fileName[k];
        }
    }
    result.nameLen = assembledLen;
    result.valid = true;
    return result;
}

// FileHandle 인코딩(exfat_driver.h 문서 주석 참고) - 하위 32비트:
// firstCluster, 상위 31비트(32~62): fileSize(v1 절단, 최대 2GiB-1),
// 최상위 1비트(63): noFatChain.
constexpr uint64_t kHandleClusterMask = 0xFFFFFFFFull;
constexpr uint32_t kHandleSizeShift = 32;
constexpr uint64_t kHandleSizeMask = 0x7FFFFFFFull;
constexpr uint64_t kHandleNoFatChainBit = 1ull << 63;

uint64_t kEncodeHandle(uint32_t cluster, uint64_t fileSize, bool noFatChain) {
    uint64_t value =
        (static_cast<uint64_t>(cluster) & kHandleClusterMask) | ((fileSize & kHandleSizeMask) << kHandleSizeShift);
    if (noFatChain) {
        value |= kHandleNoFatChainBit;
    }
    return value;
}

void kDecodeHandle(uint64_t value, uint32_t* outCluster, uint64_t* outFileSize, bool* outNoFatChain) {
    *outCluster = static_cast<uint32_t>(value & kHandleClusterMask);
    *outFileSize = (value >> kHandleSizeShift) & kHandleSizeMask;
    *outNoFatChain = (value & kHandleNoFatChainBit) != 0;
}

}  // namespace

bool ExfatDriver::mount(fs::BlockDevice* device, bool readOnly) {
    if (!volume_.mount(device)) {
        return false;
    }
    mounted_ = true;
    readOnly_ = readOnly;
    return true;
}

bool ExfatDriver::remount(bool writable) {
    // libexfat 1차 증분은 쓰기 경로 자체가 없어(§4 미결) writable=true로
    // 전환해도 실질적 의미는 없다 - Ext4Driver::remount와 동일한 관례.
    readOnly_ = !writable;
    return true;
}

kernel::AsyncExecCoro ExfatDriver::onExec(kernel::AsyncTask*, void* argsRaw) {
    const auto op = *static_cast<const kernel::KernelFsOpCode*>(argsRaw);
    fs::BlockDevice* device = volume_.device();
    const uint32_t sectorSize = volume_.sectorSizeValue();
    const uint32_t clusterSize = volume_.clusterSizeValue();
    const uint32_t sectorsPerCluster = volume_.sectorsPerClusterValue();
    const uint32_t clusterHeapOffset = volume_.clusterHeapOffsetValue();
    const uint32_t fatOffset = volume_.fatOffsetValue();
    const uint16_t* upcaseTable = volume_.upcaseTablePtr();

    switch (op) {
        case kernel::KernelFsOpCode::Open: {
            auto* args = static_cast<kernel::KernelFsOpenArgs*>(argsRaw);
            uint32_t currentCluster = volume_.rootFirstCluster();
            bool currentIsDir = true;
            uint64_t currentFileSize = 0;
            bool currentNoFatChain = false;
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

                uint16_t queryUpper[kMaxNameUtf16];
                for (uint32_t i = 0; i < segLen; ++i) {
                    queryUpper[i] = kWidenAscii(args->relPath[segStart + i]);
                }
                kUpcaseCodeUnits(upcaseTable, queryUpper, segLen);

                ExfatParsedEntry matched;
                bool foundInThisDir = false;
                bool ioFailed = false;
                {
                    uint32_t cluster = currentCluster;
                    uint64_t clusterIndex = 0;
                    uint32_t posInCluster = clusterSize;  // 다음 슬롯 요청이 즉시 첫 클러스터를 읽게 함
                    const uint64_t clusterLimit =
                        currentNoFatChain ? kCeilDiv(currentFileSize, clusterSize) : ~static_cast<uint64_t>(0);
                    SlabBuf clusterBuf(clusterSize);
                    if (!clusterBuf) {
                        ioFailed = true;
                    }
                    uint8_t entrySet[kMaxEntrySetSlots * 32];
                    uint32_t entrySetFilled = 0;
                    uint32_t entrySetNeeded = 0;
                    bool dirEnded = false;

                    while (!ioFailed && !dirEnded && !foundInThisDir) {
                        if (posInCluster >= clusterSize) {
                            if (clusterIndex != 0) {
                                if (currentNoFatChain) {
                                    ++cluster;  // NoFatChain - 물리적으로 연속(§3.2)
                                } else {
                                    uint32_t fatSector = 0;
                                    uint32_t fatByteOffset = 0;
                                    kFatEntryLocation(fatOffset, sectorSize, cluster, &fatSector, &fatByteOffset);
                                    SlabBuf fatBuf(sectorSize);
                                    if (!fatBuf) {
                                        ioFailed = true;
                                        break;
                                    }
                                    fs::BlockIoResult fatIoResult;
                                    kernel::AsyncTask* fatIoTask = kSubmitReadSectors(
                                        device, sectorSize, fatSector, 1, fatBuf.get(), &fatIoResult);
                                    if (!fatIoTask) {
                                        ioFailed = true;
                                        break;
                                    }
                                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                                    if (!fatIoResult.ok) {
                                        ioFailed = true;
                                        break;
                                    }
                                    uint32_t raw = 0;
                                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                                    uint32_t next = 0;
                                    if (!kInterpretFatEntry(raw, &next)) {
                                        dirEnded = true;
                                        break;
                                    }
                                    cluster = next;
                                }
                            }
                            ++clusterIndex;
                            if (clusterIndex > clusterLimit) {
                                dirEnded = true;
                                break;
                            }
                            uint32_t sector = 0;
                            if (!kClusterToSector(clusterHeapOffset, sectorsPerCluster, cluster, &sector)) {
                                ioFailed = true;
                                break;
                            }
                            fs::BlockIoResult ioResult;
                            kernel::AsyncTask* ioTask = kSubmitReadSectors(device, sectorSize, sector,
                                                                            sectorsPerCluster, clusterBuf.get(),
                                                                            &ioResult);
                            if (!ioTask) {
                                ioFailed = true;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                            if (!ioResult.ok) {
                                ioFailed = true;
                                break;
                            }
                            posInCluster = 0;
                        }

                        uint8_t slot[32];
                        memcpy(slot, clusterBuf.get() + posInCluster, 32);
                        posInCluster += 32;

                        if (entrySetFilled == 0) {
                            if (slot[0] == kExfatEntryTypeEndOfDirectory) {
                                dirEnded = true;
                                break;
                            }
                            if (slot[0] != kExfatEntryTypeFile) {
                                continue;
                            }
                            ExfatFileDirEntry primary;
                            memcpy(&primary, slot, sizeof(primary));
                            if (primary.secondaryCount >= kMaxEntrySetSlots) {
                                ioFailed = true;
                                break;
                            }
                            memcpy(entrySet, slot, 32);
                            entrySetNeeded = primary.secondaryCount + 1;
                            entrySetFilled = 1;
                            if (entrySetFilled < entrySetNeeded) {
                                continue;
                            }
                        } else {
                            memcpy(entrySet + entrySetFilled * 32, slot, 32);
                            ++entrySetFilled;
                            if (entrySetFilled < entrySetNeeded) {
                                continue;
                            }
                        }

                        const ExfatParsedEntry parsed = kParseFileEntrySet(entrySet, entrySetNeeded - 1);
                        entrySetFilled = 0;
                        if (!parsed.valid) {
                            continue;
                        }

                        if (parsed.nameLen == segLen) {
                            uint16_t normalized[kMaxNameUtf16];
                            for (uint32_t i = 0; i < parsed.nameLen; ++i) {
                                normalized[i] = parsed.nameUtf16[i];
                            }
                            kUpcaseCodeUnits(upcaseTable, normalized, parsed.nameLen);
                            bool matches = true;
                            for (uint32_t i = 0; i < parsed.nameLen; ++i) {
                                if (normalized[i] != queryUpper[i]) {
                                    matches = false;
                                    break;
                                }
                            }
                            if (matches) {
                                matched = parsed;
                                foundInThisDir = true;
                            }
                        }
                    }
                }

                if (ioFailed || !foundInThisDir) {
                    failed = true;
                    break;
                }
                currentCluster = matched.firstCluster;
                currentIsDir = matched.isDir;
                currentFileSize = matched.fileSize;
                currentNoFatChain = matched.noFatChain;
            }

            if (failed) {
                args->result = kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
            } else {
                const uint64_t handleValue = kEncodeHandle(currentCluster, currentFileSize, currentNoFatChain);
                args->result =
                    kernel::OpenResult{kernel::FileHandle{handleValue}, currentIsDir, kernel::VfsError::None};
            }
            break;
        }

        case kernel::KernelFsOpCode::Close: {
            // 무상태(FileHandle 자체가 firstCluster+fileSize+noFatChain을
            // 이미 담고 있음) - Ext4Driver/Fat32Driver와 동일하게 따로
            // 정리할 자원이 없다.
            break;
        }

        case kernel::KernelFsOpCode::Read: {
            auto* args = static_cast<kernel::KernelFsReadArgs*>(argsRaw);
            uint32_t firstCluster = 0;
            uint64_t fileSize = 0;
            bool noFatChain = false;
            kDecodeHandle(args->handle.value, &firstCluster, &fileSize, &noFatChain);

            if (args->offset >= fileSize) {
                args->result = kernel::ReadResult{0, kernel::VfsError::None};  // EOF
                break;
            }
            uint64_t remaining = fileSize - args->offset;
            if (remaining > args->len) {
                remaining = args->len;
            }

            // 목표 오프셋이 속한 클러스터까지 이동 - noFatChain이면 산술만
            // (§3.2 핵심 최적화), 아니면 FAT 체인을 선형으로 따라간다.
            uint32_t cluster = firstCluster;
            uint64_t clusterStartOffset = 0;
            bool chainBroken = false;
            if (noFatChain) {
                const uint64_t clustersToSkip = args->offset / clusterSize;
                cluster = firstCluster + static_cast<uint32_t>(clustersToSkip);
                clusterStartOffset = clustersToSkip * clusterSize;
            } else {
                while (clusterStartOffset + clusterSize <= args->offset) {
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatOffset, sectorSize, cluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(sectorSize);
                    if (!fatBuf) {
                        chainBroken = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, sectorSize, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        chainBroken = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        chainBroken = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t next = 0;
                    if (!kInterpretFatEntry(raw, &next)) {
                        chainBroken = true;
                        break;
                    }
                    cluster = next;
                    clusterStartOffset += clusterSize;
                }
            }

            if (chainBroken) {
                args->result = kernel::ReadResult{0, kernel::VfsError::None};
                break;
            }

            SlabBuf clusterBuf(clusterSize);
            if (!clusterBuf) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            uint32_t totalCopied = 0;
            auto* out = static_cast<uint8_t*>(args->buf);
            bool ioFailed = false;
            while (remaining > 0 && !ioFailed) {
                uint32_t sector = 0;
                if (!kClusterToSector(clusterHeapOffset, sectorsPerCluster, cluster, &sector)) {
                    ioFailed = true;
                    break;
                }
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, sectorSize, sector, sectorsPerCluster, clusterBuf.get(), &ioResult);
                if (!ioTask) {
                    ioFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    ioFailed = true;
                    break;
                }

                const uint64_t curOffset = args->offset + totalCopied;
                const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset - clusterStartOffset);
                const uint32_t chunk = static_cast<uint32_t>(
                    remaining < (clusterSize - offsetInCluster) ? remaining : (clusterSize - offsetInCluster));
                memcpy(out + totalCopied, clusterBuf.get() + offsetInCluster, chunk);
                totalCopied += chunk;
                remaining -= chunk;

                if (remaining > 0) {
                    if (noFatChain) {
                        ++cluster;
                    } else {
                        uint32_t fatSector = 0;
                        uint32_t fatByteOffset = 0;
                        kFatEntryLocation(fatOffset, sectorSize, cluster, &fatSector, &fatByteOffset);
                        SlabBuf fatBuf(sectorSize);
                        if (!fatBuf) {
                            ioFailed = true;
                            break;
                        }
                        fs::BlockIoResult fatIoResult;
                        kernel::AsyncTask* fatIoTask =
                            kSubmitReadSectors(device, sectorSize, fatSector, 1, fatBuf.get(), &fatIoResult);
                        if (!fatIoTask) {
                            ioFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                        if (!fatIoResult.ok) {
                            ioFailed = true;
                            break;
                        }
                        uint32_t raw = 0;
                        memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                        uint32_t next = 0;
                        if (!kInterpretFatEntry(raw, &next)) {
                            break;  // 체인 끝 - 지금까지 복사된 만큼만 반환
                        }
                        cluster = next;
                    }
                    clusterStartOffset += clusterSize;
                }
            }
            args->result =
                kernel::ReadResult{totalCopied, ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None};
            break;
        }

        case kernel::KernelFsOpCode::Write: {
            // libexfat 1차 증분은 쓰기 경로가 없다(§4 미결) - 조용히
            // 무시하지 않고 명시적으로 거부한다(Ext4Driver/Fat32Driver 관례).
            auto* args = static_cast<kernel::KernelFsWriteArgs*>(argsRaw);
            args->bytesWritten = 0;
            args->error = kernel::VfsError::PermissionDenied;
            break;
        }

        case kernel::KernelFsOpCode::Stat: {
            auto* args = static_cast<kernel::KernelFsStatArgs*>(argsRaw);
            uint32_t currentCluster = volume_.rootFirstCluster();
            bool currentIsDir = true;
            uint64_t currentFileSize = 0;
            bool currentNoFatChain = false;
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

                uint16_t queryUpper[kMaxNameUtf16];
                for (uint32_t i = 0; i < segLen; ++i) {
                    queryUpper[i] = kWidenAscii(args->relPath[segStart + i]);
                }
                kUpcaseCodeUnits(upcaseTable, queryUpper, segLen);

                ExfatParsedEntry matched;
                bool foundInThisDir = false;
                bool ioFailed = false;
                {
                    uint32_t cluster = currentCluster;
                    uint64_t clusterIndex = 0;
                    uint32_t posInCluster = clusterSize;
                    const uint64_t clusterLimit =
                        currentNoFatChain ? kCeilDiv(currentFileSize, clusterSize) : ~static_cast<uint64_t>(0);
                    SlabBuf clusterBuf(clusterSize);
                    if (!clusterBuf) {
                        ioFailed = true;
                    }
                    uint8_t entrySet[kMaxEntrySetSlots * 32];
                    uint32_t entrySetFilled = 0;
                    uint32_t entrySetNeeded = 0;
                    bool dirEnded = false;

                    while (!ioFailed && !dirEnded && !foundInThisDir) {
                        if (posInCluster >= clusterSize) {
                            if (clusterIndex != 0) {
                                if (currentNoFatChain) {
                                    ++cluster;
                                } else {
                                    uint32_t fatSector = 0;
                                    uint32_t fatByteOffset = 0;
                                    kFatEntryLocation(fatOffset, sectorSize, cluster, &fatSector, &fatByteOffset);
                                    SlabBuf fatBuf(sectorSize);
                                    if (!fatBuf) {
                                        ioFailed = true;
                                        break;
                                    }
                                    fs::BlockIoResult fatIoResult;
                                    kernel::AsyncTask* fatIoTask = kSubmitReadSectors(
                                        device, sectorSize, fatSector, 1, fatBuf.get(), &fatIoResult);
                                    if (!fatIoTask) {
                                        ioFailed = true;
                                        break;
                                    }
                                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                                    if (!fatIoResult.ok) {
                                        ioFailed = true;
                                        break;
                                    }
                                    uint32_t raw = 0;
                                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                                    uint32_t next = 0;
                                    if (!kInterpretFatEntry(raw, &next)) {
                                        dirEnded = true;
                                        break;
                                    }
                                    cluster = next;
                                }
                            }
                            ++clusterIndex;
                            if (clusterIndex > clusterLimit) {
                                dirEnded = true;
                                break;
                            }
                            uint32_t sector = 0;
                            if (!kClusterToSector(clusterHeapOffset, sectorsPerCluster, cluster, &sector)) {
                                ioFailed = true;
                                break;
                            }
                            fs::BlockIoResult ioResult;
                            kernel::AsyncTask* ioTask = kSubmitReadSectors(device, sectorSize, sector,
                                                                            sectorsPerCluster, clusterBuf.get(),
                                                                            &ioResult);
                            if (!ioTask) {
                                ioFailed = true;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                            if (!ioResult.ok) {
                                ioFailed = true;
                                break;
                            }
                            posInCluster = 0;
                        }

                        uint8_t slot[32];
                        memcpy(slot, clusterBuf.get() + posInCluster, 32);
                        posInCluster += 32;

                        if (entrySetFilled == 0) {
                            if (slot[0] == kExfatEntryTypeEndOfDirectory) {
                                dirEnded = true;
                                break;
                            }
                            if (slot[0] != kExfatEntryTypeFile) {
                                continue;
                            }
                            ExfatFileDirEntry primary;
                            memcpy(&primary, slot, sizeof(primary));
                            if (primary.secondaryCount >= kMaxEntrySetSlots) {
                                ioFailed = true;
                                break;
                            }
                            memcpy(entrySet, slot, 32);
                            entrySetNeeded = primary.secondaryCount + 1;
                            entrySetFilled = 1;
                            if (entrySetFilled < entrySetNeeded) {
                                continue;
                            }
                        } else {
                            memcpy(entrySet + entrySetFilled * 32, slot, 32);
                            ++entrySetFilled;
                            if (entrySetFilled < entrySetNeeded) {
                                continue;
                            }
                        }

                        const ExfatParsedEntry parsed = kParseFileEntrySet(entrySet, entrySetNeeded - 1);
                        entrySetFilled = 0;
                        if (!parsed.valid) {
                            continue;
                        }

                        if (parsed.nameLen == segLen) {
                            uint16_t normalized[kMaxNameUtf16];
                            for (uint32_t i = 0; i < parsed.nameLen; ++i) {
                                normalized[i] = parsed.nameUtf16[i];
                            }
                            kUpcaseCodeUnits(upcaseTable, normalized, parsed.nameLen);
                            bool matches = true;
                            for (uint32_t i = 0; i < parsed.nameLen; ++i) {
                                if (normalized[i] != queryUpper[i]) {
                                    matches = false;
                                    break;
                                }
                            }
                            if (matches) {
                                matched = parsed;
                                foundInThisDir = true;
                            }
                        }
                    }
                }

                if (ioFailed || !foundInThisDir) {
                    failed = true;
                    break;
                }
                currentCluster = matched.firstCluster;
                currentIsDir = matched.isDir;
                currentFileSize = matched.fileSize;
                currentNoFatChain = matched.noFatChain;
            }

            if (failed) {
                args->error = kernel::VfsError::NotFound;
            } else {
                // exFAT도 FAT류와 마찬가지로 크기/디렉터리 여부가 부모
                // 디렉터리 엔트리 자신(Stream Extension)에 이미 있어
                // ext4의 Stat과 달리 별도 "타깃 재조회"가 필요 없다.
                args->size = currentFileSize;
                args->isDirectory = currentIsDir;
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

        case kernel::KernelFsOpCode::Readdir: {
            auto* args = static_cast<kernel::KernelFsReaddirArgs*>(argsRaw);
            uint32_t dirCluster = 0;
            uint64_t dirFileSize = 0;
            bool dirNoFatChain = false;
            kDecodeHandle(args->dirHandle.value, &dirCluster, &dirFileSize, &dirNoFatChain);

            uint64_t seen = 0;
            bool found = false;
            bool ioFailed = false;

            uint32_t cluster = dirCluster;
            uint64_t clusterIndex = 0;
            uint32_t posInCluster = clusterSize;
            const uint64_t clusterLimit =
                dirNoFatChain ? kCeilDiv(dirFileSize, clusterSize) : ~static_cast<uint64_t>(0);
            SlabBuf clusterBuf(clusterSize);
            if (!clusterBuf) {
                ioFailed = true;
            }
            uint8_t entrySet[kMaxEntrySetSlots * 32];
            uint32_t entrySetFilled = 0;
            uint32_t entrySetNeeded = 0;
            bool dirEnded = false;

            while (!ioFailed && !dirEnded && !found) {
                if (posInCluster >= clusterSize) {
                    if (clusterIndex != 0) {
                        if (dirNoFatChain) {
                            ++cluster;
                        } else {
                            uint32_t fatSector = 0;
                            uint32_t fatByteOffset = 0;
                            kFatEntryLocation(fatOffset, sectorSize, cluster, &fatSector, &fatByteOffset);
                            SlabBuf fatBuf(sectorSize);
                            if (!fatBuf) {
                                ioFailed = true;
                                break;
                            }
                            fs::BlockIoResult fatIoResult;
                            kernel::AsyncTask* fatIoTask =
                                kSubmitReadSectors(device, sectorSize, fatSector, 1, fatBuf.get(), &fatIoResult);
                            if (!fatIoTask) {
                                ioFailed = true;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                            if (!fatIoResult.ok) {
                                ioFailed = true;
                                break;
                            }
                            uint32_t raw = 0;
                            memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                            uint32_t next = 0;
                            if (!kInterpretFatEntry(raw, &next)) {
                                dirEnded = true;
                                break;
                            }
                            cluster = next;
                        }
                    }
                    ++clusterIndex;
                    if (clusterIndex > clusterLimit) {
                        dirEnded = true;
                        break;
                    }
                    uint32_t sector = 0;
                    if (!kClusterToSector(clusterHeapOffset, sectorsPerCluster, cluster, &sector)) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, sectorSize, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    posInCluster = 0;
                }

                uint8_t slot[32];
                memcpy(slot, clusterBuf.get() + posInCluster, 32);
                posInCluster += 32;

                if (entrySetFilled == 0) {
                    if (slot[0] == kExfatEntryTypeEndOfDirectory) {
                        dirEnded = true;
                        break;
                    }
                    if (slot[0] != kExfatEntryTypeFile) {
                        continue;
                    }
                    ExfatFileDirEntry primary;
                    memcpy(&primary, slot, sizeof(primary));
                    if (primary.secondaryCount >= kMaxEntrySetSlots) {
                        ioFailed = true;
                        break;
                    }
                    memcpy(entrySet, slot, 32);
                    entrySetNeeded = primary.secondaryCount + 1;
                    entrySetFilled = 1;
                    if (entrySetFilled < entrySetNeeded) {
                        continue;
                    }
                } else {
                    memcpy(entrySet + entrySetFilled * 32, slot, 32);
                    ++entrySetFilled;
                    if (entrySetFilled < entrySetNeeded) {
                        continue;
                    }
                }

                const ExfatParsedEntry parsed = kParseFileEntrySet(entrySet, entrySetNeeded - 1);
                entrySetFilled = 0;
                if (!parsed.valid) {
                    continue;
                }

                if (seen == args->index) {
                    const uint32_t toCopy =
                        parsed.nameLen < sizeof(args->entry.name) ? parsed.nameLen : sizeof(args->entry.name);
                    for (uint32_t i = 0; i < toCopy; ++i) {
                        args->entry.name[i] = static_cast<char>(parsed.nameUtf16[i] & 0xFF);  // v1 ASCII 범위만
                    }
                    args->entry.nameLength = toCopy;
                    args->entry.isDirectory = parsed.isDir;
                    found = true;
                } else {
                    ++seen;
                }
            }

            args->hasMore = found;
            args->error = ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None;
            break;
        }
    }
    co_return;
}

}  // namespace exfat
