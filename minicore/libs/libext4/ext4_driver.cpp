#include "ext4_driver.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "rtc.h"

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

// [신규, 2026-09-25, PN-FE718C87] kSubmitReadExtBlocks의 쓰기 버전 -
// 이 드라이버 최초의 실제 쓰기 I/O(지금까지는 전부 읽기 전용이었음).
// 동일한 LBA 변환 그대로.
kernel::AsyncTask* kSubmitWriteExtBlocks(fs::BlockDevice* device, uint32_t extBlockSize, uint64_t extBlockStart,
                                          uint32_t extBlockCount, const void* buf, fs::BlockIoResult* outResult) {
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0 || extBlockSize % devBlockSize != 0) {
        return nullptr;
    }
    const kernel::uint32_t devBlocksPerExtBlock = extBlockSize / devBlockSize;
    return device->submitWriteBlocks(extBlockStart * devBlocksPerExtBlock, buf,
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

// [신규, 2026-09-25, PN-FE718C87] relPath를 "부모 디렉터리 경로" +
// "마지막 세그먼트(leaf 이름)"로 나눈다(순수 계산, I/O 없음) - libvfat
// (vfat_driver.cpp)의 동명 함수와 완전히 동일한 로직(이 프로젝트가
// 새로 고안한 관례가 아니라 이미 검증된 패턴 재사용 - 네임스페이스가
// 달라 그대로 복사). Mkdir/Rmdir/Unlink가 공통으로 필요. relPathLen==0
// 이거나 leaf가 빈 문자열(경로가 "/"로만 끝남)이면 false.
bool kSplitParentAndLeaf(const char* relPath, uint32_t relPathLen, uint32_t* outParentLen, uint32_t* outLeafStart,
                          uint32_t* outLeafLen) {
    if (relPathLen == 0) {
        return false;
    }
    uint32_t end = relPathLen;
    while (end > 0 && relPath[end - 1] == '/') {
        --end;
    }
    if (end == 0) {
        return false;  // 경로가 전부 '/'뿐
    }
    uint32_t leafStart = end;
    while (leafStart > 0 && relPath[leafStart - 1] != '/') {
        --leafStart;
    }
    if (leafStart == end) {
        return false;
    }
    uint32_t parentLen = leafStart;
    while (parentLen > 0 && relPath[parentLen - 1] == '/') {
        --parentLen;
    }
    *outParentLen = parentLen;
    *outLeafStart = leafStart;
    *outLeafLen = end - leafStart;
    return true;
}

// [신규, 2026-09-25, PN-FE718C87] 그룹 group의 온디스크 그룹 디스크립터
// 하나가 그룹 디스크립터 테이블(GDT) 안 어느 블록의 어느 바이트
// 오프셋에 있는지 계산하는 순수 함수(I/O 없음) - `Ext4Volume::mount()`
// (ext4.cpp)가 GDT 전체를 캐싱할 때 쓰는 것과 동일한 계산
// (`gdtStartBlock = firstDataBlock + 1`, stride는 32/64바이트)이지만
// `Ext4Volume`은 그 캐시를 읽기 전용으로만 노출하고 원시 바이트를
// 밖으로 주지 않으므로(4.항 클래스 주석 참고), 쓰기 경로는 이
// 계산을 직접 다시 하고 독자적으로 그 블록을 읽어 온다(파일 상단
// 문서 주석의 "합성 불가로 의도적 중복" 관례와 같은 이유는 아니지만
// - 이건 코루틴이 아니라 캡슐화 경계 문제 - 결과적으로 같은 모양의
// 중복이 생긴다). descSize/blockSize 관계상 디스크립터 하나가 블록
// 경계를 넘어 걸치는 일은 없다(blockSize는 항상 32/64보다 훨씬 큰
// 2의 거듭제곱).
void kLocateGroupDesc(uint64_t gdtStartBlock, uint32_t descSize, uint32_t blockSize, uint32_t group,
                       uint64_t* outBlockOffset, uint32_t* outByteOffsetInBlock) {
    const uint64_t byteOffsetInGdt = static_cast<uint64_t>(group) * descSize;
    *outBlockOffset = gdtStartBlock + byteOffsetInGdt / blockSize;
    *outByteOffsetInBlock = static_cast<uint32_t>(byteOffsetInGdt % blockSize);
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
            // [구현, 2026-09-25, PN-FE718C87] ext4 최초의 실제 쓰기
            // 오퍼레이션. v1 범위는 libvfat Mkdir(vfat_driver.cpp)과
            // 동일 - 부모 디렉터리 확장(새 블록 할당)은 미지원, 부모에
            // 빈 슬롯이 전혀 없으면 NoSpace로 정직하게 거부. 새
            // 디렉터리 자신은 항상 인라인 익스텐트(1블록)로 시작하므로
            // 실제 익스텐트 트리 확장/분할(§5, 여전히 미구현)은 필요
            // 없다. 그룹 선택 정책은 "부모의 그룹부터 순서대로 스캔"
            // (PN-FE718C87 계획 본문에 이미 명시된 v1 단순화).
            auto* args = static_cast<kernel::KernelFsMkdirArgs*>(argsRaw);
            if (readOnly_) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            uint32_t parentLen = 0, leafStart = 0, leafLen = 0;
            if (!kSplitParentAndLeaf(args->relPath, args->relPathLen, &parentLen, &leafStart, &leafLen) ||
                leafLen > kMaxNameLen) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }

            // 1) 부모 디렉터리 inode 번호를 찾는다 - Open의 경로 세그먼트
            //    순회와 동일한 패턴(파일 상단 문서 주석의 "합성 불가"
            //    제약으로 함수 공유 불가, 의도적 중복), parentLen까지만
            //    순회한다.
            uint32_t parentInodeNum = kRootInodeNumber;
            bool parentIsDir = true;
            bool parentFailed = false;
            {
                uint32_t pos = 0;
                while (pos < parentLen && !parentFailed) {
                    while (pos < parentLen && args->relPath[pos] == '/') {
                        ++pos;
                    }
                    if (pos >= parentLen) {
                        break;
                    }
                    const uint32_t segStart = pos;
                    while (pos < parentLen && args->relPath[pos] != '/') {
                        ++pos;
                    }
                    const uint32_t segLen = pos - segStart;
                    if (!parentIsDir) {
                        parentFailed = true;
                        break;
                    }

                    uint64_t segInodeBlockOffset = 0;
                    uint32_t segInodeByteOffset = 0;
                    uint32_t segInodeBlocksNeeded = 0;
                    if (!kLocateInode(sb, volume_, groupCount, blockSize, parentInodeNum, &segInodeBlockOffset,
                                       &segInodeByteOffset, &segInodeBlocksNeeded)) {
                        parentFailed = true;
                        break;
                    }
                    SlabBuf segInodeBuf(segInodeBlocksNeeded * blockSize);
                    if (!segInodeBuf) {
                        parentFailed = true;
                        break;
                    }
                    {
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(
                            device, blockSize, segInodeBlockOffset, segInodeBlocksNeeded, segInodeBuf.get(), &ioResult);
                        if (!ioTask) {
                            parentFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            parentFailed = true;
                            break;
                        }
                    }
                    InodeCore segInode;
                    memcpy(&segInode, segInodeBuf.get() + segInodeByteOffset, sizeof(segInode));
                    if (!kIsDirMode(segInode.mode)) {
                        parentFailed = true;
                        break;
                    }

                    const uint64_t segDirSize = segInode.sizeLo | (static_cast<uint64_t>(segInode.sizeHigh) << 32);
                    const uint32_t segDirBlockCount = static_cast<uint32_t>(kCeilDiv(segDirSize, blockSize));
                    bool foundSeg = false;
                    for (uint32_t logicalBlock = 0; logicalBlock < segDirBlockCount && !foundSeg; ++logicalBlock) {
                        uint64_t nodeValue = 0;
                        ExtentLookup lookup;
                        if (segInode.flags & kExtentsFl) {
                            lookup = kLookupExtent(segInode.block, logicalBlock, &nodeValue);
                            uint32_t depthGuard = 5;
                            SlabBuf extentNodeBuf(blockSize);
                            while (lookup == ExtentLookup::NeedChild && depthGuard > 0) {
                                if (!extentNodeBuf) {
                                    lookup = ExtentLookup::Invalid;
                                    break;
                                }
                                fs::BlockIoResult ioResult;
                                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, nodeValue, 1,
                                                                                  extentNodeBuf.get(), &ioResult);
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
                            const uint32_t pointersPerBlock = blockSize / sizeof(uint32_t);
                            const auto* rootBlocks = reinterpret_cast<const uint32_t*>(segInode.block);
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
                            parentFailed = true;
                            break;
                        }
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask =
                            kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, dataBuf.get(), &ioResult);
                        if (!ioTask) {
                            parentFailed = true;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            parentFailed = true;
                            break;
                        }
                        uint32_t matchedInode = 0;
                        uint8_t matchedType = 0;
                        if (kScanDirBlockForName(dataBuf.get(), blockSize, args->relPath + segStart, segLen,
                                                  &matchedInode, &matchedType)) {
                            parentInodeNum = matchedInode;
                            parentIsDir = (matchedType == kFtDir);
                            foundSeg = true;
                        }
                    }
                    if (!foundSeg) {
                        parentFailed = true;
                    }
                }
            }
            if (parentFailed || !parentIsDir) {
                args->error = kernel::VfsError::NotFound;
                break;
            }

            // 2) 부모 자신의 inode 구조체를 읽는다(디렉터리 확인 +
            //    이후 블록 순회/linksCount 갱신에 필요) - 위 루프의
            //    마지막 반복은 부모의 "부모"를 읽은 것이므로 별도로
            //    다시 읽어야 한다. inodeSize바이트 전체(보통 256)가
            //    유효 범위여야 나중에 체크섬 재계산이 안전하다(단순
            //    132바이트 InodeCore 크기로는 부족할 수 있음).
            uint64_t parentInodeBlockOffset = 0;
            uint32_t parentInodeByteOffset = 0;
            uint32_t parentInodeBlocksNeededProbe = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, parentInodeNum, &parentInodeBlockOffset,
                               &parentInodeByteOffset, &parentInodeBlocksNeededProbe)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            const uint32_t parentInodeBlocksNeeded = static_cast<uint32_t>(
                kCeilDiv(static_cast<uint64_t>(parentInodeByteOffset) + sb.inodeSize, blockSize));
            SlabBuf parentInodeBuf(parentInodeBlocksNeeded * blockSize);
            if (!parentInodeBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(
                    device, blockSize, parentInodeBlockOffset, parentInodeBlocksNeeded, parentInodeBuf.get(), &ioResult);
                if (!ioTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }
            InodeCore parentInode;
            memcpy(&parentInode, parentInodeBuf.get() + parentInodeByteOffset, sizeof(parentInode));
            if (!kIsDirMode(parentInode.mode)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            if (!(parentInode.flags & kExtentsFl)) {
                // v1 쓰기 경로는 익스텐트 기반 디렉터리에만 새 엔트리를
                // 추가할 수 있다(레거시 간접 블록 부모 확장은 범위 밖).
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }

            const uint64_t parentDirSize = parentInode.sizeLo | (static_cast<uint64_t>(parentInode.sizeHigh) << 32);
            const uint32_t parentDirBlockCount = static_cast<uint32_t>(kCeilDiv(parentDirSize, blockSize));
            const bool metadataCsum = (sb.featureRoCompat & kRoCompatMetadataCsum) != 0;
            const uint32_t hasTailBytes = metadataCsum ? sizeof(DirEntryTail) : 0;

            // 3) 부모의 데이터 블록들을 한 번 순회해 (a) 동명 엔트리
            //    존재 여부와 (b) 삽입 가능한 블록을 동시에 찾는다 -
            //    존재 여부는 뒤쪽 블록에 중복이 있을 수 있으므로 후보를
            //    찾아도 스캔을 계속한다. 삽입 가능성은 스크래치 복사본
            //    위에서만 시험한다(실제 inode 번호는 아직 없음 - 자리가
            //    있는지만 먼저 확인해 불필요한 블록/inode 소비를
            //    피한다).
            bool leafExists = false;
            bool haveCandidate = false;
            uint64_t candidateBlockAbs = 0;
            {
                bool ioFailed = false;
                SlabBuf scanBuf(blockSize);
                SlabBuf scratchBuf(blockSize);
                if (!scanBuf || !scratchBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                for (uint32_t logicalBlock = 0; logicalBlock < parentDirBlockCount && !leafExists; ++logicalBlock) {
                    uint64_t nodeValue = 0;
                    ExtentLookup lookup = kLookupExtent(parentInode.block, logicalBlock, &nodeValue);
                    uint32_t depthGuard = 5;
                    SlabBuf extentNodeBuf(blockSize);
                    while (lookup == ExtentLookup::NeedChild && depthGuard > 0) {
                        if (!extentNodeBuf) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        fs::BlockIoResult exIo;
                        kernel::AsyncTask* exTask =
                            kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, extentNodeBuf.get(), &exIo);
                        if (!exTask) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(exTask);
                        if (!exIo.ok) {
                            lookup = ExtentLookup::Invalid;
                            break;
                        }
                        lookup = kLookupExtent(extentNodeBuf.get(), logicalBlock, &nodeValue);
                        --depthGuard;
                    }
                    if (lookup != ExtentLookup::Found) {
                        continue;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, scanBuf.get(), &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                        break;
                    }

                    uint32_t existingInode = 0;
                    uint8_t existingType = 0;
                    if (kScanDirBlockForName(scanBuf.get(), blockSize, args->relPath + leafStart, leafLen,
                                              &existingInode, &existingType)) {
                        leafExists = true;
                        break;
                    }
                    if (!haveCandidate) {
                        memcpy(scratchBuf.get(), scanBuf.get(), blockSize);
                        if (kExt4InsertDirEntry(scratchBuf.get(), blockSize, hasTailBytes, 0xFFFFFFFFu,
                                                  args->relPath + leafStart, static_cast<uint8_t>(leafLen), kFtDir)) {
                            haveCandidate = true;
                            candidateBlockAbs = nodeValue;
                        }
                    }
                }
                if (ioFailed) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }
            if (leafExists) {
                args->error = kernel::VfsError::AlreadyExists;
                break;
            }
            if (!haveCandidate) {
                args->error = kernel::VfsError::NoSpace;
                break;
            }

            // 4) inode를 먼저 할당(부모의 그룹부터 순서대로 스캔) -
            //    블록 할당이 나중에 실패하면 이 inode를 되돌린다(아래
            //    5단계의 롤백 참고). 비트맵/그룹 디스크립터 버퍼는
            //    롤백에 다시 쓸 수 있도록 이 스코프 밖까지 살려 둔다.
            const bool is64Bit = (sb.featureIncompat & kIncompat64Bit) != 0;
            const uint32_t descSize = is64Bit ? sizeof(GroupDesc64) : sizeof(GroupDesc32);
            const uint64_t gdtStartBlock = static_cast<uint64_t>(sb.firstDataBlock) + 1;
            const uint32_t parentGroup = (parentInodeNum - 1) / sb.inodesPerGroup;

            uint32_t newInodeNum = 0;
            uint32_t newInodeGroup = 0;
            uint32_t newInodeRelIndex = 0;
            SlabBuf inodeBitmapBuf(blockSize);
            SlabBuf inodeGdBuf(blockSize);
            uint64_t inodeBitmapBlock = 0;
            uint64_t inodeGdBlockOffset = 0;
            uint32_t inodeGdByteOffset = 0;
            if (!inodeBitmapBuf || !inodeGdBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                bool ioFailed = false;
                bool allocated = false;
                for (uint32_t offset = 0; offset < groupCount; ++offset) {
                    const uint32_t group = (parentGroup + offset) % groupCount;
                    inodeBitmapBlock = volume_.groupInodeBitmapBlock(group);
                    kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &inodeGdBlockOffset,
                                      &inodeGdByteOffset);

                    fs::BlockIoResult bmIo;
                    kernel::AsyncTask* bmTask =
                        kSubmitReadExtBlocks(device, blockSize, inodeBitmapBlock, 1, inodeBitmapBuf.get(), &bmIo);
                    if (!bmTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(bmTask);
                    if (!bmIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    fs::BlockIoResult gdIo;
                    kernel::AsyncTask* gdTask =
                        kSubmitReadExtBlocks(device, blockSize, inodeGdBlockOffset, 1, inodeGdBuf.get(), &gdIo);
                    if (!gdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(gdTask);
                    if (!gdIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    if (!kExt4AllocateInodeInGroup(sb.uuid, group, sb.inodesPerGroup, sb.featureRoCompat, is64Bit,
                                                    inodeBitmapBuf.get(), inodeGdBuf.get() + inodeGdByteOffset,
                                                    &newInodeRelIndex)) {
                        continue;
                    }
                    // Mkdir이므로 항상 디렉터리 inode - bg_used_dirs_count
                    // 도 함께 갱신(kExt4AllocateInodeInGroup은 inode
                    // 종류를 모르는 범용 할당자라 이 값을 대신 못 다룸,
                    // ext4.h의 kExt4AdjustGroupDescUsedDirs 문서 주석
                    // 참고 - PN-4C67E1ED 실측으로 발견된 갭).
                    kExt4AdjustGroupDescUsedDirs(sb.uuid, group, sb.featureRoCompat, is64Bit,
                                                  inodeGdBuf.get() + inodeGdByteOffset, /*delta=*/1);

                    fs::BlockIoResult wBmIo;
                    kernel::AsyncTask* wBmTask =
                        kSubmitWriteExtBlocks(device, blockSize, inodeBitmapBlock, 1, inodeBitmapBuf.get(), &wBmIo);
                    if (!wBmTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(wBmTask);
                    if (!wBmIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    fs::BlockIoResult wGdIo;
                    kernel::AsyncTask* wGdTask =
                        kSubmitWriteExtBlocks(device, blockSize, inodeGdBlockOffset, 1, inodeGdBuf.get(), &wGdIo);
                    if (!wGdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(wGdTask);
                    if (!wGdIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    newInodeGroup = group;
                    newInodeNum = group * sb.inodesPerGroup + newInodeRelIndex + 1;
                    allocated = true;
                    break;
                }
                if (ioFailed) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                if (!allocated) {
                    args->error = kernel::VfsError::NoSpace;
                    break;
                }
            }

            // 5) 블록을 할당(부모의 그룹부터 순서대로 스캔, inode와 독립
            //    - ext4는 블록/inode가 같은 그룹일 것을 요구하지 않는다,
            //    지역성은 성능 최적화일 뿐). 실패하면 위에서 이미 커밋한
            //    inode 할당을 되돌린다(4단계에서 살려 둔 비트맵/그룹
            //    디스크립터 버퍼 재사용 - 단일 코루틴만 디스크를
            //    건드리므로 재확인 없이 그대로 반전 가능).
            uint64_t newBlockAbs = 0;
            {
                SlabBuf blockBitmapBuf(blockSize);
                SlabBuf blockGdBuf(blockSize);
                if (!blockBitmapBuf || !blockGdBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                bool ioFailed = false;
                bool allocated = false;
                for (uint32_t offset = 0; offset < groupCount; ++offset) {
                    const uint32_t group = (parentGroup + offset) % groupCount;
                    const uint64_t bitmapBlock = volume_.groupBlockBitmapBlock(group);
                    uint64_t gdBlockOffset = 0;
                    uint32_t gdByteOffset = 0;
                    kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset, &gdByteOffset);

                    fs::BlockIoResult bmIo;
                    kernel::AsyncTask* bmTask =
                        kSubmitReadExtBlocks(device, blockSize, bitmapBlock, 1, blockBitmapBuf.get(), &bmIo);
                    if (!bmTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(bmTask);
                    if (!bmIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    fs::BlockIoResult gdIo;
                    kernel::AsyncTask* gdTask =
                        kSubmitReadExtBlocks(device, blockSize, gdBlockOffset, 1, blockGdBuf.get(), &gdIo);
                    if (!gdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(gdTask);
                    if (!gdIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    uint32_t relIndex = 0;
                    if (!kExt4AllocateBlockInGroup(sb.uuid, group, sb.blocksPerGroup, sb.featureRoCompat, is64Bit,
                                                    blockBitmapBuf.get(), blockGdBuf.get() + gdByteOffset,
                                                    &relIndex)) {
                        continue;
                    }

                    fs::BlockIoResult wBmIo;
                    kernel::AsyncTask* wBmTask =
                        kSubmitWriteExtBlocks(device, blockSize, bitmapBlock, 1, blockBitmapBuf.get(), &wBmIo);
                    if (!wBmTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(wBmTask);
                    if (!wBmIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    fs::BlockIoResult wGdIo;
                    kernel::AsyncTask* wGdTask =
                        kSubmitWriteExtBlocks(device, blockSize, gdBlockOffset, 1, blockGdBuf.get(), &wGdIo);
                    if (!wGdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(wGdTask);
                    if (!wGdIo.ok) {
                        ioFailed = true;
                        break;
                    }

                    newBlockAbs = static_cast<uint64_t>(sb.firstDataBlock) +
                                  static_cast<uint64_t>(group) * sb.blocksPerGroup + relIndex;
                    allocated = true;
                    break;
                }
                if (ioFailed || !allocated) {
                    kExt4FreeInodeInGroup(sb.uuid, newInodeGroup, sb.inodesPerGroup, sb.featureRoCompat, is64Bit,
                                           inodeBitmapBuf.get(), inodeGdBuf.get() + inodeGdByteOffset,
                                           newInodeRelIndex);
                    // 위에서 +1 했던 bg_used_dirs_count도 함께 되돌린다
                    // (이 inode 할당 자체를 통째로 취소하는 것이므로).
                    kExt4AdjustGroupDescUsedDirs(sb.uuid, newInodeGroup, sb.featureRoCompat, is64Bit,
                                                  inodeGdBuf.get() + inodeGdByteOffset, /*delta=*/-1);
                    fs::BlockIoResult rwBmIo;
                    kernel::AsyncTask* rwBmTask =
                        kSubmitWriteExtBlocks(device, blockSize, inodeBitmapBlock, 1, inodeBitmapBuf.get(), &rwBmIo);
                    if (rwBmTask) {
                        co_await kernel::AsyncTaskCoroAwaiter(rwBmTask);
                    }
                    fs::BlockIoResult rwGdIo;
                    kernel::AsyncTask* rwGdTask = kSubmitWriteExtBlocks(device, blockSize, inodeGdBlockOffset, 1,
                                                                         inodeGdBuf.get(), &rwGdIo);
                    if (rwGdTask) {
                        co_await kernel::AsyncTaskCoroAwaiter(rwGdTask);
                    }
                    args->error = ioFailed ? kernel::VfsError::InvalidArgument : kernel::VfsError::NoSpace;
                    break;
                }
            }

            // 6) 새 inode 내용 준비(kExt4InitDirInode) + 체크섬 계산 +
            //    inode 테이블에 기록. 새로 할당된(=이전 점유자가 있었을
            //    수 있는) 슬롯이므로 inodeSize 전체를 0으로 지운 뒤
            //    InodeCore를 앞에 채운다(오래된 확장 필드 잔재 방지).
            const uint32_t epochSeconds = kernel::Rtc::toEpochSeconds(kernel::Rtc::readWallClock());
            InodeCore newInode{};
            kExt4InitDirInode(&newInode, blockSize, newBlockAbs, epochSeconds, /*uid=*/0, /*gid=*/0);

            uint64_t newInodeBlockOffset = 0;
            uint32_t newInodeByteOffset = 0;
            uint32_t newInodeBlocksNeededProbe = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, newInodeNum, &newInodeBlockOffset,
                               &newInodeByteOffset, &newInodeBlocksNeededProbe)) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            const uint32_t newInodeBlocksNeeded = static_cast<uint32_t>(
                kCeilDiv(static_cast<uint64_t>(newInodeByteOffset) + sb.inodeSize, blockSize));
            SlabBuf newInodeBlockBuf(newInodeBlocksNeeded * blockSize);
            if (!newInodeBlockBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(
                    device, blockSize, newInodeBlockOffset, newInodeBlocksNeeded, newInodeBlockBuf.get(), &ioResult);
                if (!ioTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }
            uint8_t* newInodeSlot = newInodeBlockBuf.get() + newInodeByteOffset;
            memset(newInodeSlot, 0, sb.inodeSize);
            memcpy(newInodeSlot, &newInode, sizeof(newInode));
            if (metadataCsum) {
                const uint32_t crc =
                    kExt4ComputeInodeChecksum(sb.uuid, newInodeNum, /*generation=*/0, newInodeSlot, sb.inodeSize);
                const uint16_t csumLo = static_cast<uint16_t>(crc & 0xFFFFu);
                const uint16_t csumHi = static_cast<uint16_t>(crc >> 16);
                memcpy(newInodeSlot + 124, &csumLo, sizeof(csumLo));
                memcpy(newInodeSlot + 130, &csumHi, sizeof(csumHi));
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitWriteExtBlocks(
                    device, blockSize, newInodeBlockOffset, newInodeBlocksNeeded, newInodeBlockBuf.get(), &ioResult);
                if (!ioTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 7) 새 디렉터리 자신의 데이터 블록에 "."/".." 엔트리를
            //    직접 구성해 쓴다(빈 새 블록이므로 kExt4InsertDirEntry
            //    대신 직접 구성).
            SlabBuf newDirBlockBuf(blockSize);
            if (!newDirBlockBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            memset(newDirBlockBuf.get(), 0, blockSize);
            {
                uint32_t pos = 0;
                DirEntry2Header dotHdr{};
                dotHdr.inode = newInodeNum;
                dotHdr.recLen = static_cast<uint16_t>(kExt4DirRecLen(1));
                dotHdr.nameLen = 1;
                dotHdr.fileType = kFtDir;
                memcpy(newDirBlockBuf.get() + pos, &dotHdr, sizeof(dotHdr));
                newDirBlockBuf.get()[pos + sizeof(dotHdr)] = '.';
                pos += dotHdr.recLen;

                const uint32_t tailReserve = hasTailBytes;
                DirEntry2Header dotDotHdr{};
                dotDotHdr.inode = parentInodeNum;
                dotDotHdr.recLen = static_cast<uint16_t>(blockSize - pos - tailReserve);
                dotDotHdr.nameLen = 2;
                dotDotHdr.fileType = kFtDir;
                memcpy(newDirBlockBuf.get() + pos, &dotDotHdr, sizeof(dotDotHdr));
                newDirBlockBuf.get()[pos + sizeof(dotDotHdr)] = '.';
                newDirBlockBuf.get()[pos + sizeof(dotDotHdr) + 1] = '.';

                if (hasTailBytes) {
                    DirEntryTail tail{};
                    tail.header.inode = 0;
                    tail.header.recLen = sizeof(DirEntryTail);
                    tail.header.nameLen = 0;
                    tail.header.fileType = kFtDirCsum;
                    memcpy(newDirBlockBuf.get() + blockSize - sizeof(DirEntryTail), &tail, sizeof(tail));
                }
            }
            if (metadataCsum) {
                const uint32_t csum = kExt4ComputeDirBlockChecksum(sb.uuid, newInodeNum, /*generation=*/0,
                                                                     newDirBlockBuf.get(), blockSize);
                memcpy(newDirBlockBuf.get() + blockSize - sizeof(uint32_t), &csum, sizeof(csum));
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitWriteExtBlocks(device, blockSize, newBlockAbs, 1, newDirBlockBuf.get(), &ioResult);
                if (!ioTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 8) 3단계에서 찾아 둔 후보 블록을 다시 읽어 실제 inode
            //    번호로 삽입 + 체크섬 갱신(스크래치 시험과 달리 이번엔
            //    실제로 디스크에 쓴다).
            SlabBuf parentBlockBuf(blockSize);
            if (!parentBlockBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadExtBlocks(device, blockSize, candidateBlockAbs, 1, parentBlockBuf.get(), &ioResult);
                if (!ioTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }
            if (!kExt4InsertDirEntry(parentBlockBuf.get(), blockSize, hasTailBytes, newInodeNum,
                                       args->relPath + leafStart, static_cast<uint8_t>(leafLen), kFtDir)) {
                // 이론상 불가능(3단계에서 스크래치로 이미 확인함) - 방어적 처리.
                args->error = kernel::VfsError::NoSpace;
                break;
            }
            if (metadataCsum) {
                const uint32_t csum = kExt4ComputeDirBlockChecksum(sb.uuid, parentInodeNum, /*generation=*/0,
                                                                     parentBlockBuf.get(), blockSize);
                memcpy(parentBlockBuf.get() + blockSize - sizeof(uint32_t), &csum, sizeof(csum));
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitWriteExtBlocks(device, blockSize, candidateBlockAbs, 1, parentBlockBuf.get(), &ioResult);
                if (!ioTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 9) 부모 inode의 linksCount+1(새 서브디렉터리의 ".."이
            //    부모를 가리키므로) - 2단계에서 읽어 둔 parentInodeBuf
            //    (전체 inodeSize바이트가 유효 범위)를 그대로 갱신 후
            //    다시 쓴다.
            parentInode.linksCount = static_cast<uint16_t>(parentInode.linksCount + 1);
            memcpy(parentInodeBuf.get() + parentInodeByteOffset, &parentInode, sizeof(parentInode));
            if (metadataCsum) {
                const uint32_t crc = kExt4ComputeInodeChecksum(sb.uuid, parentInodeNum, parentInode.generation,
                                                                 parentInodeBuf.get() + parentInodeByteOffset,
                                                                 sb.inodeSize);
                const uint16_t csumLo = static_cast<uint16_t>(crc & 0xFFFFu);
                const uint16_t csumHi = static_cast<uint16_t>(crc >> 16);
                memcpy(parentInodeBuf.get() + parentInodeByteOffset + 124, &csumLo, sizeof(csumLo));
                memcpy(parentInodeBuf.get() + parentInodeByteOffset + 130, &csumHi, sizeof(csumHi));
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitWriteExtBlocks(
                    device, blockSize, parentInodeBlockOffset, parentInodeBlocksNeeded, parentInodeBuf.get(), &ioResult);
                if (!ioTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 10) 슈퍼블록 전역 free 카운터 조정 - 오프셋
            //     kSuperblockOffset(1024바이트)부터 1024바이트 전체를
            //     읽어 제자리 갱신 후 다시 쓴다(슈퍼블록 자신의 블록은
            //     항상 sb.firstDataBlock - gdtStartBlock=firstDataBlock+1
            //     과 같은 관례, mount()의 파싱과 동일).
            {
                const uint64_t sbBlockOffset = sb.firstDataBlock;
                const uint32_t sbByteOffsetInBlock = static_cast<uint32_t>(kSuperblockOffset % blockSize);
                const uint32_t sbBlocksNeeded = static_cast<uint32_t>(
                    kCeilDiv(static_cast<uint64_t>(sbByteOffsetInBlock) + kSuperblockOffset, blockSize));
                SlabBuf sbBuf(sbBlocksNeeded * blockSize);
                if (!sbBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, sbBlockOffset, sbBlocksNeeded, sbBuf.get(), &ioResult);
                    if (!ioTask) {
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                }
                uint8_t* rawSb = sbBuf.get() + sbByteOffsetInBlock;
                if (!kExt4AdjustSuperblockFreeBlocks(rawSb, -1, sb.featureIncompat, sb.featureRoCompat) ||
                    !kExt4AdjustSuperblockFreeInodes(rawSb, -1, sb.featureRoCompat)) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask = kSubmitWriteExtBlocks(device, blockSize, sbBlockOffset,
                                                                       sbBlocksNeeded, sbBuf.get(), &ioResult);
                    if (!ioTask) {
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                }
            }

            args->error = kernel::VfsError::None;
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
