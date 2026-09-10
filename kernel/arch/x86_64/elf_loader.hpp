// 최소 ELF64 로더 (docs/plan/kernel-bootstrap.md M8). 동적 링킹·재배치는
// 다루지 않는다 — initrun은 고정 주소에 링크된 정적 실행파일(ET_EXEC)
// 하나뿐이라는 전제(init/initrun/link.ld)로 충분하다. PT_LOAD 세그먼트만
// 처리한다.
#pragma once

#include <cstdint>

#include <k/result.hpp>

namespace kern::arch::x86_64 {

enum class elf_error : uint32_t {
    bad_magic,
    unsupported_class,
    unsupported_machine,
    unsupported_type,
    truncated,
    out_of_memory,
    map_failed,
};

// elf_data[0..elf_size)를 pml4_phys가 가리키는 주소공간(이미
// create_address_space_root()로 만들어져 커널 higher-half가 복사되어
// 있어야 한다)에 PT_LOAD 세그먼트별로 매핑한다. 파일 바이트를 그대로
// 복사하고, p_memsz가 p_filesz보다 크면 나머지는 0으로 채운다(.bss).
// 성공하면 ELF 헤더의 entry point(유저 가상주소)를 반환한다.
result<uint64_t, elf_error> load_elf(uint64_t pml4_phys, const uint8_t* elf_data,
                                      uint64_t elf_size);

}  // namespace kern::arch::x86_64
