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

// [신규, 2026-09-25, PN-FE718C87 - Rmdir] 디렉터리 데이터 블록
// 하나에 "."/".." 외의 살아있는 엔트리가 있는지 확인(순수, I/O
// 없음) - Rmdir의 "대상이 비어있는지" 판정에 쓴다.
bool kScanDirBlockHasOtherEntries(const uint8_t* block, uint32_t blockSize) {
    uint32_t pos = 0;
    while (pos + sizeof(DirEntry2Header) <= blockSize) {
        const auto* hdr = reinterpret_cast<const DirEntry2Header*>(block + pos);
        if (hdr->recLen == 0 || pos + hdr->recLen > blockSize) {
            break;
        }
        if (hdr->inode != 0) {
            const char* name = reinterpret_cast<const char*>(block + pos + sizeof(DirEntry2Header));
            const bool isDot = (hdr->nameLen == 1 && name[0] == '.');
            const bool isDotDot = (hdr->nameLen == 2 && name[0] == '.' && name[1] == '.');
            if (!isDot && !isDotDot) {
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
            // [구현, 2026-09-25, PN-FE718C87] libext4 최초의 파일 내용
            // 쓰기 - handle(=inode 번호, 이미 Open된 대상)이 가리키는
            // 파일에 [offset, offset+len) 구간을 쓴다. v1 범위는
            // Mkdir/Rmdir/Unlink와 동일한 경계 - 익스텐트 기반
            // "인라인 리프"(depth==0, ≤4개 익스텐트)만 지원, 그 이상
            // (진짜 트리 분할, SP-7A9CED3E §5 미결) 필요해지면
            // PermissionDenied. 새 파일 생성(inode 자체를 새로 만드는
            // 것)은 이 op의 범위 밖 - handle은 항상 이미 존재하는
            // inode를 가리킨다(Open이 새로 만들지 않으므로 이 커널엔
            // 아직 "파일 생성" 자체가 없다 - 별도 과제).
            auto* args = static_cast<kernel::KernelFsWriteArgs*>(argsRaw);
            if (readOnly_) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            if (args->len == 0) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::None;
                break;
            }
            const uint32_t inodeNum = static_cast<uint32_t>(args->handle.value);

            // 1) inode 레코드 전체(inodeSize바이트)를 읽는다 - 끝에
            //    다시 쓸 것이므로 132바이트 core가 아니라 전체가
            //    필요하다(Mkdir의 새 inode 기록과 동일한 이유).
            uint64_t inodeBlockOffset = 0;
            uint32_t inodeByteOffset = 0;
            uint32_t inodeBlocksNeededProbe = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, inodeNum, &inodeBlockOffset, &inodeByteOffset,
                               &inodeBlocksNeededProbe)) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            const uint32_t inodeBlocksNeeded = static_cast<uint32_t>(
                kCeilDiv(static_cast<uint64_t>(inodeByteOffset) + sb.inodeSize, blockSize));
            SlabBuf inodeBlockBuf(inodeBlocksNeeded * blockSize);
            if (!inodeBlockBuf) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, inodeBlockOffset,
                                                                  inodeBlocksNeeded, inodeBlockBuf.get(), &ioResult);
                if (!ioTask) {
                    args->bytesWritten = 0;
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->bytesWritten = 0;
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
            }
            InodeCore inode;
            memcpy(&inode, inodeBlockBuf.get() + inodeByteOffset, sizeof(inode));
            if (kIsDirMode(inode.mode)) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            if (!(inode.flags & kExtentsFl)) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::PermissionDenied;  // 레거시 간접 블록 미지원
                break;
            }
            {
                ExtentHeader header;
                memcpy(&header, inode.block, sizeof(header));
                if (header.magic != kExtentMagic || header.depth != 0) {
                    args->bytesWritten = 0;
                    args->error = kernel::VfsError::PermissionDenied;  // 진짜 익스텐트 트리 미지원
                    break;
                }
            }

            const uint64_t existingSize = inode.sizeLo | (static_cast<uint64_t>(inode.sizeHigh) << 32);
            const uint64_t writeEnd = args->offset + args->len;
            const uint32_t blocksNeededTotal = static_cast<uint32_t>(kCeilDiv(writeEnd, blockSize));
            const bool metadataCsum = (sb.featureRoCompat & kRoCompatMetadataCsum) != 0;
            const bool is64Bit = (sb.featureIncompat & kIncompat64Bit) != 0;
            const uint32_t descSize = is64Bit ? sizeof(GroupDesc64) : sizeof(GroupDesc32);
            const uint64_t gdtStartBlock = static_cast<uint64_t>(sb.firstDataBlock) + 1;
            const uint32_t anchorGroup = (inodeNum - 1) / sb.inodesPerGroup;

            // 2) 쓰기 범위를 담기 모자라면 블록을 하나씩 할당해
            //    익스텐트를 늘린다. depth==0(인라인)이면 inode.block
            //    자체에 늘리고, 4개가 꽉 차면 실제로 트리를 depth==1로
            //    승격(`kExt4GrowExtentTreeToDepth1` - 리눅스 커널
            //    `ext4_ext_grow_indepth()`와 동일 알고리즘, PN-81C6322C)
            //    한 뒤 새로 생긴 리프 블록에 이어서 추가한다. depth==1
            //    상태에서는 그 리프 블록(디스크에서 매번 다시 읽음)에
            //    직접 추가하고, 그 리프마저 꽉 차면(1024바이트 블록
            //    기준 84개 엔트리 - 이 프로젝트의 현실적 파일 크기에서
            //    사실상 도달하지 않음, 더 깊은 분할/형제 리프는
            //    PN-81C6322C 범위 밖) PermissionDenied로 정직하게
            //    거부한다.
            uint32_t blocksAllocatedCount = 0;
            bool allocFailed = false;
            kernel::VfsError allocFailReason = kernel::VfsError::NoSpace;
            while (!allocFailed) {
                ExtentHeader curHeader;
                memcpy(&curHeader, inode.block, sizeof(curHeader));

                if (curHeader.depth == 0) {
                    Extent lastEntry{};
                    bool hasEntry = curHeader.entries > 0;
                    uint32_t curLogicalBlocks = 0;
                    if (hasEntry) {
                        memcpy(&lastEntry,
                               inode.block + sizeof(ExtentHeader) + (curHeader.entries - 1) * sizeof(Extent),
                               sizeof(lastEntry));
                        curLogicalBlocks = lastEntry.block + lastEntry.len;
                    }
                    if (curLogicalBlocks >= blocksNeededTotal) {
                        break;  // 이미 충분함
                    }

                    // 데이터 블록 하나 할당.
                    uint64_t dataBlockAbs = 0;
                    {
                        SlabBuf blockBitmapBuf(blockSize);
                        SlabBuf blockGdBuf(blockSize);
                        if (!blockBitmapBuf || !blockGdBuf) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        bool ioFailed = false;
                        bool allocated = false;
                        for (uint32_t offset = 0; offset < groupCount; ++offset) {
                            const uint32_t group = (anchorGroup + offset) % groupCount;
                            const uint64_t bitmapBlock = volume_.groupBlockBitmapBlock(group);
                            uint64_t gdBlockOffset = 0;
                            uint32_t gdByteOffset = 0;
                            kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset,
                                              &gdByteOffset);

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
                            if (!kExt4AllocateBlockInGroup(sb.uuid, group, sb.blocksPerGroup, sb.featureRoCompat,
                                                            is64Bit, blockBitmapBuf.get(),
                                                            blockGdBuf.get() + gdByteOffset, &relIndex)) {
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
                            dataBlockAbs = static_cast<uint64_t>(sb.firstDataBlock) +
                                          static_cast<uint64_t>(group) * sb.blocksPerGroup + relIndex;
                            allocated = true;
                            break;
                        }
                        if (ioFailed) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        if (!allocated) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::NoSpace;
                            break;
                        }
                    }

                    const uint64_t lastPhysStart =
                        (static_cast<uint64_t>(lastEntry.startHi) << 32) | lastEntry.startLo;
                    const bool canExtend = hasEntry && (lastPhysStart + lastEntry.len == dataBlockAbs) &&
                                            (lastEntry.len < 0x7FFFu);
                    if (canExtend) {
                        lastEntry.len = static_cast<uint16_t>(lastEntry.len + 1);
                        memcpy(inode.block + sizeof(ExtentHeader) + (curHeader.entries - 1) * sizeof(Extent),
                               &lastEntry, sizeof(lastEntry));
                        ++blocksAllocatedCount;
                        continue;
                    }
                    if (kExt4AppendInlineExtent(inode.block, curLogicalBlocks, dataBlockAbs, 1)) {
                        ++blocksAllocatedCount;
                        continue;
                    }

                    // 인라인 리프(4개)가 꽉 참 - depth 1로 승격한다.
                    // 트리 구조용 블록을 하나 더 할당(위와 동일한
                    // 그룹 스캔 - 의도적 중복, 파일 상단 문서 주석의
                    // 합성 불가 제약 때문).
                    uint64_t growBlockAbs = 0;
                    {
                        SlabBuf growBitmapBuf(blockSize);
                        SlabBuf growGdBuf(blockSize);
                        if (!growBitmapBuf || !growGdBuf) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        bool ioFailed = false;
                        bool allocated = false;
                        for (uint32_t offset = 0; offset < groupCount; ++offset) {
                            const uint32_t group = (anchorGroup + offset) % groupCount;
                            const uint64_t bitmapBlock = volume_.groupBlockBitmapBlock(group);
                            uint64_t gdBlockOffset = 0;
                            uint32_t gdByteOffset = 0;
                            kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset,
                                              &gdByteOffset);

                            fs::BlockIoResult bmIo;
                            kernel::AsyncTask* bmTask =
                                kSubmitReadExtBlocks(device, blockSize, bitmapBlock, 1, growBitmapBuf.get(), &bmIo);
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
                                kSubmitReadExtBlocks(device, blockSize, gdBlockOffset, 1, growGdBuf.get(), &gdIo);
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
                            if (!kExt4AllocateBlockInGroup(sb.uuid, group, sb.blocksPerGroup, sb.featureRoCompat,
                                                            is64Bit, growBitmapBuf.get(),
                                                            growGdBuf.get() + gdByteOffset, &relIndex)) {
                                continue;
                            }
                            fs::BlockIoResult wBmIo;
                            kernel::AsyncTask* wBmTask =
                                kSubmitWriteExtBlocks(device, blockSize, bitmapBlock, 1, growBitmapBuf.get(), &wBmIo);
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
                                kSubmitWriteExtBlocks(device, blockSize, gdBlockOffset, 1, growGdBuf.get(), &wGdIo);
                            if (!wGdTask) {
                                ioFailed = true;
                                break;
                            }
                            co_await kernel::AsyncTaskCoroAwaiter(wGdTask);
                            if (!wGdIo.ok) {
                                ioFailed = true;
                                break;
                            }
                            growBlockAbs = static_cast<uint64_t>(sb.firstDataBlock) +
                                          static_cast<uint64_t>(group) * sb.blocksPerGroup + relIndex;
                            allocated = true;
                            break;
                        }
                        if (ioFailed) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        if (!allocated) {
                            // 트리 성장용 블록을 못 구함 - 이미 할당해
                            // 둔 dataBlockAbs를 되돌린다(orphan 방지).
                            const uint32_t rbGroup = static_cast<uint32_t>(
                                (dataBlockAbs - sb.firstDataBlock) / sb.blocksPerGroup);
                            const uint32_t rbRelIndex = static_cast<uint32_t>(
                                (dataBlockAbs - sb.firstDataBlock) % sb.blocksPerGroup);
                            const uint64_t rbBitmapBlock = volume_.groupBlockBitmapBlock(rbGroup);
                            uint64_t rbGdBlockOffset = 0;
                            uint32_t rbGdByteOffset = 0;
                            kLocateGroupDesc(gdtStartBlock, descSize, blockSize, rbGroup, &rbGdBlockOffset,
                                              &rbGdByteOffset);
                            SlabBuf rbBitmapBuf(blockSize);
                            SlabBuf rbGdBuf(blockSize);
                            if (rbBitmapBuf && rbGdBuf) {
                                fs::BlockIoResult rbBmIo;
                                kernel::AsyncTask* rbBmTask = kSubmitReadExtBlocks(
                                    device, blockSize, rbBitmapBlock, 1, rbBitmapBuf.get(), &rbBmIo);
                                if (rbBmTask) {
                                    co_await kernel::AsyncTaskCoroAwaiter(rbBmTask);
                                }
                                fs::BlockIoResult rbGdIo;
                                kernel::AsyncTask* rbGdTask = kSubmitReadExtBlocks(
                                    device, blockSize, rbGdBlockOffset, 1, rbGdBuf.get(), &rbGdIo);
                                if (rbGdTask) {
                                    co_await kernel::AsyncTaskCoroAwaiter(rbGdTask);
                                }
                                kExt4FreeBlockInGroup(sb.uuid, rbGroup, sb.blocksPerGroup, sb.featureRoCompat,
                                                       is64Bit, rbBitmapBuf.get(), rbGdBuf.get() + rbGdByteOffset,
                                                       rbRelIndex);
                                fs::BlockIoResult rwBmIo;
                                kernel::AsyncTask* rwBmTask = kSubmitWriteExtBlocks(
                                    device, blockSize, rbBitmapBlock, 1, rbBitmapBuf.get(), &rwBmIo);
                                if (rwBmTask) {
                                    co_await kernel::AsyncTaskCoroAwaiter(rwBmTask);
                                }
                                fs::BlockIoResult rwGdIo;
                                kernel::AsyncTask* rwGdTask = kSubmitWriteExtBlocks(
                                    device, blockSize, rbGdBlockOffset, 1, rbGdBuf.get(), &rwGdIo);
                                if (rwGdTask) {
                                    co_await kernel::AsyncTaskCoroAwaiter(rwGdTask);
                                }
                            }
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::NoSpace;
                            break;
                        }
                    }
                    if (allocFailed) {
                        break;
                    }

                    SlabBuf growLeafBuf(blockSize);
                    if (!growLeafBuf ||
                        !kExt4GrowExtentTreeToDepth1(inode.block, growLeafBuf.get(), blockSize, growBlockAbs)) {
                        // 이론상 불가능(depth==0임을 이미 확인) - 순수
                        // 방어 경로라 orphan 롤백은 생략한다.
                        allocFailed = true;
                        allocFailReason = kernel::VfsError::PermissionDenied;
                        break;
                    }
                    // 이제 inode.block은 depth==1(인덱스 1개, growBlockAbs
                    // 를 가리킴), growLeafBuf는 옛 인라인 4개 엔트리를
                    // 그대로 물려받은 리프(max가 블록 용량으로 갱신됨).
                    // 아까 할당해 둔 dataBlockAbs를 이 새 리프에 마저
                    // 추가한다(84 vs 5이므로 항상 성공 보장).
                    {
                        ExtentHeader leafHeaderNow;
                        memcpy(&leafHeaderNow, growLeafBuf.get(), sizeof(leafHeaderNow));
                        Extent leafLast{};
                        memcpy(&leafLast,
                               growLeafBuf.get() + sizeof(ExtentHeader) + (leafHeaderNow.entries - 1) * sizeof(Extent),
                               sizeof(leafLast));
                        const uint64_t leafLastPhysStart =
                            (static_cast<uint64_t>(leafLast.startHi) << 32) | leafLast.startLo;
                        const uint32_t leafLastLogicalEnd = leafLast.block + leafLast.len;
                        if (leafLastPhysStart + leafLast.len == dataBlockAbs && leafLast.len < 0x7FFFu) {
                            leafLast.len = static_cast<uint16_t>(leafLast.len + 1);
                            memcpy(growLeafBuf.get() + sizeof(ExtentHeader) +
                                       (leafHeaderNow.entries - 1) * sizeof(Extent),
                                   &leafLast, sizeof(leafLast));
                        } else {
                            kExt4AppendInlineExtent(growLeafBuf.get(), leafLastLogicalEnd, dataBlockAbs, 1);
                        }
                    }
                    if (metadataCsum) {
                        const uint32_t csum = kExt4ComputeExtentBlockChecksum(sb.uuid, inodeNum, inode.generation,
                                                                                growLeafBuf.get(), blockSize);
                        const uint32_t tailOffset = static_cast<uint32_t>(sizeof(ExtentHeader)) +
                                                     kExt4ExtentBlockMaxEntries(blockSize) * sizeof(Extent);
                        memcpy(growLeafBuf.get() + tailOffset, &csum, sizeof(csum));
                    }
                    {
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask =
                            kSubmitWriteExtBlocks(device, blockSize, growBlockAbs, 1, growLeafBuf.get(), &ioResult);
                        if (!ioTask) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                    }
                    blocksAllocatedCount += 2;  // growBlockAbs(메타데이터) + dataBlockAbs(데이터)
                    continue;
                }

                if (curHeader.depth == 1 && curHeader.entries == 1) {
                    ExtentIdx idx;
                    memcpy(&idx, inode.block + sizeof(ExtentHeader), sizeof(idx));
                    const uint64_t leafBlockAbs = (static_cast<uint64_t>(idx.leafHi) << 32) | idx.leafLo;

                    SlabBuf leafBuf(blockSize);
                    if (!leafBuf) {
                        allocFailed = true;
                        allocFailReason = kernel::VfsError::InvalidArgument;
                        break;
                    }
                    {
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask =
                            kSubmitReadExtBlocks(device, blockSize, leafBlockAbs, 1, leafBuf.get(), &ioResult);
                        if (!ioTask) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                    }
                    ExtentHeader leafHeader;
                    memcpy(&leafHeader, leafBuf.get(), sizeof(leafHeader));
                    Extent leafLast{};
                    bool leafHasEntry = leafHeader.entries > 0;
                    uint32_t curLogicalBlocks = 0;
                    if (leafHasEntry) {
                        memcpy(&leafLast,
                               leafBuf.get() + sizeof(ExtentHeader) + (leafHeader.entries - 1) * sizeof(Extent),
                               sizeof(leafLast));
                        curLogicalBlocks = leafLast.block + leafLast.len;
                    }
                    if (curLogicalBlocks >= blocksNeededTotal) {
                        break;  // 이미 충분함
                    }

                    uint64_t dataBlockAbs = 0;
                    {
                        SlabBuf blockBitmapBuf(blockSize);
                        SlabBuf blockGdBuf(blockSize);
                        if (!blockBitmapBuf || !blockGdBuf) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        bool ioFailed = false;
                        bool allocated = false;
                        for (uint32_t offset = 0; offset < groupCount; ++offset) {
                            const uint32_t group = (anchorGroup + offset) % groupCount;
                            const uint64_t bitmapBlock = volume_.groupBlockBitmapBlock(group);
                            uint64_t gdBlockOffset = 0;
                            uint32_t gdByteOffset = 0;
                            kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset,
                                              &gdByteOffset);

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
                            if (!kExt4AllocateBlockInGroup(sb.uuid, group, sb.blocksPerGroup, sb.featureRoCompat,
                                                            is64Bit, blockBitmapBuf.get(),
                                                            blockGdBuf.get() + gdByteOffset, &relIndex)) {
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
                            dataBlockAbs = static_cast<uint64_t>(sb.firstDataBlock) +
                                          static_cast<uint64_t>(group) * sb.blocksPerGroup + relIndex;
                            allocated = true;
                            break;
                        }
                        if (ioFailed) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        if (!allocated) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::NoSpace;
                            break;
                        }
                    }

                    const uint64_t leafLastPhysStart =
                        (static_cast<uint64_t>(leafLast.startHi) << 32) | leafLast.startLo;
                    const bool leafCanExtend = leafHasEntry && (leafLastPhysStart + leafLast.len == dataBlockAbs) &&
                                                (leafLast.len < 0x7FFFu);
                    bool appended = false;
                    if (leafCanExtend) {
                        leafLast.len = static_cast<uint16_t>(leafLast.len + 1);
                        memcpy(leafBuf.get() + sizeof(ExtentHeader) + (leafHeader.entries - 1) * sizeof(Extent),
                               &leafLast, sizeof(leafLast));
                        appended = true;
                    } else if (kExt4AppendInlineExtent(leafBuf.get(), curLogicalBlocks, dataBlockAbs, 1)) {
                        appended = true;
                    }
                    if (!appended) {
                        // 리프마저 꽉 참(1024바이트 블록 기준 84개) -
                        // 더 깊은 분할/형제 리프는 PN-81C6322C 범위
                        // 밖으로 정직하게 거부, 방금 할당한 블록은
                        // 되돌린다(orphan 방지).
                        const uint32_t rbGroup = static_cast<uint32_t>(
                            (dataBlockAbs - sb.firstDataBlock) / sb.blocksPerGroup);
                        const uint32_t rbRelIndex = static_cast<uint32_t>(
                            (dataBlockAbs - sb.firstDataBlock) % sb.blocksPerGroup);
                        const uint64_t rbBitmapBlock = volume_.groupBlockBitmapBlock(rbGroup);
                        uint64_t rbGdBlockOffset = 0;
                        uint32_t rbGdByteOffset = 0;
                        kLocateGroupDesc(gdtStartBlock, descSize, blockSize, rbGroup, &rbGdBlockOffset,
                                          &rbGdByteOffset);
                        SlabBuf rbBitmapBuf(blockSize);
                        SlabBuf rbGdBuf(blockSize);
                        if (rbBitmapBuf && rbGdBuf) {
                            fs::BlockIoResult rbBmIo;
                            kernel::AsyncTask* rbBmTask =
                                kSubmitReadExtBlocks(device, blockSize, rbBitmapBlock, 1, rbBitmapBuf.get(), &rbBmIo);
                            if (rbBmTask) {
                                co_await kernel::AsyncTaskCoroAwaiter(rbBmTask);
                            }
                            fs::BlockIoResult rbGdIo;
                            kernel::AsyncTask* rbGdTask =
                                kSubmitReadExtBlocks(device, blockSize, rbGdBlockOffset, 1, rbGdBuf.get(), &rbGdIo);
                            if (rbGdTask) {
                                co_await kernel::AsyncTaskCoroAwaiter(rbGdTask);
                            }
                            kExt4FreeBlockInGroup(sb.uuid, rbGroup, sb.blocksPerGroup, sb.featureRoCompat, is64Bit,
                                                   rbBitmapBuf.get(), rbGdBuf.get() + rbGdByteOffset, rbRelIndex);
                            fs::BlockIoResult rwBmIo;
                            kernel::AsyncTask* rwBmTask =
                                kSubmitWriteExtBlocks(device, blockSize, rbBitmapBlock, 1, rbBitmapBuf.get(), &rwBmIo);
                            if (rwBmTask) {
                                co_await kernel::AsyncTaskCoroAwaiter(rwBmTask);
                            }
                            fs::BlockIoResult rwGdIo;
                            kernel::AsyncTask* rwGdTask =
                                kSubmitWriteExtBlocks(device, blockSize, rbGdBlockOffset, 1, rbGdBuf.get(), &rwGdIo);
                            if (rwGdTask) {
                                co_await kernel::AsyncTaskCoroAwaiter(rwGdTask);
                            }
                        }
                        allocFailed = true;
                        allocFailReason = kernel::VfsError::PermissionDenied;
                        break;
                    }
                    if (metadataCsum) {
                        const uint32_t csum = kExt4ComputeExtentBlockChecksum(sb.uuid, inodeNum, inode.generation,
                                                                                leafBuf.get(), blockSize);
                        const uint32_t tailOffset =
                            static_cast<uint32_t>(sizeof(ExtentHeader)) + leafHeader.max * sizeof(Extent);
                        memcpy(leafBuf.get() + tailOffset, &csum, sizeof(csum));
                    }
                    {
                        fs::BlockIoResult ioResult;
                        kernel::AsyncTask* ioTask =
                            kSubmitWriteExtBlocks(device, blockSize, leafBlockAbs, 1, leafBuf.get(), &ioResult);
                        if (!ioTask) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                        co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                        if (!ioResult.ok) {
                            allocFailed = true;
                            allocFailReason = kernel::VfsError::InvalidArgument;
                            break;
                        }
                    }
                    ++blocksAllocatedCount;
                    continue;
                }

                // depth>1이거나 인덱스 엔트리가 1개가 아님 - 이
                // 드라이버가 한 번도 만든 적 없는 모양(범위 밖).
                allocFailed = true;
                allocFailReason = kernel::VfsError::PermissionDenied;
                break;
            }
            if (allocFailed) {
                args->bytesWritten = 0;
                args->error = allocFailReason;
                break;
            }

            // 3) [offset, offset+len) 구간을 청크 단위로
            //    read-modify-write한다(Read 케이스와 동일한 청크
            //    순회 - 경계 블록은 부분 겹침이라 항상 읽어서 합친다).
            SlabBuf chunkBuf(blockSize);
            if (!chunkBuf) {
                args->bytesWritten = 0;
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            uint32_t totalWritten = 0;
            bool writeIoFailed = false;
            const auto* src = static_cast<const uint8_t*>(args->buf);
            while (totalWritten < args->len && !writeIoFailed) {
                const uint64_t curOffset = args->offset + totalWritten;
                const uint32_t logicalBlock = static_cast<uint32_t>(curOffset / blockSize);
                const uint32_t offsetInBlock = static_cast<uint32_t>(curOffset % blockSize);
                const uint32_t remaining = args->len - totalWritten;
                const uint32_t chunk =
                    remaining < (blockSize - offsetInBlock) ? remaining : (blockSize - offsetInBlock);

                uint64_t nodeValue = 0;
                ExtentLookup lookup = kLookupExtent(inode.block, logicalBlock, &nodeValue);
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
                    // 2단계에서 이미 충분히 확장했으므로 이론상 불가능.
                    writeIoFailed = true;
                    break;
                }

                fs::BlockIoResult readResult;
                kernel::AsyncTask* readTask =
                    kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, chunkBuf.get(), &readResult);
                if (!readTask) {
                    writeIoFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(readTask);
                if (!readResult.ok) {
                    writeIoFailed = true;
                    break;
                }
                memcpy(chunkBuf.get() + offsetInBlock, src + totalWritten, chunk);
                fs::BlockIoResult writeResult;
                kernel::AsyncTask* writeTask =
                    kSubmitWriteExtBlocks(device, blockSize, nodeValue, 1, chunkBuf.get(), &writeResult);
                if (!writeTask) {
                    writeIoFailed = true;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(writeTask);
                if (!writeResult.ok) {
                    writeIoFailed = true;
                    break;
                }
                totalWritten += chunk;
            }
            if (writeIoFailed) {
                args->bytesWritten = totalWritten;
                args->error = kernel::VfsError::InvalidHandle;
                break;
            }

            // 4) 파일 크기가 늘었으면 반영 + mtime 갱신 + inode 레코드
            //    다시 씀(체크섬 포함).
            // [실측(e2fsck)으로 발견된 갭, PN-81C6322C] i_blocks(blocksLo)
            // 는 "논리 데이터 블록 수"가 아니라 "이 inode가 실제로
            // 점유한 물리 블록 총합"이다 - 트리가 depth 1로 성장하면
            // 그 리프/인덱스 메타데이터 블록 자체도 여기 포함돼야
            // 한다(리눅스 커널과 동일). `blocksNeededTotal`(파일 크기
            // 기준 논리 블록 수)로 다시 계산하면 이 메타데이터 블록을
            // 빼먹는다 - `blocksAllocatedCount`(2단계에서 실제로 새로
            // 소비한 블록 수, 데이터+트리 성장 블록 전부 포함)를 기존
            // 값에 더하는 것이 정확하다.
            if (writeEnd > existingSize) {
                inode.sizeLo = static_cast<uint32_t>(writeEnd & 0xFFFFFFFFu);
                inode.sizeHigh = static_cast<uint32_t>(writeEnd >> 32);
                inode.blocksLo = inode.blocksLo + blocksAllocatedCount * (blockSize / 512);
            }
            inode.mtime = kernel::Rtc::toEpochSeconds(kernel::Rtc::readWallClock());
            memcpy(inodeBlockBuf.get() + inodeByteOffset, &inode, sizeof(inode));
            if (metadataCsum) {
                const uint32_t crc = kExt4ComputeInodeChecksum(sb.uuid, inodeNum, inode.generation,
                                                                 inodeBlockBuf.get() + inodeByteOffset, sb.inodeSize);
                const uint16_t csumLo = static_cast<uint16_t>(crc & 0xFFFFu);
                const uint16_t csumHi = static_cast<uint16_t>(crc >> 16);
                memcpy(inodeBlockBuf.get() + inodeByteOffset + 124, &csumLo, sizeof(csumLo));
                memcpy(inodeBlockBuf.get() + inodeByteOffset + 130, &csumHi, sizeof(csumHi));
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitWriteExtBlocks(device, blockSize, inodeBlockOffset,
                                                                   inodeBlocksNeeded, inodeBlockBuf.get(), &ioResult);
                if (!ioTask) {
                    args->bytesWritten = totalWritten;
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                if (!ioResult.ok) {
                    args->bytesWritten = totalWritten;
                    args->error = kernel::VfsError::InvalidHandle;
                    break;
                }
            }

            // 5) 새로 할당한 블록만큼 슈퍼블록 전역 free 카운터 차감.
            if (blocksAllocatedCount > 0) {
                const uint64_t sbBlockOffset = sb.firstDataBlock;
                const uint32_t sbByteOffsetInBlock = static_cast<uint32_t>(kSuperblockOffset % blockSize);
                const uint32_t sbBlocksNeeded = static_cast<uint32_t>(
                    kCeilDiv(static_cast<uint64_t>(sbByteOffsetInBlock) + kSuperblockOffset, blockSize));
                SlabBuf sbBuf(sbBlocksNeeded * blockSize);
                if (!sbBuf) {
                    args->bytesWritten = totalWritten;
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, sbBlockOffset, sbBlocksNeeded, sbBuf.get(), &ioResult);
                    if (!ioTask) {
                        args->bytesWritten = totalWritten;
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        args->bytesWritten = totalWritten;
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                }
                uint8_t* rawSb = sbBuf.get() + sbByteOffsetInBlock;
                if (!kExt4AdjustSuperblockFreeBlocks(rawSb, -static_cast<kernel::int64_t>(blocksAllocatedCount),
                                                       sb.featureIncompat, sb.featureRoCompat)) {
                    args->bytesWritten = totalWritten;
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask = kSubmitWriteExtBlocks(device, blockSize, sbBlockOffset,
                                                                       sbBlocksNeeded, sbBuf.get(), &ioResult);
                    if (!ioTask) {
                        args->bytesWritten = totalWritten;
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        args->bytesWritten = totalWritten;
                        args->error = kernel::VfsError::InvalidArgument;
                        break;
                    }
                }
            }

            args->bytesWritten = totalWritten;
            args->error = kernel::VfsError::None;
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
            // [구현, 2026-09-25, PN-FE718C87] Mkdir의 정확한 역연산 -
            // (a) 부모 탐색(Open/Mkdir과 동일한 세그먼트 순회, 의도적
            // 중복) → (b) 부모 블록에서 leaf 검색(디렉터리인지 확인) →
            // (c) 대상이 비어있는지 확인("."/".." 외 엔트리 없음) →
            // (d) 대상의 데이터 블록들 free → (e) 대상 inode free +
            // bg_used_dirs_count -1 → (f) 부모 블록에서 엔트리 제거 →
            // (g) 부모 linksCount -1(대상의 ".."이 더 이상 부모를
            // 안 가리키므로) → (h) 슈퍼블록 free 카운터 +되돌림.
            // v1은 Mkdir과 마찬가지로 익스텐트 기반 디렉터리만 지원
            // (부모/대상 둘 다 레거시 간접 블록이면 거부).
            auto* args = static_cast<kernel::KernelFsRmdirArgs*>(argsRaw);
            if (readOnly_) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            uint32_t parentLen = 0, leafStart = 0, leafLen = 0;
            if (!kSplitParentAndLeaf(args->relPath, args->relPathLen, &parentLen, &leafStart, &leafLen)) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }

            // 1) 부모 디렉터리 inode 번호를 찾는다 - Mkdir/Open과
            //    동일한 패턴(합성 불가로 함수 공유 불가, 의도적 중복).
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

            // 2) 부모 자신의 inode 구조체를 읽는다(inodeSize 전체 -
            //    나중에 linksCount 갱신+체크섬 재계산에 필요).
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
            if (!kIsDirMode(parentInode.mode) || !(parentInode.flags & kExtentsFl)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }

            const uint64_t parentDirSize = parentInode.sizeLo | (static_cast<uint64_t>(parentInode.sizeHigh) << 32);
            const uint32_t parentDirBlockCount = static_cast<uint32_t>(kCeilDiv(parentDirSize, blockSize));
            const bool metadataCsum = (sb.featureRoCompat & kRoCompatMetadataCsum) != 0;

            // 3) 부모의 데이터 블록들을 순회해 leaf 이름을 찾는다(대상
            //    inode 번호/타입 + 그 엔트리가 든 블록의 절대 번호를
            //    기억해 둔다 - 나중에 엔트리 제거에 재사용).
            uint32_t targetInodeNum = 0;
            uint8_t targetFileType = 0;
            bool leafFound = false;
            uint64_t leafBlockAbs = 0;
            {
                bool ioFailed = false;
                SlabBuf scanBuf(blockSize);
                if (!scanBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                for (uint32_t logicalBlock = 0; logicalBlock < parentDirBlockCount && !leafFound; ++logicalBlock) {
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
                    if (kScanDirBlockForName(scanBuf.get(), blockSize, args->relPath + leafStart, leafLen,
                                              &targetInodeNum, &targetFileType)) {
                        leafFound = true;
                        leafBlockAbs = nodeValue;
                    }
                }
                if (ioFailed) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }
            if (!leafFound) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            if (targetFileType != kFtDir) {
                args->error = kernel::VfsError::InvalidArgument;  // Unlink를 써야 함
                break;
            }

            // 4) 대상 inode를 읽는다(디렉터리 확인 + 데이터 블록
            //    순회에 필요 - 132바이트 core만으로 충분, 이 레코드
            //    자체를 다시 쓰지 않으므로 inodeSize 전체는 불필요).
            uint64_t targetInodeBlockOffset = 0;
            uint32_t targetInodeByteOffset = 0;
            uint32_t targetInodeBlocksNeeded = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, targetInodeNum, &targetInodeBlockOffset,
                               &targetInodeByteOffset, &targetInodeBlocksNeeded)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            SlabBuf targetInodeBuf(targetInodeBlocksNeeded * blockSize);
            if (!targetInodeBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, targetInodeBlockOffset,
                                                                  targetInodeBlocksNeeded, targetInodeBuf.get(), &ioResult);
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
            InodeCore targetInode;
            memcpy(&targetInode, targetInodeBuf.get() + targetInodeByteOffset, sizeof(targetInode));
            if (!kIsDirMode(targetInode.mode) || !(targetInode.flags & kExtentsFl)) {
                // v1은 익스텐트 기반 디렉터리만 지원(레거시 간접 블록
                // 대상은 범위 밖 - Mkdir의 부모 제약과 동일한 관례).
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            const uint64_t targetDirSize = targetInode.sizeLo | (static_cast<uint64_t>(targetInode.sizeHigh) << 32);
            const uint32_t targetDirBlockCount = static_cast<uint32_t>(kCeilDiv(targetDirSize, blockSize));

            // 5) 대상이 비어있는지 확인("."/".." 외 엔트리가 하나라도
            //    있으면 NotEmpty) - 동시에 이후 free에 쓸 물리 블록
            //    번호들을 전부 모아 둔다(최대 4개 - 인라인 익스텐트
            //    한도, kExtentInlineMaxEntries).
            uint64_t targetBlockAbs[kExtentInlineMaxEntries] = {};
            uint32_t targetBlockCount = 0;
            bool targetEmpty = true;
            {
                bool ioFailed = false;
                SlabBuf checkBuf(blockSize);
                if (!checkBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                for (uint32_t logicalBlock = 0; logicalBlock < targetDirBlockCount && targetEmpty; ++logicalBlock) {
                    uint64_t nodeValue = 0;
                    ExtentLookup lookup = kLookupExtent(targetInode.block, logicalBlock, &nodeValue);
                    // 인라인 리프(depth==0)만 지원하는 v1 범위 - Mkdir이
                    // 만드는 디렉터리는 항상 이 형태(NeedChild가 나올 수
                    // 없음, kExtentInlineMaxEntries<=4개 리프 엔트리뿐).
                    if (lookup != ExtentLookup::Found) {
                        continue;
                    }
                    if (targetBlockCount < kExtentInlineMaxEntries) {
                        targetBlockAbs[targetBlockCount++] = nodeValue;
                    }
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask =
                        kSubmitReadExtBlocks(device, blockSize, nodeValue, 1, checkBuf.get(), &ioResult);
                    if (!ioTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(ioTask);
                    if (!ioResult.ok) {
                        ioFailed = true;
                        break;
                    }
                    if (kScanDirBlockHasOtherEntries(checkBuf.get(), blockSize)) {
                        targetEmpty = false;
                    }
                }
                if (ioFailed) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }
            if (!targetEmpty) {
                args->error = kernel::VfsError::NotEmpty;
                break;
            }

            const bool is64Bit = (sb.featureIncompat & kIncompat64Bit) != 0;
            const uint32_t descSize = is64Bit ? sizeof(GroupDesc64) : sizeof(GroupDesc32);
            const uint64_t gdtStartBlock = static_cast<uint64_t>(sb.firstDataBlock) + 1;

            // 6) 대상의 데이터 블록들을 전부 free한다(비트맵+그룹
            //    디스크립터 read-modify-write, 블록마다 그룹이 다를
            //    수 있으므로 매번 새로 읽는다 - 최대 4개뿐이라 부담
            //    없음).
            {
                bool ioFailed = false;
                for (uint32_t i = 0; i < targetBlockCount && !ioFailed; ++i) {
                    const uint64_t abs = targetBlockAbs[i];
                    const uint32_t group =
                        static_cast<uint32_t>((abs - sb.firstDataBlock) / sb.blocksPerGroup);
                    const uint32_t relIndex =
                        static_cast<uint32_t>((abs - sb.firstDataBlock) % sb.blocksPerGroup);
                    const uint64_t bitmapBlock = volume_.groupBlockBitmapBlock(group);
                    uint64_t gdBlockOffset = 0;
                    uint32_t gdByteOffset = 0;
                    kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset, &gdByteOffset);

                    SlabBuf bitmapBuf(blockSize);
                    SlabBuf gdBuf(blockSize);
                    if (!bitmapBuf || !gdBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult bmIo;
                    kernel::AsyncTask* bmTask =
                        kSubmitReadExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &bmIo);
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
                        kSubmitReadExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &gdIo);
                    if (!gdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(gdTask);
                    if (!gdIo.ok) {
                        ioFailed = true;
                        break;
                    }
                    if (!kExt4FreeBlockInGroup(sb.uuid, group, sb.blocksPerGroup, sb.featureRoCompat, is64Bit,
                                                bitmapBuf.get(), gdBuf.get() + gdByteOffset, relIndex)) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult wBmIo;
                    kernel::AsyncTask* wBmTask =
                        kSubmitWriteExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &wBmIo);
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
                        kSubmitWriteExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &wGdIo);
                    if (!wGdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(wGdTask);
                    if (!wGdIo.ok) {
                        ioFailed = true;
                        break;
                    }
                }
                if (ioFailed) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 7) 대상 inode 자체를 free(비트맵+그룹 디스크립터) +
            //    bg_used_dirs_count -1(Mkdir이 +1 했던 것의 정확한
            //    역연산).
            {
                const uint32_t group = (targetInodeNum - 1) / sb.inodesPerGroup;
                const uint32_t relIndex = (targetInodeNum - 1) % sb.inodesPerGroup;
                const uint64_t bitmapBlock = volume_.groupInodeBitmapBlock(group);
                uint64_t gdBlockOffset = 0;
                uint32_t gdByteOffset = 0;
                kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset, &gdByteOffset);

                SlabBuf bitmapBuf(blockSize);
                SlabBuf gdBuf(blockSize);
                if (!bitmapBuf || !gdBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                fs::BlockIoResult bmIo;
                kernel::AsyncTask* bmTask = kSubmitReadExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &bmIo);
                if (!bmTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(bmTask);
                if (!bmIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                fs::BlockIoResult gdIo;
                kernel::AsyncTask* gdTask = kSubmitReadExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &gdIo);
                if (!gdTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(gdTask);
                if (!gdIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                if (!kExt4FreeInodeInGroup(sb.uuid, group, sb.inodesPerGroup, sb.featureRoCompat, is64Bit,
                                            bitmapBuf.get(), gdBuf.get() + gdByteOffset, relIndex)) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                kExt4AdjustGroupDescUsedDirs(sb.uuid, group, sb.featureRoCompat, is64Bit, gdBuf.get() + gdByteOffset,
                                              /*delta=*/-1);
                fs::BlockIoResult wBmIo;
                kernel::AsyncTask* wBmTask = kSubmitWriteExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &wBmIo);
                if (!wBmTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(wBmTask);
                if (!wBmIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                fs::BlockIoResult wGdIo;
                kernel::AsyncTask* wGdTask = kSubmitWriteExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &wGdIo);
                if (!wGdTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(wGdTask);
                if (!wGdIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 7b) [실측(e2fsck)으로 발견된 갭] 대상 inode의 온디스크
            //     레코드 자체를 0으로 지운다 - 비트맵만 free로
            //     표시하고 레코드 내용(mode 등)을 그대로 남겨두면,
            //     e2fsck의 Pass 1(비트맵을 안 믿고 inode 테이블을
            //     직접 훑어 "그럴듯해 보이는" inode를 전부 찾아내는
            //     방식)이 이 inode를 여전히 살아있는 디렉터리로 오인해
            //     "Unconnected directory inode"로 보고한다(부모
            //     linksCount까지 그 유령 참조 때문에 틀렸다고 잘못
            //     판단하는 연쇄까지 발생). Mkdir이 새 inode를 쓸 때
            //     inodeSize 전체를 0으로 지우고 시작하는 것과 대칭되는
            //     "역방향" 정리 - 체크섬은 필요 없다(mode==0인 슬롯은
            //     e2fsck/커널 둘 다 체크섬 검증 대상에서 제외).
            {
                uint64_t freeInodeBlockOffset = 0;
                uint32_t freeInodeByteOffset = 0;
                uint32_t freeInodeBlocksNeededProbe = 0;
                if (!kLocateInode(sb, volume_, groupCount, blockSize, targetInodeNum, &freeInodeBlockOffset,
                                   &freeInodeByteOffset, &freeInodeBlocksNeededProbe)) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                const uint32_t freeInodeBlocksNeeded = static_cast<uint32_t>(
                    kCeilDiv(static_cast<uint64_t>(freeInodeByteOffset) + sb.inodeSize, blockSize));
                SlabBuf freeInodeBlockBuf(freeInodeBlocksNeeded * blockSize);
                if (!freeInodeBlockBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, freeInodeBlockOffset,
                                                                      freeInodeBlocksNeeded, freeInodeBlockBuf.get(),
                                                                      &ioResult);
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
                memset(freeInodeBlockBuf.get() + freeInodeByteOffset, 0, sb.inodeSize);
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask = kSubmitWriteExtBlocks(device, blockSize, freeInodeBlockOffset,
                                                                       freeInodeBlocksNeeded, freeInodeBlockBuf.get(),
                                                                       &ioResult);
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

            // 8) 부모 블록에서 엔트리 제거 + 체크섬 갱신 + 씀.
            SlabBuf parentBlockBuf(blockSize);
            if (!parentBlockBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadExtBlocks(device, blockSize, leafBlockAbs, 1, parentBlockBuf.get(), &ioResult);
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
            uint8_t removedFileType = 0;
            if (!kExt4RemoveDirEntry(parentBlockBuf.get(), blockSize, args->relPath + leafStart,
                                       static_cast<uint8_t>(leafLen), &removedFileType)) {
                args->error = kernel::VfsError::NotFound;  // 방어적 처리(이론상 불가능 - 3단계에서 이미 확인)
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
                    kSubmitWriteExtBlocks(device, blockSize, leafBlockAbs, 1, parentBlockBuf.get(), &ioResult);
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

            // 9) 부모 inode의 linksCount-1(Mkdir의 +1을 정확히
            //    되돌림) + 체크섬 재계산 + 씀.
            parentInode.linksCount = static_cast<uint16_t>(parentInode.linksCount - 1);
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

            // 10) 슈퍼블록 전역 free 카운터 되돌림(+블록개수/+1 inode).
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
                if (!kExt4AdjustSuperblockFreeBlocks(rawSb, static_cast<kernel::int64_t>(targetBlockCount),
                                                       sb.featureIncompat, sb.featureRoCompat) ||
                    !kExt4AdjustSuperblockFreeInodes(rawSb, 1, sb.featureRoCompat)) {
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
        case kernel::KernelFsOpCode::Unlink: {
            // [구현, 2026-09-25, PN-FE718C87] Rmdir과 거의 동일한 구조
            // (부모 탐색 → leaf 검색 → free → 부모 엔트리 제거)지만
            // 다른 점 셋: (1) 대상이 파일이어야 함(디렉터리면 거부,
            // Rmdir을 써야 함), (2) "비어있는지" 확인이 없음, (3) 대상
            // 삭제가 부모의 linksCount에 영향을 주지 않음(".."은
            // 디렉터리만 가지므로 부모 inode를 아예 다시 쓸 필요가
            // 없음) - Rmdir과 합성 불가 제약(파일 상단 문서 주석)으로
            // 공유 못 해 부모 탐색 부분은 그대로 중복.
            auto* args = static_cast<kernel::KernelFsUnlinkArgs*>(argsRaw);
            if (readOnly_) {
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            uint32_t parentLen = 0, leafStart = 0, leafLen = 0;
            if (!kSplitParentAndLeaf(args->relPath, args->relPathLen, &parentLen, &leafStart, &leafLen)) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }

            // 1) 부모 디렉터리 inode 번호를 찾는다 - Mkdir/Rmdir/Open과
            //    동일한 패턴(합성 불가로 함수 공유 불가, 의도적 중복).
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

            // 2) 부모 자신의 inode 구조체를 읽는다(132바이트 core만 -
            //    Unlink는 부모를 다시 쓰지 않으므로 inodeSize 전체는
            //    불필요, Rmdir과 다른 점).
            uint64_t parentInodeBlockOffset = 0;
            uint32_t parentInodeByteOffset = 0;
            uint32_t parentInodeBlocksNeeded = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, parentInodeNum, &parentInodeBlockOffset,
                               &parentInodeByteOffset, &parentInodeBlocksNeeded)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
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
            if (!kIsDirMode(parentInode.mode) || !(parentInode.flags & kExtentsFl)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }

            const uint64_t parentDirSize = parentInode.sizeLo | (static_cast<uint64_t>(parentInode.sizeHigh) << 32);
            const uint32_t parentDirBlockCount = static_cast<uint32_t>(kCeilDiv(parentDirSize, blockSize));
            const bool metadataCsum = (sb.featureRoCompat & kRoCompatMetadataCsum) != 0;

            // 3) 부모의 데이터 블록들을 순회해 leaf 이름을 찾는다.
            uint32_t targetInodeNum = 0;
            uint8_t targetFileType = 0;
            bool leafFound = false;
            uint64_t leafBlockAbs = 0;
            {
                bool ioFailed = false;
                SlabBuf scanBuf(blockSize);
                if (!scanBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                for (uint32_t logicalBlock = 0; logicalBlock < parentDirBlockCount && !leafFound; ++logicalBlock) {
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
                    if (kScanDirBlockForName(scanBuf.get(), blockSize, args->relPath + leafStart, leafLen,
                                              &targetInodeNum, &targetFileType)) {
                        leafFound = true;
                        leafBlockAbs = nodeValue;
                    }
                }
                if (ioFailed) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }
            if (!leafFound) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            if (targetFileType == kFtDir) {
                args->error = kernel::VfsError::InvalidArgument;  // Rmdir을 써야 함
                break;
            }

            // 4) 대상 inode를 읽는다(132바이트 core만 - 레코드를
            //    다시 쓰지 않고 0으로 지울 것이므로 충분).
            uint64_t targetInodeBlockOffset = 0;
            uint32_t targetInodeByteOffset = 0;
            uint32_t targetInodeBlocksNeeded = 0;
            if (!kLocateInode(sb, volume_, groupCount, blockSize, targetInodeNum, &targetInodeBlockOffset,
                               &targetInodeByteOffset, &targetInodeBlocksNeeded)) {
                args->error = kernel::VfsError::NotFound;
                break;
            }
            SlabBuf targetInodeBuf(targetInodeBlocksNeeded * blockSize);
            if (!targetInodeBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, targetInodeBlockOffset,
                                                                  targetInodeBlocksNeeded, targetInodeBuf.get(), &ioResult);
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
            InodeCore targetInode;
            memcpy(&targetInode, targetInodeBuf.get() + targetInodeByteOffset, sizeof(targetInode));
            if (kIsDirMode(targetInode.mode) || !(targetInode.flags & kExtentsFl)) {
                // v1은 익스텐트 기반 "파일"만 지원(디렉터리는 이미
                // 위에서 걸러짐 - 방어적 재확인. 레거시 간접 블록
                // 파일도 범위 밖 - Mkdir/Rmdir과 동일한 관례).
                args->error = kernel::VfsError::PermissionDenied;
                break;
            }
            const uint64_t targetSize = targetInode.sizeLo | (static_cast<uint64_t>(targetInode.sizeHigh) << 32);
            const uint32_t targetBlockCountLogical = static_cast<uint32_t>(kCeilDiv(targetSize, blockSize));

            // 5) 대상의 물리 블록 번호들을 모은다(인라인 익스텐트
            //    리프만 지원 - depth>0(진짜 트리, NeedChild)이 나오면
            //    v1 범위 밖으로 거부한다, Write의 "5개 이상 익스텐트"
            //    미구현과 동일한 경계).
            uint64_t targetBlockAbs[kExtentInlineMaxEntries] = {};
            uint32_t targetBlockCount = 0;
            {
                bool unsupported = false;
                for (uint32_t logicalBlock = 0; logicalBlock < targetBlockCountLogical && !unsupported;
                     ++logicalBlock) {
                    uint64_t nodeValue = 0;
                    const ExtentLookup lookup = kLookupExtent(targetInode.block, logicalBlock, &nodeValue);
                    if (lookup == ExtentLookup::NeedChild || lookup == ExtentLookup::Invalid) {
                        unsupported = true;
                        break;
                    }
                    if (lookup != ExtentLookup::Found) {
                        continue;
                    }
                    if (targetBlockCount >= kExtentInlineMaxEntries) {
                        unsupported = true;
                        break;
                    }
                    targetBlockAbs[targetBlockCount++] = nodeValue;
                }
                if (unsupported) {
                    args->error = kernel::VfsError::PermissionDenied;
                    break;
                }
            }

            const bool is64Bit = (sb.featureIncompat & kIncompat64Bit) != 0;
            const uint32_t descSize = is64Bit ? sizeof(GroupDesc64) : sizeof(GroupDesc32);
            const uint64_t gdtStartBlock = static_cast<uint64_t>(sb.firstDataBlock) + 1;

            // 6) 대상의 데이터 블록들을 전부 free한다.
            {
                bool ioFailed = false;
                for (uint32_t i = 0; i < targetBlockCount && !ioFailed; ++i) {
                    const uint64_t abs = targetBlockAbs[i];
                    const uint32_t group = static_cast<uint32_t>((abs - sb.firstDataBlock) / sb.blocksPerGroup);
                    const uint32_t relIndex = static_cast<uint32_t>((abs - sb.firstDataBlock) % sb.blocksPerGroup);
                    const uint64_t bitmapBlock = volume_.groupBlockBitmapBlock(group);
                    uint64_t gdBlockOffset = 0;
                    uint32_t gdByteOffset = 0;
                    kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset, &gdByteOffset);

                    SlabBuf bitmapBuf(blockSize);
                    SlabBuf gdBuf(blockSize);
                    if (!bitmapBuf || !gdBuf) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult bmIo;
                    kernel::AsyncTask* bmTask =
                        kSubmitReadExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &bmIo);
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
                        kSubmitReadExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &gdIo);
                    if (!gdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(gdTask);
                    if (!gdIo.ok) {
                        ioFailed = true;
                        break;
                    }
                    if (!kExt4FreeBlockInGroup(sb.uuid, group, sb.blocksPerGroup, sb.featureRoCompat, is64Bit,
                                                bitmapBuf.get(), gdBuf.get() + gdByteOffset, relIndex)) {
                        ioFailed = true;
                        break;
                    }
                    fs::BlockIoResult wBmIo;
                    kernel::AsyncTask* wBmTask =
                        kSubmitWriteExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &wBmIo);
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
                        kSubmitWriteExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &wGdIo);
                    if (!wGdTask) {
                        ioFailed = true;
                        break;
                    }
                    co_await kernel::AsyncTaskCoroAwaiter(wGdTask);
                    if (!wGdIo.ok) {
                        ioFailed = true;
                        break;
                    }
                }
                if (ioFailed) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 7) 대상 inode 자체를 free(비트맵+그룹 디스크립터) -
            //    bg_used_dirs_count는 파일이라 건드리지 않는다(Rmdir과
            //    다른 점).
            {
                const uint32_t group = (targetInodeNum - 1) / sb.inodesPerGroup;
                const uint32_t relIndex = (targetInodeNum - 1) % sb.inodesPerGroup;
                const uint64_t bitmapBlock = volume_.groupInodeBitmapBlock(group);
                uint64_t gdBlockOffset = 0;
                uint32_t gdByteOffset = 0;
                kLocateGroupDesc(gdtStartBlock, descSize, blockSize, group, &gdBlockOffset, &gdByteOffset);

                SlabBuf bitmapBuf(blockSize);
                SlabBuf gdBuf(blockSize);
                if (!bitmapBuf || !gdBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                fs::BlockIoResult bmIo;
                kernel::AsyncTask* bmTask = kSubmitReadExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &bmIo);
                if (!bmTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(bmTask);
                if (!bmIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                fs::BlockIoResult gdIo;
                kernel::AsyncTask* gdTask = kSubmitReadExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &gdIo);
                if (!gdTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(gdTask);
                if (!gdIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                if (!kExt4FreeInodeInGroup(sb.uuid, group, sb.inodesPerGroup, sb.featureRoCompat, is64Bit,
                                            bitmapBuf.get(), gdBuf.get() + gdByteOffset, relIndex)) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                fs::BlockIoResult wBmIo;
                kernel::AsyncTask* wBmTask = kSubmitWriteExtBlocks(device, blockSize, bitmapBlock, 1, bitmapBuf.get(), &wBmIo);
                if (!wBmTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(wBmTask);
                if (!wBmIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                fs::BlockIoResult wGdIo;
                kernel::AsyncTask* wGdTask = kSubmitWriteExtBlocks(device, blockSize, gdBlockOffset, 1, gdBuf.get(), &wGdIo);
                if (!wGdTask) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                co_await kernel::AsyncTaskCoroAwaiter(wGdTask);
                if (!wGdIo.ok) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
            }

            // 7b) [Rmdir 실측(e2fsck)으로 이미 발견된 교훈 재적용] 대상
            //     inode의 온디스크 레코드 자체를 0으로 지운다 - 안
            //     지우면 e2fsck의 Pass 1이 비트맵과 무관하게 이 inode를
            //     여전히 살아있는 파일로 오인한다.
            {
                uint64_t freeInodeBlockOffset = 0;
                uint32_t freeInodeByteOffset = 0;
                uint32_t freeInodeBlocksNeededProbe = 0;
                if (!kLocateInode(sb, volume_, groupCount, blockSize, targetInodeNum, &freeInodeBlockOffset,
                                   &freeInodeByteOffset, &freeInodeBlocksNeededProbe)) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                const uint32_t freeInodeBlocksNeeded = static_cast<uint32_t>(
                    kCeilDiv(static_cast<uint64_t>(freeInodeByteOffset) + sb.inodeSize, blockSize));
                SlabBuf freeInodeBlockBuf(freeInodeBlocksNeeded * blockSize);
                if (!freeInodeBlockBuf) {
                    args->error = kernel::VfsError::InvalidArgument;
                    break;
                }
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask = kSubmitReadExtBlocks(device, blockSize, freeInodeBlockOffset,
                                                                      freeInodeBlocksNeeded, freeInodeBlockBuf.get(),
                                                                      &ioResult);
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
                memset(freeInodeBlockBuf.get() + freeInodeByteOffset, 0, sb.inodeSize);
                {
                    fs::BlockIoResult ioResult;
                    kernel::AsyncTask* ioTask = kSubmitWriteExtBlocks(device, blockSize, freeInodeBlockOffset,
                                                                       freeInodeBlocksNeeded, freeInodeBlockBuf.get(),
                                                                       &ioResult);
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

            // 8) 부모 블록에서 엔트리 제거 + 체크섬 갱신 + 씀(부모
            //    inode 자체는 linksCount 변화가 없으므로 다시 쓸
            //    필요 없음 - Rmdir과 다른 점).
            SlabBuf parentBlockBuf(blockSize);
            if (!parentBlockBuf) {
                args->error = kernel::VfsError::InvalidArgument;
                break;
            }
            {
                fs::BlockIoResult ioResult;
                kernel::AsyncTask* ioTask =
                    kSubmitReadExtBlocks(device, blockSize, leafBlockAbs, 1, parentBlockBuf.get(), &ioResult);
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
            uint8_t removedFileType = 0;
            if (!kExt4RemoveDirEntry(parentBlockBuf.get(), blockSize, args->relPath + leafStart,
                                       static_cast<uint8_t>(leafLen), &removedFileType)) {
                args->error = kernel::VfsError::NotFound;  // 방어적 처리(이론상 불가능 - 3단계에서 이미 확인)
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
                    kSubmitWriteExtBlocks(device, blockSize, leafBlockAbs, 1, parentBlockBuf.get(), &ioResult);
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

            // 9) 슈퍼블록 전역 free 카운터 되돌림(+블록개수/+1 inode).
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
                if (!kExt4AdjustSuperblockFreeBlocks(rawSb, static_cast<kernel::int64_t>(targetBlockCount),
                                                       sb.featureIncompat, sb.featureRoCompat) ||
                    !kExt4AdjustSuperblockFreeInodes(rawSb, 1, sb.featureRoCompat)) {
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
