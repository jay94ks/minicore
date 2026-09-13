#ifndef MINICORE_KERNEL_HVM_START_INFO_H
#define MINICORE_KERNEL_HVM_START_INFO_H

namespace kernel {

// Xen 공개 헤더(xen/arch-x86/hvm/start_info.h)의 PVH start info -
// 하이퍼바이저가 채워서 EBX(→ 우리 kMain의 인자)로 물리 주소를 넘겨준다.
// 버전 1부터 있는 memmap_paddr/memmap_entries까지만 쓴다.
struct HvmStartInfo {
    unsigned int magic;
    unsigned int version;
    unsigned int flags;
    unsigned int nrModules;
    unsigned long modlistPaddr;
    unsigned long cmdlinePaddr;
    unsigned long rsdpPaddr;
    unsigned long memmapPaddr;
    unsigned int memmapEntries;
    unsigned int reserved;
} __attribute__((packed));

// E820 타입 값을 그대로 재사용한다(Xen 문서에 명시).
enum class HvmMemmapType : unsigned int {
    kUsable = 1,
    kReserved = 2,
    kAcpiReclaimable = 3,
    kAcpiNvs = 4,
    kUnusable = 5,
};

struct HvmMemmapEntry {
    unsigned long addr;
    unsigned long size;
    unsigned int type;
    unsigned int reserved;
} __attribute__((packed));

// HvmStartInfo::modlistPaddr가 가리키는 배열의 원소 하나(Xen PVH
// 스펙) - nrModules개 있다. cmdlinePaddr는 이 모듈 전용 커맨드라인
// (null-terminated) - 없으면 0.
struct HvmModlistEntry {
    unsigned long paddr;
    unsigned long size;
    unsigned long cmdlinePaddr;
    unsigned long reserved;
} __attribute__((packed));

constexpr unsigned int kHvmStartInfoMagic = 0x336ec578;

}  // namespace kernel

#endif  // MINICORE_KERNEL_HVM_START_INFO_H
