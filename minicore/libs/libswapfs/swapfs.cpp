#include "swapfs.h"

#include "block_device.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"

namespace fs {

namespace {

// SwapHeaderInfo 안에서 badPages가 시작하는 바이트 오프셋 - 이 struct는
// badPages를 1개짜리 배열로만 선언해 두므로(리눅스 원문의 가변 길이
// 관례 그대로), nrBadPages > 1인 나머지는 이 오프셋부터 장치에서 직접
// 읽어야 한다. offsetof(freestanding 환경에 <stddef.h> 존재 여부가
// 불확실 - RM-23F4B687 "표준 헤더는 실제 확인 전까지 가정하지 않는다")
// 대신 필드 폭을 그대로 더한 상수로 계산한다(SwapHeaderInfo 필드
// 순서와 반드시 함께 유지):
//   bootbits[1024] + version(4) + lastPage(4) + nrBadPages(4) +
//   uuid[16] + volumeName[16] + padding[117*4=468] = 1536.
constexpr kernel::uint64_t kSwapBadPagesOffset = 1536;

}  // namespace

bool SwapfsBackend::mount(BlockDevice* device) {
    if (!device) {
        return false;
    }
    const kernel::uint32_t blockSize = device->blockSize();
    if (blockSize == 0 || kSwapPageSize % blockSize != 0) {
        return false;  // 이 v1은 4096이 블록 크기의 정확한 배수인 장치만 지원
    }
    const kernel::uint32_t blocksPerPage = static_cast<kernel::uint32_t>(kSwapPageSize / blockSize);

    // 1) 페이지 0(헤더) 읽기 - 4096바이트를 커널 스택(8KiB)에 로컬
    // 배열로 두지 않고 슬랩에서 확보한다(task.h의 커널 스택 크기 문서
    // 주석 참고 - 이 함수 하나가 스택 절반을 차지하면 안 됨).
    auto* headerPage = static_cast<kernel::uint8_t*>(kernel::GenericSlabAllocator::alloc(kSwapPageSize));
    if (!headerPage) {
        return false;
    }
    if (!device->readBlocks(0, blocksPerPage, headerPage)) {
        kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
        return false;
    }

    // 2) 매직 확인 - 불일치는 에러가 아니라 "이 포맷이 아님"(다음
    // 드라이버 시도 경로, ext4/FAT32와 동일한 판별 실패 관례).
    for (kernel::uint64_t i = 0; i < sizeof(kSwapMagic); ++i) {
        if (headerPage[kSwapMagicOffset + i] != static_cast<kernel::uint8_t>(kSwapMagic[i])) {
            kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
            return false;
        }
    }

    memcpy(&header_, headerPage, sizeof(header_));
    if (header_.version != kSwapFormatVersion) {
        kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
        return false;  // 서브버전 불일치(swapfs.h의 kSwapFormatVersion 주석 참고) - 매직은 이미 위에서 확인됨
    }
    if (header_.lastPage == 0) {
        kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
        return false;  // 데이터 슬롯이 하나도 없는 스왑 영역 - 무의미
    }

    // 3) 런타임 전용 사용 비트맵 확보(§3.2 - 디스크에 절대 안 씀) -
    // 슬롯 0..lastPage를 전부 표현할 수 있게 (lastPage+1)비트.
    const kernel::uint64_t bitCount = static_cast<kernel::uint64_t>(header_.lastPage) + 1;
    usedBitmapBytes_ = (bitCount + 7) / 8;
    usedBitmap_ = static_cast<kernel::uint8_t*>(kernel::GenericSlabAllocator::alloc(usedBitmapBytes_));
    if (!usedBitmap_) {
        kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
        return false;
    }
    memset(usedBitmap_, 0, usedBitmapBytes_);
    markSlotUsed(0);  // 슬롯 0은 헤더 자신 - 데이터 슬롯으로 절대 배정되지 않음

    // 4) badPages[]를 읽어 영구 할당 불가로 표시 - nrBadPages는 이론상
    // 수천 개까지 갈 수 있어(손상된 값이면 더 클 수도 있음) 이것도
    // 정확히 필요한 만큼만 슬랩에서 확보한다.
    const kernel::uint64_t bytesNeeded = static_cast<kernel::uint64_t>(header_.nrBadPages) * sizeof(kernel::uint32_t);
    if (kSwapBadPagesOffset + bytesNeeded > kSwapPageSize) {
        kernel::GenericSlabAllocator::free(usedBitmap_, usedBitmapBytes_);
        usedBitmap_ = nullptr;
        kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
        return false;  // nrBadPages가 손상됐거나 이 v1의 가정(헤더 페이지 안에 다 들어감)을 벗어남
    }
    if (bytesNeeded > 0) {
        auto* badPages = static_cast<kernel::uint32_t*>(kernel::GenericSlabAllocator::alloc(bytesNeeded));
        if (!badPages) {
            kernel::GenericSlabAllocator::free(usedBitmap_, usedBitmapBytes_);
            usedBitmap_ = nullptr;
            kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
            return false;
        }
        memcpy(badPages, headerPage + kSwapBadPagesOffset, bytesNeeded);
        for (kernel::uint32_t i = 0; i < header_.nrBadPages; ++i) {
            if (badPages[i] >= 1 && badPages[i] <= header_.lastPage) {
                markSlotUsed(badPages[i]);
            }
        }
        kernel::GenericSlabAllocator::free(badPages, bytesNeeded);
    }

    kernel::GenericSlabAllocator::free(headerPage, kSwapPageSize);
    device_ = device;
    nextScanHint_ = 1;
    return true;
}

bool SwapfsBackend::isSlotUsed(SwapSlot slot) const {
    return (usedBitmap_[slot / 8] & (1u << (slot % 8))) != 0;
}

void SwapfsBackend::markSlotUsed(SwapSlot slot) {
    usedBitmap_[slot / 8] |= static_cast<kernel::uint8_t>(1u << (slot % 8));
}

void SwapfsBackend::markSlotFree(SwapSlot slot) {
    usedBitmap_[slot / 8] &= static_cast<kernel::uint8_t>(~(1u << (slot % 8)));
}

bool SwapfsBackend::allocateSlot(SwapSlot* out) {
    if (!device_) {
        return false;
    }
    // nextScanHint_부터 한 바퀴(lastPage개) 선형 스캔 - 순수 성능
    // 힌트일 뿐 정확성에는 영향 없음(§3.4 문서 주석 그대로).
    for (kernel::uint64_t attempts = 0; attempts < header_.lastPage; ++attempts) {
        const SwapSlot candidate = nextScanHint_;
        nextScanHint_ = (nextScanHint_ >= header_.lastPage) ? 1 : nextScanHint_ + 1;
        if (!isSlotUsed(candidate)) {
            markSlotUsed(candidate);
            *out = candidate;
            return true;
        }
    }
    return false;  // 스왑 공간 완전 고갈
}

void SwapfsBackend::freeSlot(SwapSlot slot) {
    if (!device_ || slot == 0 || slot > header_.lastPage) {
        return;
    }
    markSlotFree(slot);  // 이중 해제는 방어하지 않음(PTE당 슬롯 소유권 1:1이 불변조건, SP-D02C4A73 §5)
}

bool SwapfsBackend::writeSlot(SwapSlot slot, const void* page) {
    if (!device_ || slot == 0 || slot > header_.lastPage) {
        return false;
    }
    const kernel::uint32_t blockSize = device_->blockSize();
    const kernel::uint32_t blocksPerPage = static_cast<kernel::uint32_t>(kSwapPageSize / blockSize);
    const kernel::uint64_t lba = (slot * kSwapPageSize) / blockSize;
    return device_->writeBlocks(lba, blocksPerPage, page);
}

bool SwapfsBackend::readSlot(SwapSlot slot, void* page) {
    if (!device_ || slot == 0 || slot > header_.lastPage) {
        return false;
    }
    const kernel::uint32_t blockSize = device_->blockSize();
    const kernel::uint32_t blocksPerPage = static_cast<kernel::uint32_t>(kSwapPageSize / blockSize);
    const kernel::uint64_t lba = (slot * kSwapPageSize) / blockSize;
    return device_->readBlocks(lba, blocksPerPage, page);
}

}  // namespace fs
