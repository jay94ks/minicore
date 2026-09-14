#include "elf.h"

// libcpio(RM-23F4B687 §3 배치 규칙)와 같은 이유로 <cstring>을 쓰지
// 않는다 - x86_64-unknown-none-elf 타깃엔 libc가 없어 <cstring>/
// <string.h>(hosted 헤더) 자체가 없다. 이 파일(순수 파싱부)은 원본
// 버퍼를 reinterpret_cast로 그대로 읽기만 해 복사가 필요 없다 -
// 실제 메모리 매핑용 memset/memcpy는 아래 MINICORE_LIBELF_KERNEL
// 블록에서 libkenv/mem.h(커널이 freestanding용으로 직접 정의한 전역
// memset/memcpy)를 통해서만 쓴다.

namespace elf {

namespace {

constexpr u16 kElfClass64 = 2;       // EI_CLASS
constexpr u16 kElfDataLsb = 1;       // EI_DATA(리틀엔디안)
constexpr u16 kElfTypeExec = 2;      // e_type(ET_EXEC)
constexpr u16 kElfMachineX86_64 = 62;  // e_machine(EM_X86_64)

}  // namespace

Error Image::parse(const void* data, u64 size, Image* out) {
    if (size < sizeof(FileHeader)) {
        return Error::TooSmall;
    }
    const auto* bytes = static_cast<const u8*>(data);
    const auto* header = reinterpret_cast<const FileHeader*>(bytes);

    if (header->ident[0] != 0x7F || header->ident[1] != 'E' || header->ident[2] != 'L' ||
        header->ident[3] != 'F') {
        return Error::BadMagic;
    }
    if (header->ident[4] != kElfClass64) {
        return Error::Not64Bit;
    }
    if (header->ident[5] != kElfDataLsb) {
        return Error::NotLittleEndian;
    }
    if (header->type != kElfTypeExec) {
        // v1은 비-PIE 고정 실행파일(ET_EXEC)만 지원한다(SP-8B6B8D25
        // §5-A 확정 - 유저 주소공간은 0x400000 고정 베이스, 재배치/
        // 동적 링킹은 범위 밖). ET_DYN(공유 라이브러리/PIE)은 향후
        // userland/tests/libuserlandtest의 동적 로더 과제로 남긴다.
        return Error::NotExecutable;
    }
    if (header->machine != kElfMachineX86_64) {
        return Error::WrongMachine;
    }
    if (header->phentsize != sizeof(ProgramHeader)) {
        return Error::BadProgramHeaderTable;
    }
    // phoff + phnum*phentsize가 오버플로우 없이 size 이내인지 확인.
    const u64 phTableBytes = static_cast<u64>(header->phnum) * header->phentsize;
    if (header->phoff > size || phTableBytes > size - header->phoff) {
        return Error::BadProgramHeaderTable;
    }

    out->_data = bytes;
    out->_size = size;
    out->_entry = header->entry;
    out->_phoff = header->phoff;
    out->_phentsize = header->phentsize;
    out->_phnum = header->phnum;
    return Error::None;
}

ProgramHeader Image::segment(u32 index) const {
    return *reinterpret_cast<const ProgramHeader*>(_data + _phoff + static_cast<u64>(index) * _phentsize);
}

const u8* Image::segmentData(const ProgramHeader& seg) const {
    return _data + seg.offset;
}

}  // namespace elf

#if defined(MINICORE_LIBELF_KERNEL)

#include "libkenv/mem.h"
#include "page_frame_allocator.h"
#include "paging.h"

namespace elf {

bool loadIntoAddressSpace(const Image& image, u64 pml4Phys) {
    constexpr u64 kPageSize = 4096;

    for (u32 i = 0; i < image.segmentCount(); ++i) {
        const ProgramHeader seg = image.segment(i);
        if (seg.type != kSegmentTypeLoad || seg.memsz == 0) {
            continue;
        }

        // NX(실행 금지) 비트는 이 프로젝트의 Paging 플래그에 아직 없다
        // - 코드/데이터 구분 없이 전부 실행 가능하게 매핑된다(v1 한계,
        // PTE의 NX 비트 도입은 별도 계획으로 남긴다). "프로그래밍되지
        // 않은 영역에 대한 실행 탐지"(SP-8B6B8D25 §5-A)는 이 함수가
        // 세그먼트가 선언한 페이지만 매핑하고 그 밖은 전부 not-present로
        // 남겨 두는 것으로 이미 만족된다(그 밖을 실행하려 하면 자연히
        // #PF).
        u64 flags = kernel::PAGE_USER;
        if (seg.flags & kSegmentFlagWrite) {
            flags |= kernel::PAGE_WRITABLE;
        }

        const u64 segStartPage = seg.vaddr & ~(kPageSize - 1);
        const u64 segEndPage = (seg.vaddr + seg.memsz + kPageSize - 1) & ~(kPageSize - 1);
        const u8* fileData = image.segmentData(seg);
        const u64 fileVaEnd = seg.vaddr + seg.filesz;

        for (u64 pageAddr = segStartPage; pageAddr < segEndPage; pageAddr += kPageSize) {
            const u64 phys = kernel::PageFrameAllocator::allocPage();
            if (!phys) {
                return false;  // 고갈 - 이미 매핑한 페이지 정리는 호출부(Process::destroy) 책임
            }
            auto* dest = reinterpret_cast<u8*>(kernel::kPhysToVirt(phys));
            memset(dest, 0, kPageSize);

            // 이 페이지가 커버하는 가상주소 구간과 세그먼트의 파일
            // 데이터 구간의 교집합만 복사한다 - memsz가 filesz보다
            // 크면 나머지(BSS)는 위 memset의 0이 그대로 남는다.
            const u64 pageEnd = pageAddr + kPageSize;
            const u64 copyStart = pageAddr > seg.vaddr ? pageAddr : seg.vaddr;
            const u64 copyEnd = pageEnd < fileVaEnd ? pageEnd : fileVaEnd;
            if (copyStart < copyEnd) {
                memcpy(dest + (copyStart - pageAddr), fileData + (copyStart - seg.vaddr), copyEnd - copyStart);
            }

            kernel::Paging::mapPage(pageAddr, phys, flags, pml4Phys);
        }
    }
    return true;
}

}  // namespace elf

#endif  // MINICORE_LIBELF_KERNEL
