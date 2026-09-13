#ifndef MINICORE_KERNEL_BOOT_INFO_H
#define MINICORE_KERNEL_BOOT_INFO_H

namespace kernel {

constexpr unsigned int kBootInfoMaxModules = 16;

struct BootModule {
    unsigned long physStart;
    unsigned long physEnd;
    const char* cmdline;  // 없으면 nullptr
};

// PVH/multiboot2 어느 프로토콜로 부팅했든 kMain이 이 형태로 통일해
// 둔다(메모리맵/RSDP를 HvmMemmapEntry 배열+물리주소로 통일해 둔 것과
// 같은 패턴, PL-FC38956C) - QU-9DCDCE3E, 설계자 지시, 2026-09-14:
// "커맨드라인, 모듈, 부트로더 이름 등 모두 파싱해서 커널이 보유하도록
// 설계하라. initrd 역시도 마찬가지다."
//
// 문자열/모듈 물리주소는 전부 Paging::init() 이전(저지대 identity
// map 구간)에 얻은 값이라, kMain이 이 시점에 바로 포인터로 읽는다 -
// PVH 경로의 startInfo/memmap을 다루는 방식과 동일.
struct BootInfo {
    const char* cmdline;         // 없으면 nullptr(양쪽 다 null-terminated)
    const char* bootloaderName;  // multiboot2 전용 - PVH는 항상 nullptr
    unsigned int moduleCount;
    BootModule modules[kBootInfoMaxModules];
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_BOOT_INFO_H
