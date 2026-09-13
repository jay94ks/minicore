#ifndef MINICORE_KERNEL_MULTIBOOT2_H
#define MINICORE_KERNEL_MULTIBOOT2_H

#include "hvm_start_info.h"

namespace kernel {

constexpr unsigned int kMultiboot2Magic = 0x36D76289;
constexpr unsigned int kMultiboot2MaxMemmapEntries = 64;

// GRUB(멀티부트2)가 넘기는 태그 기반 가변 길이 정보 구조체를 파싱해
// PVH 경로와 같은 형태(HvmMemmapEntry 배열 + RSDP 물리주소)로 변환한다
// - kMain이 그 뒤로는 부팅 프로토콜을 구분할 필요가 없게 하기 위함
// (PL-FC38956C 4단계). Paging::init() 이전(boot.S의 저지대 identity
// map 안)에서만 안전하게 부를 수 있다 - hvm_start_info 파싱과 같은
// 타이밍(직접 물리주소를 포인터로 캐스팅해서 읽는다, kPhysToVirt 아님).
class Multiboot2Info {
public:
    // outMemmap/maxEntries: 호출자가 준비한 버퍼(정적 배열 권장) - 실제로
    // 채운 개수는 outMemmapCount로 돌려준다(태그 안 엔트리가 더 많아도
    // 버퍼를 넘는 만큼은 무시). outRsdpPaddr: ACPI RSDP 태그(신규=15
    // 우선, 없으면 구형=14)에서 얻은 물리주소 - 둘 다 없으면 0(호출부가
    // Acpi::init 실패로 자연스럽게 처리됨). outTotalSize: MB2 정보
    // 구조체 자체의 전체 바이트 크기 - PageFrameAllocator가 그 범위를
    // usable 메모리에서 제외하는 데 쓴다(hvm_start_info를 위해 하는
    // 것과 동일한 이유).
    static void parse(unsigned long infoPhysAddr, HvmMemmapEntry* outMemmap, unsigned int maxEntries,
                       unsigned int* outMemmapCount, unsigned long* outRsdpPaddr, unsigned int* outTotalSize);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_MULTIBOOT2_H
