#ifndef MINICORE_KERNEL_HVM_START_INFO_H
#define MINICORE_KERNEL_HVM_START_INFO_H

#include "libkenv/types.h"

namespace kernel {

// Xen 공개 헤더(xen/arch-x86/hvm/start_info.h)의 PVH start info -
// 하이퍼바이저가 채워서 EBX(→ 우리 kMain의 인자)로 물리 주소를 넘겨준다.
// 버전 1부터 있는 memmap_paddr/memmap_entries까지만 쓴다.
struct HvmStartInfo {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nrModules;
    uint64_t modlistPaddr;
    uint64_t cmdlinePaddr;
    uint64_t rsdpPaddr;
    uint64_t memmapPaddr;
    uint32_t memmapEntries;
    uint32_t reserved;
} __attribute__((packed));

// E820 타입 값을 그대로 재사용한다(Xen 문서에 명시).
enum class HvmMemmapType : uint32_t {
    kUsable = 1,
    kReserved = 2,
    kAcpiReclaimable = 3,
    kAcpiNvs = 4,
    kUnusable = 5,
};

struct HvmMemmapEntry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

// HvmStartInfo::modlistPaddr가 가리키는 배열의 원소 하나(Xen PVH
// 스펙) - nrModules개 있다. cmdlinePaddr는 이 모듈 전용 커맨드라인
// (null-terminated) - 없으면 0.
struct HvmModlistEntry {
    uint64_t paddr;
    uint64_t size;
    uint64_t cmdlinePaddr;
    uint64_t reserved;
} __attribute__((packed));

constexpr uint32_t kHvmStartInfoMagic = 0x336ec578;

}  // namespace kernel

#endif  // MINICORE_KERNEL_HVM_START_INFO_H
