#ifndef MINICORE_LIBS_LIBELF_ELF_H
#define MINICORE_LIBS_LIBELF_ELF_H

// libelf: ELF64 실행 파일 파서 - 커널의 프로세스 로더(PN-16CA347D)와
// 유저랜드의 향후 동적 라이브러리 로더(userland/tests/libuserlandtest
// 등, "동적 라이브러리도 되는지 테스트" 확정) 양쪽에서 재사용하도록
// 설계됐다(설계자 지시, 2026-09-14). libcpio(RM-23F4B687 §3 배치 규칙,
// "kernel/userland 공용이라 kernel:: 네임스페이스나 freestanding 전용
// 타입에 의존하지 않는다")와 완전히 같은 원칙 - 표준 C++ 타입만 쓰고
// libkenv에 의존하지 않는다.
//
// 이 헤더/구현이 하는 일은 **순수 파싱뿐**이다 - 실제로 세그먼트를
// 페이지 테이블에 매핑하는 것은 호출부(커널의 프로세스 로더는
// Paging::mapPage, 유저랜드 동적 로더는 각자의 mmap류 syscall)의
// 책임이다. `MINICORE_LIBELF_KERNEL` 매크로가 정의됐을 때만 커널
// 전용 편의 함수(loadIntoAddressSpace, elf.cpp 하단)가 추가로
// 컴파일된다 - **매크로가 없으면(기본값) 유저 영역용**이라 이
// 헤더/파서 자체는 어떤 OS 종속 헤더도 include하지 않는다(설계자
// 지시 "기본적으로 유저 영역용으로 컴파일되도록"). 커널 빌드
// (minicore/libs/libelf/CMakeLists.txt)만 이 매크로를 정의해 켠다 -
// 커널과 유저랜드는 애초에 서로 다른 툴체인(mcmodel/PIE 등)으로
// 컴파일되는 별도 빌드라 하나의 컴파일된 오브젝트를 공유할 수 없고,
// 공유되는 건 이 소스 코드 자체다.
namespace elf {

// libcpio(cpio.h)와 같은 이유로 <cstdint>를 쓰지 않는다 - 이
// freestanding 타깃엔 표준 헤더가 없다(libkenv/types.h의 동일 주석
// 참고). x86_64에서 unsigned long이 64비트임은 이미 kernel::uint64_t
// 정의(libkenv/types.h)와 이 프로젝트 전역에서 전제하는 사실.
using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long;

// ELF64 파일 헤더(System V ABI, e_ident 16바이트 + 나머지) - 파서
// 내부에서만 쓰고, 호출부에는 Segment 목록/entryPoint()로 가공해
// 노출한다(호출부가 raw ELF 구조체를 직접 다룰 필요가 없게).
struct FileHeader {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};
static_assert(sizeof(FileHeader) == 64, "ELF64 파일 헤더는 정확히 64바이트여야 한다");

// ELF64 프로그램 헤더(세그먼트 서술자, System V ABI) - PT_LOAD 타입만
// 실제 메모리 매핑 대상이다(그 외 타입은 v1에서 전부 무시 - PT_DYNAMIC/
// PT_INTERP는 동적 링킹용이라 이번 비-PIE v1 범위 밖, PT_NOTE/PT_TLS
// 등도 마찬가지).
struct ProgramHeader {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};
static_assert(sizeof(ProgramHeader) == 56, "ELF64 프로그램 헤더는 정확히 56바이트여야 한다");

constexpr u32 kSegmentTypeLoad = 1;  // PT_LOAD

constexpr u32 kSegmentFlagExecute = 1;  // PF_X
constexpr u32 kSegmentFlagWrite = 2;    // PF_W
constexpr u32 kSegmentFlagRead = 4;     // PF_R

enum class Error {
    None,
    TooSmall,           // 파일 헤더조차 못 담을 만큼 작음
    BadMagic,            // e_ident[0..3] != 0x7F 'E' 'L' 'F'
    Not64Bit,            // EI_CLASS != ELFCLASS64
    NotLittleEndian,     // EI_DATA != ELFDATA2LSB(x86_64는 항상 리틀엔디안)
    NotExecutable,       // e_type != ET_EXEC(v1은 비-PIE 고정 실행파일만, ET_DYN 등은 범위 밖)
    WrongMachine,        // e_machine != EM_X86_64
    BadProgramHeaderTable,  // phentsize가 스펙과 다르거나 phoff+phnum*phentsize가 파일 범위를 벗어남
};

// 파싱된 ELF64 이미지 - 원본 바이트 버퍼를 복사하지 않고 그대로
// 가리키기만 한다(호출부가 이미 메모리에 올려 둔 initrd 안의 파일
// 등을 그대로 재사용하려는 의도) - 그래서 Image가 살아있는 동안 원본
// 버퍼도 유효해야 한다.
class Image {
public:
    // data/size: 파일 전체 바이트. 성공하면 Error::None과 함께 *out을
    // 채운다 - 실패하면 *out은 건드리지 않는다.
    static Error parse(const void* data, u64 size, Image* out);

    u64 entryPoint() const { return _entry; }
    u32 segmentCount() const { return _phnum; }

    // index < segmentCount()만 유효(호출부 책임, parse()가 이미
    // phoff+phnum*phentsize가 파일 범위 안임을 검증해 뒀으므로 이
    // 함수 자체는 다시 경계 검사를 하지 않는다).
    ProgramHeader segment(u32 index) const;

    // segment(index)가 PT_LOAD이면 그 세그먼트의 파일 내용 시작
    // 주소를 돌려준다(원본 버퍼 안을 직접 가리킴, filesz바이트만
    // 유효 - memsz가 더 크면 나머지는 0으로 채워야 한다, BSS류).
    const u8* segmentData(const ProgramHeader& seg) const;

private:
    const u8* _data = nullptr;
    u64 _size = 0;
    u64 _entry = 0;
    u64 _phoff = 0;
    u16 _phentsize = 0;
    u16 _phnum = 0;
};

#if defined(MINICORE_LIBELF_KERNEL)

}  // namespace elf

namespace kernel {
class ProcessAddressSpaceManager;  // 포인터로만 참조 - 전체 정의는 address_space.h(PN-71C3D483 항목 3)
}  // namespace kernel

namespace elf {

// 커널 전용 편의 함수 - Image가 가진 모든 PT_LOAD 세그먼트를 주어진
// 프로세스 주소공간(pml4Phys, Paging::createAddressSpace가 만든 것)에
// 실제로 매핑한다. GenericSlabAllocator/PageFrameAllocator로 세그먼트당
// 필요한 프레임을 확보해 파일 내용을 복사하고(memsz > filesz분은 0으로
// 채움 - BSS), Paging::mapPage(..., pml4Phys)로 유저 권한(PAGE_USER)
// 매핑한다. 세그먼트마다 매핑을 마치는 즉시
// `addressSpace->registerFixedRegion(...)`으로 그 범위를 장부에
// 등록한다(PN-71C3D483 항목 3 - Process::destroy()가 나중에
// ProcessAddressSpaceManager::unmapAll()로 이 페이지들을 찾아 반납할
// 수 있으려면 먼저 존재를 알아야 한다). 실패(할당 고갈, 또는 겹치는
// 세그먼트라 등록 자체가 거부됨 등) 시 false - 이미 매핑/등록한
// 세그먼트를 되돌리는 롤백은 호출부(프로세스 생성 실패 처리) 책임이다
// (이 함수는 Process::destroy()가 있으니 그쪽에서 정리 가능).
bool loadIntoAddressSpace(const Image& image, u64 pml4Phys, kernel::ProcessAddressSpaceManager* addressSpace);

#endif  // MINICORE_LIBELF_KERNEL

}  // namespace elf

#endif  // MINICORE_LIBS_LIBELF_ELF_H
