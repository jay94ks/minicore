#include "ext4_driver.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

// [실측으로 발견, 2026-09-22, PN-9AE5BFE4] `Ext4Volume`의 무상태 함수
// (resolvePath/readInode/statInode/readdirAt)는 재사용하지 않는다 -
// 전부 `fs::BlockDevice::readBlocks()`(동기 래퍼, Task 레벨 블로킹)
// 를 쓰는데, 이건 `onExec()`(코루틴) 안에서 못 쓴다(PN-6EDED542
// 문서 주석 참고). `kernel::AsyncTaskCoroAwaiter`(PN-6EDED542,
// co_await 프로토콜)로 바꿔도, 그 클래스가 `AsyncTask::current()`
// (항상 최상위 - 이 경우 onExec 자신의 AsyncTask)를 기준으로 재개
// 대상을 고르기 때문에, `Ext4Volume`의 헬퍼들을 **별도 코루틴
// 함수**로 감싸 `onExec`이 그걸 다시 `co_await`하는 합성(nested
// coroutine composition)은 안전하게 재개되지 않는다(중간 코루틴
// 프레임의 존재를 `AsyncTaskCoroAwaiter`가 전혀 모름 - 실측 분석
// 확인, `kernel::AsyncExecCoro` 자체를 고치지 않는 한 원천적으로
// 안 됨, 그 타입은 커널 전체 공유 타입이라 이 세션이 임의로
// 고치지 않는다). 그래서 이 파일은 `Ext4Volume`의 로직을 재사용하는
// 대신, I/O 지점마다 `co_await kernel::AsyncTaskCoroAwaiter(...)`를
// **onExec 자신의 몸체 안에 직접** 박아 넣는 평탄화(flatten)된
// 버전으로 다시 구현한다 - 순수 계산(버퍼 해석) 부분만 별도 함수로
// 뽑고, 실제 I/O 오케스트레이션은 전부 onExec 한 함수 안에 있다.
// `Open`/`Stat` 둘 다 경로 탐색이 필요해 그 루프가 두 곳에 거의
// 그대로 중복되는데, 위 제약(합성 불가) 때문에 함수로 뽑아 공유할
// 수 없어 의도적으로 감수한 중복이다.
namespace ext4 {

namespace {

uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

bool kIsDirMode(uint16_t mode) { return (mode & 0xF000u) == 0x4000u; }

// slab 버퍼 RAII - 코루틴 지역 변수는 co_return/조기 종료 어느
// 경로로도 프레임 소멸 시 소멸자가 정확히 불린다(표준 C++20 코루틴
// 프레임 수명 규칙 - process.cpp의 JoinHandler가 이미 이 성질에
// 의존하고 있다는 문서 주석 참고, 이 코드베이스에 이미 있는 관례).
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

// 장치 블록 크기가 뭐든 ext4 블록 하나(extBlockSize바이트, extBlock
// 번째)를 읽는 AsyncTask를 제출한다 - ext4.cpp의 readExtBlocksImpl과
// 동일한 LBA 변환, 다만 여기선 co_await할 수 있게 AsyncTask*를 그대로
// 돌려준다(동기 wrapper를 쓰지 않는다).
kernel::AsyncTask* kSubmitReadExtBlocks(fs::BlockDevice* device, uint32_t extBlockSize, uint64_t extBlockStart,
                                         uint32_t extBlockCount, void* buf, fs::BlockIoResult* outResult) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || extBlockSize % devBlockSize != 0) {
        return nullptr;
    }
    const kernel::uint32_t devBlocksPerExtBlock = extBlockSize / devBlockSize;
    return device->submitReadBlocks(extBlockStart * devBlocksPerExtBlock, buf,
                                     extBlockCount * devBlocksPerExtBlock, outResult);
}

// inodeNum이 속한 그룹/inode 테이블 상의 정확한 바이트 위치를
// 계산한다(ext4.cpp의 Ext4Volume::readInodeStruct와 동일 계산) -
// 실패(inode 번호 범위 밖)면 false.
// [갱신, 2026-09-25, PN-36747363] `groupDescs` 배열 포인터 대신
// `Ext4Volume::groupInodeTableBlock()`을 쓴다 - INCOMPAT_64BIT
// 볼륨에서 4G 블록을 실제로 초과하는 그룹의 inode 테이블 주소는
// hi 필드까지 합성해야 정확한 64비트 값이 나오기 때문(예전
// `GroupDesc32`로 압축된 뷰는 이 hi 필드를 이미 잃어버린 상태였다).
bool kLocateInode(const SuperblockCore& sb, const Ext4Volume& volume, uint32_t groupCount, uint32_t blockSize,
                   uint32_t inodeNum, uint64_t* outBlockOffset, uint32_t* outByteOffsetInBlock,
                   uint32_t* outBlocksNeeded) {
    if (inodeNum == 0) {
        return false;
    }
    const uint32_t group = (inodeNum - 1) / sb.inodesPerGroup;
    if (group >= groupCount) {
        return false;
    }
    const uint32_t indexInGroup = (inodeNum - 1) % sb.inodesPerGroup;
    const uint64_t byteOffsetInTable = static_cast<uint64_t>(indexInGroup) * sb.inodeSize;
    *outBlockOffset = volume.groupInodeTableBlock(group) + byteOffsetInTable / blockSize;
    *outByteOffsetInBlock = static_cast<uint32_t>(byteOffsetInTable % blockSize);
    *outBlocksNeeded = static_cast<uint32_t>(kCeilDiv(*outByteOffsetInBlock + sizeof(InodeCore), blockSize));
    return true;
}

enum class ExtentLookup { Found, Hole, NeedChild, Invalid };

// 순수 계산(버퍼 해석만, I/O 없음) - ext4.cpp의 resolveExtentNode와
// 동일한 판별. depth==0(리프)면 Found/Hole, depth>0(내부 노드)면
// NeedChild(outValue=다음에 읽어야 할 자식 블록 번호)를 돌려준다 -
// 실제로 그 자식 블록을 읽는 건 호출부(onExec)의 몫(합성 불가 제약,
// 파일 상단 문서 주석 참고).
ExtentLookup kLookupExtent(const uint8_t* nodeBytes, uint32_t logicalBlock, uint64_t* outValue) {
    const auto* header = reinterpret_cast<const ExtentHeader*>(nodeBytes);
    if (header->magic != kExtentMagic) {
        return ExtentLookup::Invalid;
    }
    const uint8_t* entries = nodeBytes + sizeof(ExtentHeader);
    if (header->depth == 0) {
        for (uint16_t i = 0; i < header->entries; ++i) {
            const auto* ext = reinterpret_cast<const Extent*>(entries + i * sizeof(Extent));
            const uint32_t len = ext->len & ~kExtentUninitLenBit;
            if (logicalBlock >= ext->block && logicalBlock < ext->block + len) {
                *outValue =
                    ((static_cast<uint64_t>(ext->startHi) << 32) | ext->startLo) + (logicalBlock - ext->block);
                return ExtentLookup::Found;
            }
        }
        return ExtentLookup::Hole;
    }
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
        return ExtentLookup::Invalid;
    }
    *outValue = (static_cast<uint64_t>(chosen->leafHi) << 32) | chosen->leafLo;
    return ExtentLookup::NeedChild;
}

// [신규, 2026-09-23, PN-E3629BE9, SP-7A9CED3E §2.2 항목1] 레거시
// 간접 블록 매핑(EXTENTS_FL 꺼진 inode - ext2/ext3 표준 레이아웃,
// 이 프로젝트가 새로 고안한 게 아니다) - 순수 계산(I/O 없음)만
// 담당한다. i_block[0..11]=direct, [12]=singly/[13]=doubly/
// [14]=triply indirect - 각 간접 블록은 blockSize/4개의 uint32_t
// 포인터 배열. kLookupExtent와 나란한 역할이지만, 익스텐트 트리와
// 달리 깊이가 logicalBlock 값만으로 정확히 결정되므로(트리를 실제로
// 안 읽어봐도 몇 단계인지 안다) NeedChild 루프 대신 필요한 hop 수를
// 미리 계산해 돌려준다 - 실제 간접 블록 읽기(1~3회)는 호출부(onExec)
// 의 몫(파일 상단 문서 주석의 합성 불가 제약과 동일한 이유).
enum class IndirectLevel { Direct, Single, Double, Triple, OutOfRange };

struct IndirectResolution {
    IndirectLevel level;
    uint32_t index0;  // Direct: i_block 인덱스. 그 외: 최상위 간접 블록 안의 인덱스(다음 hop 대상 선택)
    uint32_t index1;  // Double/Triple: 두 번째 단계 인덱스
    uint32_t index2;  // Triple: 세 번째(최종) 단계 인덱스
};

IndirectResolution kResolveIndirect(uint32_t logicalBlock, uint32_t pointersPerBlock) {
    if (logicalBlock < 12) {
        return IndirectResolution{IndirectLevel::Direct, logicalBlock, 0, 0};
    }
    uint64_t rem = static_cast<uint64_t>(logicalBlock) - 12;
    if (rem < pointersPerBlock) {
        return IndirectResolution{IndirectLevel::Single, static_cast<uint32_t>(rem), 0, 0};
    }
    rem -= pointersPerBlock;
    const uint64_t doubleCapacity = static_cast<uint64_t>(pointersPerBlock) * pointersPerBlock;
    if (rem < doubleCapacity) {
        return IndirectResolution{IndirectLevel::Double, static_cast<uint32_t>(rem / pointersPerBlock),
                                   static_cast<uint32_t>(rem % pointersPerBlock), 0};
    }
    rem -= doubleCapacity;
    const uint64_t tripleCapacity = doubleCapacity * pointersPerBlock;
    if (rem < tripleCapacity) {
        const uint32_t idx0 = static_cast<uint32_t>(rem / doubleCapacity);
        const uint64_t rem2 = rem % doubleCapacity;
        return IndirectResolution{IndirectLevel::Triple, idx0, static_cast<uint32_t>(rem2 / pointersPerBlock),
                                   static_cast<uint32_t>(rem2 % pointersPerBlock)};
    }
    return IndirectResolution{IndirectLevel::OutOfRange, 0, 0, 0};
}

// 디렉터리 데이터 블록 하나 안에서 name과 일치하는 엔트리를 찾는다
// (ext4.cpp의 forEachDirEntryInBlock + findDirEntry의 콜백을 합친
// 순수 버전, I/O 없음).
bool kScanDirBlockForName(const uint8_t* block, uint32_t blockSize, const char* name, uint32_t nameLen,
                           uint32_t* outInode, uint8_t* outFileType) {
    uint32_t pos = 0;
    while (pos + sizeof(DirEntry2Header) <= blockSize) {
        const auto* hdr = reinterpret_cast<const DirEntry2Header*>(block + pos);
        if (hdr->recLen == 0 || pos + hdr->recLen > blockSize) {
            break;
        }
        if (hdr->inode != 0 && hdr->nameLen == nameLen) {
            const char* entryName = reinterpret_cast<const char*>(block + pos + sizeof(DirEntry2Header));
            bool matches = true;
            for (uint32_t i = 0; i < nameLen; ++i) {
                if (entryName[i] != name[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                *outInode = hdr->inode;
                *outFileType = hdr->fileType;
                return true;
            }
        }
        pos += hdr->recLen;
    }
    return false;
}

// 디렉터리 데이터 블록 하나 안에서 0-based 인덱스 target을 찾는다 -
// 찾으면 true(이름/타입/inode를 채움), 못 찾았으면 seen을 갱신하고
// false(호출부가 다음 블록으로 계속).
bool kScanDirBlockForIndex(const uint8_t* block, uint32_t blockSize, uint64_t target, uint64_t* seen, char* nameOut,
                            uint32_t nameOutCap, uint32_t* outNameLen, bool* outIsDir, uint32_t* outEntryInode) {
    uint32_t pos = 0;
    while (pos + sizeof(DirEntry2Header) <= blockSize) {
        const auto* hdr = reinterpret_cast<const DirEntry2Header*>(block + pos);
        if (hdr->recLen == 0 || pos + hdr->recLen > blockSize) {
            break;
        }
        if (hdr->inode != 0) {
            if (*seen == target) {
                const char* entryName = reinterpret_cast<const char*>(block + pos + sizeof(DirEntry2Header));
                const uint32_t toCopy = hdr->nameLen < nameOutCap ? hdr->nameLen : nameOutCap;
                memcpy(nameOut, entryName, toCopy);
                *outNameLen = toCopy;
                *outIsDir = (hdr->fileType == kFtDir);
                *outEntryInode = hdr->inode;
                return true;
            }
            ++(*seen);
        }
        pos += hdr->recLen;
    }
    return false;
}

}  // namespace

bool Ext4Driver::mount(fs::BlockDevice* device, bool readOnly) {
    if (!volume_.mount(device)) {
        return false;
    }
    mounted_ = true;
    readOnly_ = readOnly;
    return true;
}

bool Ext4Driver::remount(bool writable) {
    // libext4 1차 증분은 쓰기 경로 자체가 없어(§5) writable=true로
    // 전환해도 실질적 의미는 없다 - 계약대로("이미 writable이면 아무
    // 효과 없이 true") 요청은 받아들이지만, 실제 쓰기 오퍼레이션은
    // 여전히 onExec에서 PermissionDenied로 거부된다.
    readOnly_ = !writable;
    return true;
}

kernel::AsyncExecCoro Ext4Driver::onExec(kernel::AsyncTask*, void* argsRaw) {
    const auto op = *static_cast<const kernel::KernelFsOpCode*>(argsRaw);
    const SuperblockCore& sb = volume_.superblockInfo();
    const uint32_t blockSize = volume_.blockSizeValue();
    const uint32_t groupCount = volume_.groupCountValue();
    fs::BlockDevice* device = volume_.device();

    switch (op) {
        case kernel::KernelFsOpCode::Open: {
            auto* args = static_cast<kernel::KernelFsOpenArgs*>(argsRaw);
            uint32_t currentInode = kRootInodeNumber;
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
                if (!currentIsDir) {
                    failed = true;
                    break;
                }

                // currentInode(디렉터리)의 inode 구조체를 읽는다.
                uint64_t inodeBlockOffset = 0;
                uint32_t inodeByteOffset = 0;
                uint32_t inodeBlocksNeeded = 0;
                if (!kLocateInode(sb, volume_, groupCount, blockSize, currentInode, &inodeBlockOffset,
                                   &inodeByteOffset, &inodeBlocksNeeded)) {
                    failed = true;
                    break;
                }
                SlabBuf inodeBuf(inodeBlocksNeeded * blockSize);
                if (!inodeBuf) {
                    failed = true;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, inodeBlockOffset, inodeBlocksNeeded, inodeBuf.get(),
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
                }
                InodeCore dirInode;
                memcpy(&dirInode, inodeBuf.get() + inodeByteOffset, sizeof(dirInode));
                if (!kIsDirMode(dirInode.mode)) {
                    failed = true;
                    break;
                }

                // dirInode의 데이터 블록들을 순회하며 segment 이름을 찾는다.
                const uint64_t dirSize = dirInode.sizeLo | (static_cast<uint64_t>(dirInode.sizeHigh) << 32);
                const uint32_t dirBlockCount = static_cast<uint32_t>(kCeilDiv(dirSize, blockSize));
                bool foundInThisDir = false;

                for (uint32_t logicalBlock = 0; logicalBlock < dirBlockCount && !foundInThisDir; ++logicalBlock) {
                    uint64_t nodeValue = 0;
                    ExtentLookup lookup;
                    if (dirInode.flags & kExtentsFl) {
                        lookup = kLookupExtent(dirInode.block, logicalBlock, &nodeValue);
                        uint32_t depthGuard = 5;
                        SlabBuf extentNodeBuf(blockSize);
                        while (lookup == ExtentLookup::NeedChild && depthGuard > 0) {
                            if (!extentNodeBuf) {
                                lookup = ExtentLookup::Invalid;
                                break;
                            }
                            fs::BlockIoResult ioResult;
                            kernel::AsyncTask* ioTask =
                                kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, extentNodeBuf.get(), &ioResult);
                            if (!ioTask) {
                                lookup = ExtentLookup::Invalid;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                            if (!ioResult.ok) {
                                lookup = ExtentLookup::Invalid;
                                break;
                            }
                            lookup = kLookupExtent(extentNodeBuf.get(), logicalBlock, &nodeValue);
                            --depthGuard;
                        }
                    } else {
                        // [신규, PN-E3629BE9] 레거시 간접 블록(ext2/ext3 호환).
                        const uint32_t pointersPerBlock = blockSize / sizeof(uint32_t);
                        const auto* rootBlocks = reinterpret_cast<const uint32_t*>(dirInode.block);
                        const IndirectResolution res = kResolveIndirect(logicalBlock, pointersPerBlock);
                        if (res.level == IndirectLevel::OutOfRange) {
                            lookup = ExtentLookup::Hole;
                        } else if (res.level == IndirectLevel::Direct) {
                            nodeValue = rootBlocks[res.index0];
                            lookup = nodeValue == 0 ? ExtentLookup::Hole : ExtentLookup::Found;
                        } else {
                            uint32_t indices[3];
                            uint32_t hops;
                            uint32_t currentBlockNum;
                            if (res.level == IndirectLevel::Single) {
                                indices[0] = res.index0;
                                hops = 1;
                                currentBlockNum = rootBlocks[12];
                            } else if (res.level == IndirectLevel::Double) {
                                indices[0] = res.index0;
                                indices[1] = res.index1;
                                hops = 2;
                                currentBlockNum = rootBlocks[13];
                            } else {
                                indices[0] = res.index0;
                                indices[1] = res.index1;
                                indices[2] = res.index2;
                                hops = 3;
                                currentBlockNum = rootBlocks[14];
                            }
                            if (currentBlockNum == 0) {
                                lookup = ExtentLookup::Hole;
                            } else {
                                lookup = ExtentLookup::Found;  // hop 루프가 끝까지 가면 Found로 남김
                                for (uint32_t h = 0; h < hops; ++h) {
                                    SlabBuf indBuf(blockSize);
                                    if (!indBuf) {
                                        lookup = ExtentLookup::Invalid;
                                        break;
                                    }
                                    fs::BlockIoResult ioResult;
                                    kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(
                                        device, blockSize, currentBlockNum, 1, indBuf.get(), &ioResult);
                                    if (!ioTask) {
                                        lookup = ExtentLookup::Invalid;
                                        break;
                                    }
                                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                                    if (!ioResult.ok) {
                                        lookup = ExtentLookup::Invalid;
                                        break;
                                    }
                                    const uint32_t nextPtr =
                                        reinterpret_cast<const uint32_t*>(indBuf.get())[indices[h]];
                                    if (nextPtr == 0) {
                                        lookup = ExtentLookup::Hole;
                                        break;
                                    }
                                    if (h + 1 == hops) {
                                        nodeValue = nextPtr;
                                    } else {
                                        currentBlockNum = nextPtr;
                                    }
                                }
                            }
                        }
                    }
                    if (lookup != ExtentLookup::Found) {
                        continue;  // 구멍(hole)이거나 손상 - 이 논리 블록은 건너뜀(디렉터리엔 정상적으로 안 생김)
                    }

                    SlabBuf dataBuf(blockSize);
                    if (!dataBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, dataBuf.get(), &ioResult);
                    if (!ioTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        failed = true;
                        break;
                    }

                    uint32_t matchedInode = 0;
                    uint8_t matchedType = 0;
                    if (kScanDirBlockForName(dataBuf.get(), blockSize, args->relPath + segStart, segLen,
                                              &matchedInode, &matchedType)) {
                        currentInode = matchedInode;
                        currentIsDir = (matchedType == kFtDir);
                        foundInThisDir = true;
                    }
                }
                if (!foundInThisDir) {
                    failed = true;
                }
            }

            if (failed) {
                args->result = kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::NotFound};
            } else {
                args->result = kernel::OpenResult{kernel::FileHandle{currentInode}, currentIsDir,
                                                   kernel::VfsError::None};
            }
            break;
        }

        case kernel::KernelFsOpCode::Close: {
            // 무상태(inode 번호 자체가 곧 핸들) - LiveFs와 동일하게
            // 이 드라이버가 따로 정리할 자원이 없다.
            break;
        }

        case kernel::KernelFsOpCode::Read: {
            auto* args = static_cast<kernel::KernelFsReadArgs*>(argsRaw);
            const uint32_t inodeNum = static_cast<uint32_t>(args->handle.value);

            uint64_t inodeBlockOffset = 0;
            uint32_t inodeByteOffset = 0;
            uint32_t inodeBlocksNeeded = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, inodeNum, &inodeBlockOffset, &inodeByteOffset,
                               &inodeBlocksNeeded)) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            SlabBuf inodeBuf(inodeBlocksNeeded * blockSize);
            if (!inodeBuf) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, inodeBlockOffset,
                                                                  inodeBlocksNeeded, inodeBuf.get(), &ioResult);
                if (!ioTask) {
                    args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                    break;
                }
            }
            InodeCore inode;
            memcpy(&inode, inodeBuf.get() + inodeByteOffset, sizeof(inode));

            const uint64_t fileSize = inode.sizeLo | (static_cast<uint64_t>(inode.sizeHigh) << 32);
            if (args->offset >= fileSize) {
                args->result = kernel::ReadResult{0, kernel::VfsError::None};  // EOF
                break;
            }
            uint64_t remaining = fileSize - args->offset;
            if (remaining > args->len) {
                remaining = args->len;
            }

            SlabBuf dataBuf(blockSize);
            if (!dataBuf) {
                args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
                break;
            }
            uint32_t totalCopied = 0;
            auto* out = static_cast<uint8_t*>(args->buf);
            bool ioFailed = false;
            while (remaining > 0 && !ioFailed) {
                const uint64_t curOffset = args->offset + totalCopied;
                const uint32_t logicalBlock = static_cast<uint32_t>(curOffset / blockSize);
                const uint32_t offsetInBlock = static_cast<uint32_t>(curOffset % blockSize);
                const uint32_t chunk = static_cast<uint32_t>(remaining < (blockSize - offsetInBlock)
                                                                   ? remaining
                                                                   : (blockSize - offsetInBlock));

                uint64_t nodeValue = 0;
                ExtentLookup lookup;
                if (inode.flags & kExtentsFl) {
                    lookup = kLookupExtent(inode.block, logicalBlock, &nodeValue);
                    uint32_t depthGuard = 5;
                    SlabBuf extentNodeBuf(blockSize);
                    while (lookup == ExtentLookup::NeedChild && depthGuard > 0) {
                        if (!extentNodeBuf) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask =
                            kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, extentNodeBuf.get(), &ioResult);
                        if (!ioTask) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        lookup = kLookupExtent(extentNodeBuf.get(), logicalBlock, &nodeValue);
                        --depthGuard;
                    }
                } else {
                    // [신규, PN-E3629BE9] 레거시 간접 블록(ext2/ext3 호환).
                    const uint32_t pointersPerBlock = blockSize / sizeof(uint32_t);
                    const auto* rootBlocks = reinterpret_cast<const uint32_t*>(inode.block);
                    const IndirectResolution res = kResolveIndirect(logicalBlock, pointersPerBlock);
                    if (res.level == IndirectLevel::OutOfRange) {
                        lookup = ExtentLookup::Hole;
                    } else if (res.level == IndirectLevel::Direct) {
                        nodeValue = rootBlocks[res.index0];
                        lookup = nodeValue == 0 ? ExtentLookup::Hole : ExtentLookup::Found;
                    } else {
                        uint32_t indices[3];
                        uint32_t hops;
                        uint32_t currentBlockNum;
                        if (res.level == IndirectLevel::Single) {
                            indices[0] = res.index0;
                            hops = 1;
                            currentBlockNum = rootBlocks[12];
                        } else if (res.level == IndirectLevel::Double) {
                            indices[0] = res.index0;
                            indices[1] = res.index1;
                            hops = 2;
                            currentBlockNum = rootBlocks[13];
                        } else {
                            indices[0] = res.index0;
                            indices[1] = res.index1;
                            indices[2] = res.index2;
                            hops = 3;
                            currentBlockNum = rootBlocks[14];
                        }
                        if (currentBlockNum == 0) {
                            lookup = ExtentLookup::Hole;
                        } else {
                            lookup = ExtentLookup::Found;
                            for (uint32_t h = 0; h < hops; ++h) {
                                SlabBuf indBuf(blockSize);
                                if (!indBuf) {
                                    lookup = ExtentLookup::Invalid;
                                    break;
                                }
                                fs::BlockIoResult ioResult;
                                kernel::AsyncTask* ioTask =
                                    kSubmitReadExtBlocks(device, blockSize, currentBlockNum, 1, indBuf.get(), &ioResult);
                                if (!ioTask) {
                                    lookup = ExtentLookup::Invalid;
                                    break;
                                }
                                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                                if (!ioResult.ok) {
                                    lookup = ExtentLookup::Invalid;
                                    break;
                                }
                                const uint32_t nextPtr = reinterpret_cast<const uint32_t*>(indBuf.get())[indices[h]];
                                if (nextPtr == 0) {
                                    lookup = ExtentLookup::Hole;
                                    break;
                                }
                                if (h + 1 == hops) {
                                    nodeValue = nextPtr;
                                } else {
                                    currentBlockNum = nextPtr;
                                }
                            }
                        }
                    }
                }

                if (lookup == ExtentLookup::Found) {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, dataBuf.get(), &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    memcpy(out + totalCopied, dataBuf.get() + offsetInBlock, chunk);
                } else {
                    // 구멍(hole) - 파일 구멍은 논리적으로 0(POSIX sparse file 관례).
                    memset(out + totalCopied, 0, chunk);
                }

                totalCopied += chunk;
                remaining -= chunk;
            }
            args->result =
                kernel::ReadResult{totalCopied, ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None};
            break;
        }

        case kernel::KernelFsOpCode::Write: {
            // libext4 1차 증분은 쓰기 경로가 없다(SP-7A9CED3E §5의
            // 익스텐트 트리 분할 알고리즘 등 미결) - 조용히 무시하지
            // 않고 명시적으로 거부한다(LiveFs 관례).
            auto* args = static_cast<kernel::KernelFsWriteArgs*>(argsRaw);
            args->bytesWritten = 0;
            args->error = kernel::VfsError::PermissionDenied;
            break;
        }

        case kernel::KernelFsOpCode::Stat: {
            auto* args = static_cast<kernel::KernelFsStatArgs*>(argsRaw);
            uint32_t currentInode = kRootInodeNumber;
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
                if (!currentIsDir) {
                    failed = true;
                    break;
                }

                uint64_t inodeBlockOffset = 0;
                uint32_t inodeByteOffset = 0;
                uint32_t inodeBlocksNeeded = 0;
                if (!kLocateInode(sb, volume_, groupCount, blockSize, currentInode, &inodeBlockOffset,
                                   &inodeByteOffset, &inodeBlocksNeeded)) {
                    failed = true;
                    break;
                }
                SlabBuf inodeBuf(inodeBlocksNeeded * blockSize);
                if (!inodeBuf) {
                    failed = true;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, inodeBlockOffset, inodeBlocksNeeded, inodeBuf.get(),
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
                }
                InodeCore dirInode;
                memcpy(&dirInode, inodeBuf.get() + inodeByteOffset, sizeof(dirInode));
                if (!kIsDirMode(dirInode.mode)) {
                    failed = true;
                    break;
                }

                const uint64_t dirSize = dirInode.sizeLo | (static_cast<uint64_t>(dirInode.sizeHigh) << 32);
                const uint32_t dirBlockCount = static_cast<uint32_t>(kCeilDiv(dirSize, blockSize));
                bool foundInThisDir = false;

                for (uint32_t logicalBlock = 0; logicalBlock < dirBlockCount && !foundInThisDir; ++logicalBlock) {
                    uint64_t nodeValue = 0;
                    ExtentLookup lookup;
                    if (dirInode.flags & kExtentsFl) {
                        lookup = kLookupExtent(dirInode.block, logicalBlock, &nodeValue);
                        uint32_t depthGuard = 5;
                        SlabBuf extentNodeBuf(blockSize);
                        while (lookup == ExtentLookup::NeedChild && depthGuard > 0) {
                            if (!extentNodeBuf) {
                                lookup = ExtentLookup::Invalid;
                                break;
                            }
                            fs::BlockIoResult ioResult;
                            kernel::AsyncTask* ioTask =
                                kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, extentNodeBuf.get(), &ioResult);
                            if (!ioTask) {
                                lookup = ExtentLookup::Invalid;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                            if (!ioResult.ok) {
                                lookup = ExtentLookup::Invalid;
                                break;
                            }
                            lookup = kLookupExtent(extentNodeBuf.get(), logicalBlock, &nodeValue);
                            --depthGuard;
                        }
                    } else {
                        // [신규, PN-E3629BE9] 레거시 간접 블록(ext2/ext3 호환).
                        const uint32_t pointersPerBlock = blockSize / sizeof(uint32_t);
                        const auto* rootBlocks = reinterpret_cast<const uint32_t*>(dirInode.block);
                        const IndirectResolution res = kResolveIndirect(logicalBlock, pointersPerBlock);
                        if (res.level == IndirectLevel::OutOfRange) {
                            lookup = ExtentLookup::Hole;
                        } else if (res.level == IndirectLevel::Direct) {
                            nodeValue = rootBlocks[res.index0];
                            lookup = nodeValue == 0 ? ExtentLookup::Hole : ExtentLookup::Found;
                        } else {
                            uint32_t indices[3];
                            uint32_t hops;
                            uint32_t currentBlockNum;
                            if (res.level == IndirectLevel::Single) {
                                indices[0] = res.index0;
                                hops = 1;
                                currentBlockNum = rootBlocks[12];
                            } else if (res.level == IndirectLevel::Double) {
                                indices[0] = res.index0;
                                indices[1] = res.index1;
                                hops = 2;
                                currentBlockNum = rootBlocks[13];
                            } else {
                                indices[0] = res.index0;
                                indices[1] = res.index1;
                                indices[2] = res.index2;
                                hops = 3;
                                currentBlockNum = rootBlocks[14];
                            }
                            if (currentBlockNum == 0) {
                                lookup = ExtentLookup::Hole;
                            } else {
                                lookup = ExtentLookup::Found;
                                for (uint32_t h = 0; h < hops; ++h) {
                                    SlabBuf indBuf(blockSize);
                                    if (!indBuf) {
                                        lookup = ExtentLookup::Invalid;
                                        break;
                                    }
                                    fs::BlockIoResult ioResult;
                                    kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(
                                        device, blockSize, currentBlockNum, 1, indBuf.get(), &ioResult);
                                    if (!ioTask) {
                                        lookup = ExtentLookup::Invalid;
                                        break;
                                    }
                                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                                    if (!ioResult.ok) {
                                        lookup = ExtentLookup::Invalid;
                                        break;
                                    }
                                    const uint32_t nextPtr =
                                        reinterpret_cast<const uint32_t*>(indBuf.get())[indices[h]];
                                    if (nextPtr == 0) {
                                        lookup = ExtentLookup::Hole;
                                        break;
                                    }
                                    if (h + 1 == hops) {
                                        nodeValue = nextPtr;
                                    } else {
                                        currentBlockNum = nextPtr;
                                    }
                                }
                            }
                        }
                    }
                    if (lookup != ExtentLookup::Found) {
                        continue;
                    }

                    SlabBuf dataBuf(blockSize);
                    if (!dataBuf) {
                        failed = true;
                        break;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, dataBuf.get(), &ioResult);
                    if (!ioTask) {
                        failed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        failed = true;
                        break;
                    }

                    uint32_t matchedInode = 0;
                    uint8_t matchedType = 0;
                    if (kScanDirBlockForName(dataBuf.get(), blockSize, args->relPath + segStart, segLen,
                                              &matchedInode, &matchedType)) {
                        currentInode = matchedInode;
                        currentIsDir = (matchedType == kFtDir);
                        foundInThisDir = true;
                    }
                }
                if (!foundInThisDir) {
                    failed = true;
                }
            }

            if (failed) {
                args->error = kernel::VfsError::NotFound;
                break;
            }

            uint64_t targetBlockOffset = 0;
            uint32_t targetByteOffset = 0;
            uint32_t targetBlocksNeeded = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, currentInode, &targetBlockOffset,
                               &targetByteOffset, &targetBlocksNeeded)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            SlabBuf targetInodeBuf(targetBlocksNeeded * blockSize);
            if (!targetInodeBuf) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            fs::BlockIoResult statIoResult;
            kernel::AsyncTask* statIoTask = kSubmitReadExtBlocks(
                device, blockSize, targetBlockOffset, targetBlocksNeeded, targetInodeBuf.get(), &statIoResult);
            if (!statIoTask) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            co_await kernel::AsyncTaskCoroAwaiter(statIoTask);
            if (!statIoResult.ok) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            InodeCore targetInode;
            memcpy(&targetInode, targetInodeBuf.get() + targetByteOffset, sizeof(targetInode));
            args->size = targetInode.sizeLo | (static_cast<uint64_t>(targetInode.sizeHigh) << 32);
            args->isDirectory = currentIsDir;
            args->error = kernel::VfsError::None;
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
            const uint32_t dirInodeNum = static_cast<uint32_t>(args->dirHandle.value);

            uint64_t inodeBlockOffset = 0;
            uint32_t inodeByteOffset = 0;
            uint32_t inodeBlocksNeeded = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, dirInodeNum, &inodeBlockOffset,
                               &inodeByteOffset, &inodeBlocksNeeded)) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            SlabBuf inodeBuf(inodeBlocksNeeded * blockSize);
            if (!inodeBuf) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, inodeBlockOffset,
                                                                  inodeBlocksNeeded, inodeBuf.get(), &ioResult);
                if (!ioTask) {
                    args->hasMore = false;
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->hasMore = false;
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
            }
            InodeCore dirInode;
            memcpy(&dirInode, inodeBuf.get() + inodeByteOffset, sizeof(dirInode));
            if (!kIsDirMode(dirInode.mode)) {
                args->hasMore = false;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            const uint64_t dirSize = dirInode.sizeLo | (static_cast<uint64_t>(dirInode.sizeHigh) << 32);
            const uint32_t dirBlockCount = static_cast<uint32_t>(kCeilDiv(dirSize, blockSize));
            uint64_t seen = 0;
            bool found = false;
            bool ioFailed = false;

            for (uint32_t logicalBlock = 0; logicalBlock < dirBlockCount && !found && !ioFailed; ++logicalBlock) {
                uint64_t nodeValue = 0;
                ExtentLookup lookup;
                if (dirInode.flags & kExtentsFl) {
                    lookup = kLookupExtent(dirInode.block, logicalBlock, &nodeValue);
                    uint32_t depthGuard = 5;
                    SlabBuf extentNodeBuf(blockSize);
                    while (lookup == ExtentLookup::NeedChild && depthGuard > 0) {
                        if (!extentNodeBuf) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask =
                            kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, extentNodeBuf.get(), &ioResult);
                        if (!ioTask) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        lookup = kLookupExtent(extentNodeBuf.get(), logicalBlock, &nodeValue);
                        --depthGuard;
                    }
                } else {
                    // [신규, PN-E3629BE9] 레거시 간접 블록(ext2/ext3 호환).
                    const uint32_t pointersPerBlock = blockSize / sizeof(uint32_t);
                    const auto* rootBlocks = reinterpret_cast<const uint32_t*>(dirInode.block);
                    const IndirectResolution res = kResolveIndirect(logicalBlock, pointersPerBlock);
                    if (res.level == IndirectLevel::OutOfRange) {
                        lookup = ExtentLookup::Hole;
                    } else if (res.level == IndirectLevel::Direct) {
                        nodeValue = rootBlocks[res.index0];
                        lookup = nodeValue == 0 ? ExtentLookup::Hole : ExtentLookup::Found;
                    } else {
                        uint32_t indices[3];
                        uint32_t hops;
                        uint32_t currentBlockNum;
                        if (res.level == IndirectLevel::Single) {
                            indices[0] = res.index0;
                            hops = 1;
                            currentBlockNum = rootBlocks[12];
                        } else if (res.level == IndirectLevel::Double) {
                            indices[0] = res.index0;
                            indices[1] = res.index1;
                            hops = 2;
                            currentBlockNum = rootBlocks[13];
                        } else {
                            indices[0] = res.index0;
                            indices[1] = res.index1;
                            indices[2] = res.index2;
                            hops = 3;
                            currentBlockNum = rootBlocks[14];
                        }
                        if (currentBlockNum == 0) {
                            lookup = ExtentLookup::Hole;
                        } else {
                            lookup = ExtentLookup::Found;
                            for (uint32_t h = 0; h < hops; ++h) {
                                SlabBuf indBuf(blockSize);
                                if (!indBuf) {
                                    lookup = ExtentLookup::Invalid;
                                    break;
                                }
                                fs::BlockIoResult ioResult;
                                kernel::AsyncTask* ioTask =
                                    kSubmitReadExtBlocks(device, blockSize, currentBlockNum, 1, indBuf.get(), &ioResult);
                                if (!ioTask) {
                                    lookup = ExtentLookup::Invalid;
                                    break;
                                }
                                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                                if (!ioResult.ok) {
                                    lookup = ExtentLookup::Invalid;
                                    break;
                                }
                                const uint32_t nextPtr = reinterpret_cast<const uint32_t*>(indBuf.get())[indices[h]];
                                if (nextPtr == 0) {
                                    lookup = ExtentLookup::Hole;
                                    break;
                                }
                                if (h + 1 == hops) {
                                    nodeValue = nextPtr;
                                } else {
                                    currentBlockNum = nextPtr;
                                }
                            }
                        }
                    }
                }
                if (lookup != ExtentLookup::Found) {
                    continue;
                }

                SlabBuf dataBuf(blockSize);
                if (!dataBuf) {
                    ioFailed = true;
                    break;
                }
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, dataBuf.get(), &ioResult);
                if (!ioTask) {
                    ioFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    ioFailed = true;
                    break;
                }

                uint32_t entryInodeUnused = 0;  // VfsDirEntry는 inode 번호를 담지 않는다 - 계약상 호출부가 안 씀
                if (kScanDirBlockForIndex(dataBuf.get(), blockSize, args->index, &seen, args->entry.name,
                                           sizeof(args->entry.name), &args->entry.nameLength, &args->entry.isDirectory,
                                           &entryInodeUnused)) {
                    found = true;
                }
            }

            args->hasMore = found;
            args->error = ioFailed ? kernel::VfsError::InvalidHandle : kernel::VfsError::None;
            break;
        }
    }
    co_return;
}

}  // namespace ext4
