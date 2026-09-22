#ifndef MINICORE_LIBS_LIBSWAPFS_SWAPFS_H
#define MINICORE_LIBS_LIBSWAPFS_SWAPFS_H

#include "libkenv/types.h"
#include "swap_backend.h"

// libswapfs - Linux swap-space v2 온디스크 포맷을 그대로 따르는 스왑
// 백엔드(SP-D02C4A73, PN-6D9A5DAE) - 설계자 답변(QU-CE388254) "libswapfs도
// 외부 표준에 맞춰야해"에 따라 이 프로젝트가 새로 고안하는 포맷이
// 아니라 기존 Linux swap-space v2(매직 "SWAPSPACE2")를 그대로 읽고
// 쓴다 - `mkswap`이 만든 실제 스왑 파티션/파일을 그대로 마운트할 수
// 있어야 한다는 게 표준 준수의 실질적 검증 기준이다.
namespace kernel {
class Logger;
}

namespace fs {

// [SP-D02C4A73 §3.1] Linux 커널 include/linux/swap.h의
// `union swap_header::info`와 필드 순서/폭까지 정확히 일치(실제
// 리눅스 소스로 1바이트 단위 대조 완료, 2026-09-22 - torvalds/linux
// master, include/linux/swap.h). `version`/`lastPage`/`nrBadPages`는
// 리눅스 원문의 `__u32`(4바이트, 부호 없음)와 동일하게 `uint32_t`.
struct SwapHeaderInfo {
    char bootbits[1024];       // 부트로더/디스크레이블 예약 공간 - 이 커널은 안 씀
    kernel::uint32_t version;  // 스왑 포맷 내부 버전 - v1 범위는 2("SWAPSPACE2")만 지원
    kernel::uint32_t lastPage;    // 마지막 사용 가능 페이지 번호(1-based, 유효 슬롯은 1..lastPage)
    kernel::uint32_t nrBadPages;  // badPages[]의 실제 원소 수
    kernel::uint8_t uuid[16];
    kernel::uint8_t volumeName[16];
    kernel::uint32_t padding[117];  // 리눅스 원문과 동일한 길이(1바이트 단위 대조 완료)
    kernel::uint32_t badPages[1];   // nrBadPages개, 가변 길이로 이어짐 - v1은 첫 원소만 이 struct로 읽음
};

// 헤더는 항상 블록 장치의 페이지 0(4096바이트) 전체를 차지한다 -
// 매직은 별도 필드가 아니라 그 페이지의 마지막 10바이트 위치 자체가
// 자리다(실제 리눅스 구현도 union으로 같은 페이지를 두 가지 시선으로
// 겹쳐 본다 - 이 라이브러리는 union 대신 오프셋 상수로 그 자리만
// 읽고 쓴다, 동일한 온디스크 결과).
constexpr kernel::uint64_t kSwapPageSize = 4096;
constexpr kernel::uint64_t kSwapMagicOffset = kSwapPageSize - 10;
constexpr char kSwapMagic[10] = {'S', 'W', 'A', 'P', 'S', 'P', 'A', 'C', 'E', '2'};
// [정정, 2026-09-22, 실측 발견] SP-D02C4A73 원안은 "SWAPSPACE2"의
// "2"에 이끌려 이 필드가 2여야 한다고 가정했으나 틀렸다 - 실제
// `mkswap`(util-linux 2.39.3)으로 만든 스왑 이미지를 직접 만들어
// 헥스덤프로 확인한 결과 `info.version` 필드는 항상 1이었고, 리눅스
// 커널 소스(mm/swapfile.c의 `read_swap_header()`)도 정확히
// `if (swap_header->info.version != 1)`로 검사한다(주석: "Check the
// swap header's sub-version") - "SWAP-SPACE"(구버전) vs "SWAPSPACE2"
// (신버전) 구분은 **매직 문자열 자체**가 전담하고, 이 `version`
// 필드는 신버전 안에서 한 번도 바뀐 적 없는 별개의 "서브버전"
// 상수다. 매직을 이미 "SWAPSPACE2"로 확인한 이 코드에서는 이
// 상수가 곧 "그 신버전 안에서 지원하는 유일한 서브버전"이라는 뜻.
constexpr kernel::uint32_t kSwapFormatVersion = 1;

// [SP-D02C4A73 §3.2] Linux swap 포맷은 디스크에 free/used 비트맵을
// 두지 않는다 - 어느 슬롯이 쓰이는지는 순수 런타임(메모리) 상태다.
// mount() 시 1..lastPage 전부를 free로 초기화하고 badPages[]에 나열된
// 페이지만 영구 할당 불가로 표시한다.
class SwapfsBackend : public SwapBackend {
public:
    SwapfsBackend() = default;

    bool mount(BlockDevice* device) override;
    bool writeSlot(SwapSlot slot, const void* page) override;
    bool readSlot(SwapSlot slot, void* page) override;
    bool allocateSlot(SwapSlot* out) override;
    void freeSlot(SwapSlot slot) override;

    // 진단/테스트 전용 - mount() 이후에만 유효한 값.
    const SwapHeaderInfo& header() const { return header_; }

private:
    BlockDevice* device_ = nullptr;
    SwapHeaderInfo header_{};
    // 런타임 전용 사용 비트맵(§3.2) - header_.lastPage비트(슬롯
    // 1..lastPage, 슬롯 0은 헤더 자신이라 절대 배정되지 않음).
    // GenericSlabAllocator에서 확보 - 디스크에 절대 쓰지 않는다.
    kernel::uint8_t* usedBitmap_ = nullptr;
    kernel::uint64_t usedBitmapBytes_ = 0;
    kernel::uint64_t nextScanHint_ = 1;  // 슬롯 0(헤더)부터 스캔하지 않도록 1부터 시작 - 순수 성능 힌트

    bool isSlotUsed(SwapSlot slot) const;
    void markSlotUsed(SwapSlot slot);
    void markSlotFree(SwapSlot slot);
};

}  // namespace fs

#endif  // MINICORE_LIBS_LIBSWAPFS_SWAPFS_H
