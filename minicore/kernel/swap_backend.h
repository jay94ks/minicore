#ifndef MINICORE_KERNEL_SWAP_BACKEND_H
#define MINICORE_KERNEL_SWAP_BACKEND_H

#include "libkenv/types.h"

// [SP-2BCE5D60 §4, 확정 2026-09-18, 설계자 opinion("swapfs는 기존의
// FileSystemDriver와 별도로 분리해야 할것 같은데")] fs 서비스 내부 -
// swapfs는 ext4/FAT32 같은 "디렉터리 트리를 가진 범용 파일시스템"이
// 아니라 스왑 슬롯(고정 크기 블록) 배열에 가까운 단순 구조라, §3의
// `FileSystemDriver`(9개 파일 API) 대신 훨씬 좁은 이 전용 인터페이스로
// 분리한다 - "슬롯 번호 -> 물리 페이지 크기 블록" 매핑만 다룬다.
//
// [신규, 2026-09-22, PN-6D9A5DAE] 이 문서가 이미 확정해 둔 인터페이스를
// 실제 코드로 처음 옮긴다 - fs가 SP-43331889로 Process 없는 순수 커널
// KernelThread에 완전히 흡수된 뒤라(block_device.h와 동일한 사정),
// SP-2BCE5D60 §1-A/§4가 원래 상정했던 "유저랜드 fs 서비스"라는
// 전제는 이제 무의미해졌다 - `BlockDevice`(block_device.h)가 이미
// 순수 kernel:: 타입만 쓰는 커널 전용 인터페이스로 자리 잡았으므로,
// 이 인터페이스도 그 실제 배치를 그대로 따른다(dual-compile 매크로
// 없음 - 현재 유일한 소비자가 커널 자신이라 libelf/libcpio류의
// 커널/유저 공용 장치가 필요 없다).
namespace kernel {
struct AsyncTask;
}  // namespace kernel

namespace fs {

class BlockDevice;

using SwapSlot = kernel::uint64_t;

class SwapBackend {
public:
    // [정정, 2026-09-18, 설계자 지시] readOnly 파라미터 없음 - 스왑은
    // 절대 읽기 전용으로 마운트되면 안 된다(스왑아웃이 곧 이 백엔드에
    // 대한 쓰기이므로 그 자체로 의미가 있어야 함).
    virtual bool mount(BlockDevice* device) = 0;
    // 물리 페이지 하나(4KiB 가정)를 슬롯에 기록/조회 - 파일 오프셋이
    // 아니라 슬롯 번호로 직접 색인.
    virtual bool writeSlot(SwapSlot slot, const void* page) = 0;
    virtual bool readSlot(SwapSlot slot, void* page) = 0;
    // 빈 슬롯 할당/반납 - ext4의 mkdir/unlink에 대응하는 "공간 관리"
    // 축이지만 이름/경로 개념이 없어 훨씬 단순하다.
    virtual bool allocateSlot(SwapSlot* out) = 0;
    virtual void freeSlot(SwapSlot slot) = 0;

    // [신규, 2026-09-22, PN-6D9A5DAE] block_device.h의 BlockDevice와
    // 동일한 이유로 가상 소멸자를 일부러 안 둔다(그 헤더의 클래스 문서
    // 주석 참고) - 이 인터페이스의 유일한 구현체(SwapfsBackend,
    // libswapfs)도 정적/전역 인스턴스로만 쓰인다(heap 할당/delete 없음).
};

}  // namespace fs

#endif  // MINICORE_KERNEL_SWAP_BACKEND_H
