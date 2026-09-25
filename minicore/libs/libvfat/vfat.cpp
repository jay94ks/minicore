#include "vfat.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace vfat {

namespace {

constexpr uint64_t kCeilDiv(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

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
    // [신규, 2026-09-23, PN-9D6FE4B6 준비 작업] 쓰기 경로(§3.4 free
    // 클러스터 스캔의 종료 조건 - 클러스터 번호는 2..clusterCount+1
    // 범위)와 다중 FAT 사본 동기화(§3.4 "numFats가 2 이상이면 모든
    // FAT 사본에 반영")에 필요해 저장해 둔다 - 위에서 이미 계산해
    // 뒀던 값들을 그대로 멤버로 옮기는 것뿐, 판별 로직 자체는 그대로.
    clusterCount_ = clusterCount;
    numFats_ = bpb_.numFats;
    fatSize32_ = ext32_.fatSize32;
    device_ = device;
    return true;
}

bool Fat16Volume::mount(fs::BlockDevice* device) {
    if (!device) {
        return false;
    }
    const kernel::uint32_t devBlockSize = device->blockSize();
    if (devBlockSize == 0) {
        return false;
    }

    // 1) 부트 섹터 - Fat32Volume::mount()와 동일한 방식, 다만
    // Fat32Extended 대신 Fat16Extended(kFat16ExtendedOffset=36)를 읽는다.
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
    memcpy(&ext16_, buf + kFat16ExtendedOffset, sizeof(ext16_));
    kernel::GenericSlabAllocator::free(buf, blocksNeeded * devBlockSize);

    // 2) BPB 기본 유효성 - Fat32Volume::mount()와 동일한 검사, 다만
    // fatSize16(BpbCommon 자신의 필드)이 FAT12/16의 FAT 크기다.
    if (bpb_.bytesPerSector == 0 || (bpb_.bytesPerSector & (bpb_.bytesPerSector - 1)) != 0) {
        return false;
    }
    if (bpb_.sectorsPerCluster == 0 || (bpb_.sectorsPerCluster & (bpb_.sectorsPerCluster - 1)) != 0) {
        return false;
    }
    if (bpb_.numFats == 0 || bpb_.fatSize16 == 0 || bpb_.rootEntryCount == 0) {
        return false;
    }

    // 3) §3.2 클러스터 수 기반 포맷 재확인(fileSystemType 문자열은
    // 신뢰하지 않음) - FAT32와 달리 고정 루트 디렉터리 영역까지 뺀
    // 뒤에야 데이터 영역/클러스터 수가 나온다(§3.3).
    const uint32_t totalSectors = (bpb_.totalSectors32 != 0) ? bpb_.totalSectors32 : bpb_.totalSectors16;
    const uint32_t fatSectors = static_cast<uint32_t>(bpb_.numFats) * bpb_.fatSize16;
    uint32_t rootDirStartSector = 0;
    uint32_t rootDirSectorCount = 0;
    kFatFixedRootDirLocation(bpb_.reservedSectorCount, bpb_.numFats, bpb_.fatSize16, bpb_.rootEntryCount,
                              bpb_.bytesPerSector, &rootDirStartSector, &rootDirSectorCount);
    const uint32_t nonDataSectors = bpb_.reservedSectorCount + fatSectors + rootDirSectorCount;
    if (totalSectors <= nonDataSectors) {
        return false;
    }
    const uint32_t dataSectors = totalSectors - nonDataSectors;
    const uint32_t clusterCount = dataSectors / bpb_.sectorsPerCluster;
    FatEntryWidth width;
    if (clusterCount < 4085) {
        width = FatEntryWidth::Fat12;
    } else if (clusterCount < 65525) {
        width = FatEntryWidth::Fat16;
    } else {
        return false;  // FAT32 범위 - Fat32Volume 몫
    }

    // 4) 장치 블록 크기와의 정합성 - Fat32Volume::mount()와 동일 제약.
    if (bpb_.bytesPerSector % devBlockSize != 0) {
        return false;
    }

    entryWidth_ = width;
    fatStartSector_ = bpb_.reservedSectorCount;
    fatSizeSectors_ = bpb_.fatSize16;
    numFats_ = bpb_.numFats;
    rootDirStartSector_ = rootDirStartSector;
    rootDirSectorCount_ = rootDirSectorCount;
    dataStartSector_ = rootDirStartSector + rootDirSectorCount;
    bytesPerCluster_ = static_cast<uint32_t>(bpb_.sectorsPerCluster) * bpb_.bytesPerSector;
    clusterCount_ = clusterCount;
    device_ = device;
    return true;
}

}  // namespace vfat
