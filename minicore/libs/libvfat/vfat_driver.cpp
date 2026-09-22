#include "vfat_driver.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

// [실측으로 발견, minicore/libs/libext4/ext4_driver.cpp(PN-9AE5BFE4)와
// 동일한 제약] `Fat32Volume`의 옛 무상태 함수(resolvePath/readData/
// readdirAt)는 재사용하지 않는다 - 전부 `fs::BlockDevice::readBlocks()`
// (동기 래퍼, Task 레벨 블로킹)를 쓰는데, 이건 `onExec()`(코루틴) 안에서
// 못 쓴다(PN-6EDED542 문서 주석 참고). `kernel::AsyncTaskCoroAwaiter`로
// 바꿔도, 그 클래스가 `AsyncTask::current()`(항상 최상위 - 이 경우
// onExec 자신의 AsyncTask)를 기준으로 재개 대상을 고르기 때문에, 별도
// 코루틴 함수로 감싼 I/O 헬퍼를 onExec이 다시 co_await하는 합성은
// 안전하게 재개되지 않는다(SP-F682B889 §9.5 항목3, 2026-09-22 정정
// 문단). 그래서 이 파일도 ext4_driver.cpp와 동일하게, I/O 지점마다
// `co_await kernel::AsyncTaskCoroAwaiter(...)`를 onExec 자신의 몸체
// 안에 직접 박아 넣는 평탄화(flatten)된 버전으로 구현한다 - 순수 계산
// (버퍼 해석) 부분만 별도 함수로 뽑고, 실제 I/O 오케스트레이션은 전부
// onExec 한 함수 안에 있다. `Open`/`Stat` 둘 다 경로 탐색이 필요해 그
// 루프가 두 곳에 거의 그대로 중복되는데, 위 제약(합성 불가) 때문에
// 함수로 뽑아 공유할 수 없어 의도적으로 감수한 중복이다.
namespace vfat {

namespace {

// 경로 세그먼트(정규화 안 된 원문)를 8.3 형식(대문자, 11바이트)으로
// 정규화한다 - vfat.cpp에 있던 것과 동일(그쪽은 옛 resolvePath 전용
// anonymous namespace라 여기서 재사용할 수 없어 그대로 복제 - ext4의
// kIsDirMode 등과 동일한 관례).
void kNormalizeTo83(const char* seg, uint32_t segLen, char out11[11]) {
    for (uint32_t i = 0; i < 11; ++i) {
        out11[i] = ' ';
    }
    uint32_t dot = segLen;
    for (uint32_t i = 0; i < segLen; ++i) {
        if (seg[i] == '.') {
            dot = i;
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

// slab 버퍼 RAII - ext4_driver.cpp의 SlabBuf와 동일(코루틴 지역
// 변수는 co_return/조기 종료 어느 경로로도 프레임 소멸 시 소멸자가
// 정확히 불린다는 이 코드베이스의 기존 관례에 의존).
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

// 장치 블록 크기가 뭐든 섹터 단위(sectorSize바이트, sectorStart부터
// sectorCount개)로 co_await 가능한 읽기 AsyncTask를 제출한다 -
// ext4_driver.cpp의 kSubmitReadExtBlocks와 동일한 LBA 변환.
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

// 데이터 클러스터 번호를 장치 섹터 번호로 바꾼다(순수 계산) - vfat.cpp에
// 있던 옛 Fat32Volume::clusterToSector와 동일한 판별.
bool kClusterToSector(uint32_t dataStartSector, uint32_t sectorsPerCluster, uint32_t cluster, uint32_t* outSector) {
    if (cluster < kFirstDataCluster) {
        return false;
    }
    *outSector = dataStartSector + (cluster - kFirstDataCluster) * sectorsPerCluster;
    return true;
}

// cluster번째 FAT 엔트리(32비트)가 있는 섹터/그 섹터 안 바이트
// 오프셋을 계산한다(순수 계산) - 실제로 그 섹터를 읽는 건 호출부
// (onExec)의 몫(합성 불가 제약, 파일 상단 문서 주석 참고).
void kFatEntryLocation(uint32_t fatStartSector, uint32_t bytesPerSector, uint32_t cluster, uint32_t* outSector,
                        uint32_t* outByteOffset) {
    const uint64_t fatByteOffset = static_cast<uint64_t>(cluster) * 4;
    *outSector = fatStartSector + static_cast<uint32_t>(fatByteOffset / bytesPerSector);
    *outByteOffset = static_cast<uint32_t>(fatByteOffset % bytesPerSector);
}

enum class ChainStep { Next, End, Invalid };

// raw 32비트 FAT 엔트리를 해석한다(순수 계산, I/O 없음) - vfat.cpp에
// 있던 옛 Fat32Volume::nextCluster와 동일한 판별.
ChainStep kInterpretFatEntry(uint32_t raw, uint32_t* outNext) {
    const uint32_t entry = raw & kFatEntryMask;
    if (entry == 0 || entry == kFatBadCluster) {
        return ChainStep::Invalid;
    }
    if (entry >= kFatEocMin) {
        return ChainStep::End;
    }
    *outNext = entry;
    return ChainStep::Next;
}

enum class DirScanResult { Found, NotFoundContinue, EndOfDir };

// 디렉터리 데이터 클러스터 하나 안에서 8.3 정규화 이름과 일치하는
// 엔트리를 찾는다(순수 계산, I/O 없음) - vfat.cpp에 있던 옛
// Fat32Volume::findDirEntry의 블록-스캔 부분만 뽑은 버전.
DirScanResult kScanDirClusterForName(const uint8_t* clusterBuf, uint32_t bytesPerCluster,
                                      const char normalized11[11], ResolvedEntry* out) {
    const uint32_t entriesPerCluster = bytesPerCluster / sizeof(DirEntry);
    const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf);
    for (uint32_t i = 0; i < entriesPerCluster; ++i) {
        const DirEntry& e = entries[i];
        const uint8_t firstByte = static_cast<uint8_t>(e.name[0]);
        if (firstByte == kNameFreeRestMarker) {
            return DirScanResult::EndOfDir;
        }
        if (firstByte == kNameDeletedMarker) {
            continue;
        }
        if (e.attr == kAttrLongName || (e.attr & kAttrVolumeId) != 0) {
            continue;  // LFN/볼륨 라벨 - §2 스코프 컷(짧은 이름만 지원)
        }
        if (kNameMatches(e, normalized11)) {
            out->firstCluster = kFatFirstCluster(e);
            out->isDir = (e.attr & kAttrDirectory) != 0;
            out->fileSize = out->isDir ? 0 : e.fileSize;
            return DirScanResult::Found;
        }
    }
    return DirScanResult::NotFoundContinue;
}

// 디렉터리 데이터 클러스터 하나 안에서 0-based index를 찾는다(순수
// 계산, I/O 없음) - vfat.cpp에 있던 옛 Fat32Volume::readdirAt의
// 블록-스캔 부분만 뽑은 버전.
DirScanResult kScanDirClusterForIndex(const uint8_t* clusterBuf, uint32_t bytesPerCluster, uint64_t target,
                                       uint64_t* seen, char* nameOut, uint32_t nameOutCap, uint32_t* outNameLen,
                                       bool* outIsDir) {
    const uint32_t entriesPerCluster = bytesPerCluster / sizeof(DirEntry);
    const auto* entries = reinterpret_cast<const DirEntry*>(clusterBuf);
    for (uint32_t i = 0; i < entriesPerCluster; ++i) {
        const DirEntry& e = entries[i];
        const uint8_t firstByte = static_cast<uint8_t>(e.name[0]);
        if (firstByte == kNameFreeRestMarker) {
            return DirScanResult::EndOfDir;
        }
        if (firstByte == kNameDeletedMarker) {
            continue;
        }
        if (e.attr == kAttrLongName || (e.attr & kAttrVolumeId) != 0) {
            continue;
        }
        if (*seen == target) {
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
            return DirScanResult::Found;
        }
        ++(*seen);
    }
    return DirScanResult::NotFoundContinue;
}

}  // namespace

bool Fat32Driver::mount(fs::BlockDevice* device, bool readOnly) {
    if (!volume_.mount(device)) {
        return false;
    }
    mounted_ = true;
    readOnly_ = readOnly;
    return true;
}

bool Fat32Driver::remount(bool writable) {
    // libvfat 1차 증분은 쓰기 경로 자체가 없어(§4) writable=true로
    // 전환해도 실질적 의미는 없다 - Ext4Driver::remount와 동일한 관례.
    readOnly_ = !writable;
    return true;
}

kernel::AsyncExecCoro Fat32Driver::onExec(kernel::AsyncTask*, void* argsRaw) {
    const auto op = *static_cast<const kernel::KernelFsOpCode*>(argsRaw);
    fs::BlockDevice* device = volume_.device();
    const uint32_t bytesPerSector = volume_.bytesPerSectorValue();
    const uint32_t sectorsPerCluster = volume_.sectorsPerClusterValue();
    const uint32_t bytesPerCluster = volume_.bytesPerClusterValue();
    const uint32_t fatStartSector = volume_.fatStartSectorValue();
    const uint32_t dataStartSector = volume_.dataStartSectorValue();

    switch (op) {
        case kernel::KernelFsOpCode::Open: {
            auto* args = static_cast<kernel::KernelFsOpenArgs*>(argsRaw);
            uint32_t currentCluster = volume_.rootFirstCluster();
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
                if (!currentIsDir) {
                    failed = true;
                    break;
                }

                char normalized[11];
                kNormalizeTo83(args->relPath + segStart, segLen, normalized);

                ResolvedEntry matched;
                bool foundInThisDir = false;
                uint32_t scanCluster = currentCluster;
                while (true) {
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        failed = true;
                        break;
                    }

                    const DirScanResult scanResult =
                        kScanDirClusterForName(clusterBuf.get(), bytesPerCluster, normalized, &matched);
                    if (scanResult == DirScanResult::Found) {
                        foundInThisDir = true;
                        break;
                    }
                    if (scanResult == DirScanResult::EndOfDir) {
                        break;
                    }

                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        failed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t nextCluster = 0;
                    if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                        break;  // 체인 끝(EOC) 또는 손상 - "이 이름 없음"으로 처리
                    }
                    scanCluster = nextCluster;
                }

                if (!foundInThisDir) {
                    failed = true;
                    break;
                }
                currentCluster = matched.firstCluster;
                currentIsDir = matched.isDir;
                currentFileSize = matched.fileSize;
            }

            if (failed) {
                args->result = kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
            } else {
                // FileHandle 인코딩(vfat_driver.h 문서 주석) - 하위
                // 32비트: firstCluster, 상위 32비트: fileSize.
                const uint64_t handleValue = (currentFileSize << 32) | currentCluster;
                args->result =
                    kernel::OpenResult{kernel::FileHandle{handleValue}, currentIsDir, kernel::VfsError::None};
            }
            break;
        }

        case kernel::KernelFsOpCode::Close: {
            // 무상태(FileHandle 자체가 firstCluster+fileSize를 이미
            // 담고 있음) - Ext4Driver와 동일하게 따로 정리할 자원이 없다.
            break;
        }

        case kernel::KernelFsOpCode::Read: {
            auto* args = static_cast<kernel::KernelFsReadArgs*>(argsRaw);
            const uint32_t firstCluster = static_cast<uint32_t>(args->handle.value);
            const uint64_t fileSize = args->handle.value >> 32;

            if (args->offset >= fileSize) {
                args->result = kernel::ReadResult{0, kernel::VfsError::None};  // EOF
                break;
            }
            uint64_t remaining = fileSize - args->offset;
            if (remaining > args->len) {
                remaining = args->len;
            }

            // 목표 오프셋이 속한 클러스터까지 체인을 선형으로 따라간다
            // (v1은 캐시 없음 - vfat.cpp에 있던 옛 readData와 동일한
            // 단순한 버전).
            uint32_t cluster = firstCluster;
            uint64_t clusterStartOffset = 0;
            bool chainBroken = false;
            while (clusterStartOffset + bytesPerCluster <= args->offset) {
                uint32_t fatSector = 0;
                uint32_t fatByteOffset = 0;
                kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                SlabBuf fatBuf(bytesPerSector);
                if (!fatBuf) {
                    chainBroken = true;
                    break;
                }
                fs::BlockIoResult fatIoResult;
                kernel::AsyncTask* fatIoTask =
                    kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
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
                uint32_t nextCluster = 0;
                if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                    chainBroken = true;
                    break;
                }
                cluster = nextCluster;
                clusterStartOffset += bytesPerCluster;
            }

            if (chainBroken) {
                // 파일 크기보다 짧은 체인 - 손상되었거나 예상 밖이나,
                // 방어적으로 0 반환(vfat.cpp에 있던 옛 readData와 동일 관례).
                args->result = kernel::ReadResult{0, kernel::VfsError::None};
                break;
            }

            SlabBuf clusterBuf(bytesPerCluster);
            if (!clusterBuf) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            uint32_t totalCopied = 0;
            auto* out = static_cast<uint8_t*>(args->buf);
            bool ioFailed = false;
            while (remaining > 0 && !ioFailed) {
                uint32_t sector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, cluster, &sector)) {
                    ioFailed = true;
                    break;
                }
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
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

                const uint64_t curOffset = args->offset + totalCopied;
                const uint32_t offsetInCluster = static_cast<uint32_t>(curOffset - clusterStartOffset);
                const uint32_t chunk = static_cast<uint32_t>(
                    remaining < (bytesPerCluster - offsetInCluster) ? remaining : (bytesPerCluster - offsetInCluster));
                memcpy(out + totalCopied, clusterBuf.get() + offsetInCluster, chunk);
                totalCopied += chunk;
                remaining -= chunk;

                if (remaining > 0) {
                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
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
                    uint32_t nextCluster = 0;
                    if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                        break;  // 체인 끝 - 지금까지 복사된 만큼만 반환
                    }
                    cluster = nextCluster;
                    clusterStartOffset += bytesPerCluster;
                }
            }
            args->result =
                kernel::ReadResult{totalCopied, ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None};
            break;
        }

        case kernel::KernelFsOpCode::Write: {
            // libvfat 1차 증분은 쓰기 경로가 없다(§4 미결) - 조용히
            // 무시하지 않고 명시적으로 거부한다(Ext4Driver 관례).
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
                if (!currentIsDir) {
                    failed = true;
                    break;
                }

                char normalized[11];
                kNormalizeTo83(args->relPath + segStart, segLen, normalized);

                ResolvedEntry matched;
                bool foundInThisDir = false;
                uint32_t scanCluster = currentCluster;
                while (true) {
                    uint32_t sector = 0;
                    if (!kClusterToSector(dataStartSector, sectorsPerCluster, scanCluster, &sector)) {
                        break;
                    }
                    SlabBuf clusterBuf(bytesPerCluster);
                    if (!clusterBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
                                           &ioResult);
                    if (!ioTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        failed = true;
                        break;
                    }

                    const DirScanResult scanResult =
                        kScanDirClusterForName(clusterBuf.get(), bytesPerCluster, normalized, &matched);
                    if (scanResult == DirScanResult::Found) {
                        foundInThisDir = true;
                        break;
                    }
                    if (scanResult == DirScanResult::EndOfDir) {
                        break;
                    }

                    uint32_t fatSector = 0;
                    uint32_t fatByteOffset = 0;
                    kFatEntryLocation(fatStartSector, bytesPerSector, scanCluster, &fatSector, &fatByteOffset);
                    SlabBuf fatBuf(bytesPerSector);
                    if (!fatBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult fatIoResult;
                    kernel::AsyncTask* fatIoTask =
                        kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
                    if (!fatIoTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(fatIoTask);
                    if (!fatIoResult.ok) {
                        failed = true;
                        break;
                    }
                    uint32_t raw = 0;
                    memcpy(&raw, fatBuf.get() + fatByteOffset, sizeof(raw));
                    uint32_t nextCluster = 0;
                    if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                        break;
                    }
                    scanCluster = nextCluster;
                }

                if (!foundInThisDir) {
                    failed = true;
                    break;
                }
                currentCluster = matched.firstCluster;
                currentIsDir = matched.isDir;
                currentFileSize = matched.fileSize;
            }

            if (failed) {
                args->error = kernel::VfsError::NotFound;
            } else {
                // ext4의 Stat과 달리 별도 "타깃 재조회"가 필요 없다 -
                // FAT은 크기/디렉터리 여부가 부모 디렉터리 엔트리
                // 자신에 이미 있어(vfat.h의 ResolvedEntry 문서 주석)
                // 위 탐색 루프의 마지막 매치 결과를 그대로 쓰면 된다.
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
            const uint32_t dirFirstCluster = static_cast<uint32_t>(args->dirHandle.value);

            uint64_t seen = 0;
            bool found = false;
            bool ioFailed = false;
            uint32_t cluster = dirFirstCluster;
            SlabBuf clusterBuf(bytesPerCluster);
            if (!clusterBuf) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            while (!found && !ioFailed) {
                uint32_t sector = 0;
                if (!kClusterToSector(dataStartSector, sectorsPerCluster, cluster, &sector)) {
                    break;
                }
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadSectors(device, bytesPerSector, sector, sectorsPerCluster, clusterBuf.get(),
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

                const DirScanResult scanResult =
                    kScanDirClusterForIndex(clusterBuf.get(), bytesPerCluster, args->index, &seen, args->entry.name,
                                             sizeof(args->entry.name), &args->entry.nameLength,
                                             &args->entry.isDirectory);
                if (scanResult == DirScanResult::Found) {
                    found = true;
                    break;
                }
                if (scanResult == DirScanResult::EndOfDir) {
                    break;
                }

                uint32_t fatSector = 0;
                uint32_t fatByteOffset = 0;
                kFatEntryLocation(fatStartSector, bytesPerSector, cluster, &fatSector, &fatByteOffset);
                SlabBuf fatBuf(bytesPerSector);
                if (!fatBuf) {
                    ioFailed = true;
                    break;
                }
                fs::BlockIoResult fatIoResult;
                kernel::AsyncTask* fatIoTask =
                    kSubmitReadSectors(device, bytesPerSector, fatSector, 1, fatBuf.get(), &fatIoResult);
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
                uint32_t nextCluster = 0;
                if (kInterpretFatEntry(raw, &nextCluster) != ChainStep::Next) {
                    break;
                }
                cluster = nextCluster;
            }

            args->hasMore = found;
            args->error = ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None;
            break;
        }
    }
    co_return;
}

}  // namespace vfat
