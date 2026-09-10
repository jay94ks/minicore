// 최소 ELF64 로더 구현. elf_loader.hpp 상단 주석 참고.
#include "elf_loader.hpp"

#include "page_table.hpp"

#include <klog.hpp>
#include <mm/page_allocator.hpp>
#include <mm/phys_map.hpp>

namespace kern::arch::x86_64 {

namespace {

struct elf64_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

constexpr uint32_t k_pt_load = 1;
constexpr uint32_t k_pf_exec = 1u << 0;
constexpr uint32_t k_pf_write = 1u << 1;

constexpr uint16_t k_et_exec = 2;
constexpr uint16_t k_em_x86_64 = 62;

uint64_t page_align_down(uint64_t v) { return v & ~static_cast<uint64_t>(kern::mm::k_page_size - 1); }
uint64_t page_align_up(uint64_t v) {
    return page_align_down(v + kern::mm::k_page_size - 1);
}

}  // namespace

result<uint64_t, elf_error> load_elf(uint64_t pml4_phys, const uint8_t* elf_data,
                                      uint64_t elf_size) {
    if (elf_size < sizeof(elf64_ehdr)) {
        kern::klog::printf("[elf_loader] truncated(header) elf_size=0x%lx\n",
                     static_cast<unsigned long>(elf_size));
        return result<uint64_t, elf_error>::err(elf_error::truncated);
    }
    elf64_ehdr eh;
    __builtin_memcpy(&eh, elf_data, sizeof(eh));

    if (eh.e_ident[0] != 0x7F || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
        eh.e_ident[3] != 'F') {
        kern::klog::printf("[elf_loader] bad_magic\n");
        return result<uint64_t, elf_error>::err(elf_error::bad_magic);
    }
    if (eh.e_ident[4] != 2) {  // ELFCLASS64
        kern::klog::printf("[elf_loader] unsupported_class=%u\n", eh.e_ident[4]);
        return result<uint64_t, elf_error>::err(elf_error::unsupported_class);
    }
    if (eh.e_machine != k_em_x86_64) {
        kern::klog::printf("[elf_loader] unsupported_machine=%u\n", eh.e_machine);
        return result<uint64_t, elf_error>::err(elf_error::unsupported_machine);
    }
    if (eh.e_type != k_et_exec) {
        // ET_DYN(PIE)은 재배치 처리가 필요해 이 최소 로더의 범위 밖 —
        // init/initrun/link.ld가 항상 ET_EXEC(고정 주소)로 링크한다.
        kern::klog::printf("[elf_loader] unsupported_type=%u\n", eh.e_type);
        return result<uint64_t, elf_error>::err(elf_error::unsupported_type);
    }

    uint64_t ph_bytes = static_cast<uint64_t>(eh.e_phnum) * eh.e_phentsize;
    if (eh.e_phoff > elf_size || elf_size - eh.e_phoff < ph_bytes) {
        kern::klog::printf("[elf_loader] truncated(phdrs) e_phoff=0x%lx ph_bytes=0x%lx elf_size=0x%lx\n",
                     static_cast<unsigned long>(eh.e_phoff), static_cast<unsigned long>(ph_bytes),
                     static_cast<unsigned long>(elf_size));
        return result<uint64_t, elf_error>::err(elf_error::truncated);
    }

    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        elf64_phdr ph;
        __builtin_memcpy(&ph, elf_data + eh.e_phoff + static_cast<uint64_t>(i) * eh.e_phentsize,
                          sizeof(ph));
        if (ph.p_type != k_pt_load) {
            continue;
        }
        if (ph.p_offset > elf_size || elf_size - ph.p_offset < ph.p_filesz) {
            kern::klog::printf("[elf_loader] truncated(seg) p_offset=0x%lx p_filesz=0x%lx elf_size=0x%lx\n",
                         static_cast<unsigned long>(ph.p_offset),
                         static_cast<unsigned long>(ph.p_filesz),
                         static_cast<unsigned long>(elf_size));
            return result<uint64_t, elf_error>::err(elf_error::truncated);
        }
        if (ph.p_filesz > ph.p_memsz) {
            kern::klog::printf("[elf_loader] truncated(filesz>memsz) filesz=0x%lx memsz=0x%lx\n",
                         static_cast<unsigned long>(ph.p_filesz),
                         static_cast<unsigned long>(ph.p_memsz));
            return result<uint64_t, elf_error>::err(elf_error::truncated);
        }

        page_perm perm = page_perm::user;
        if (ph.p_flags & k_pf_write) {
            perm = perm | page_perm::write;
        }
        if (ph.p_flags & k_pf_exec) {
            perm = perm | page_perm::exec;
        }

        uint64_t seg_start = page_align_down(ph.p_vaddr);
        uint64_t seg_end = page_align_up(ph.p_vaddr + ph.p_memsz);
        uint64_t file_start = ph.p_vaddr;
        uint64_t file_end = ph.p_vaddr + ph.p_filesz;

        for (uint64_t page_vaddr = seg_start; page_vaddr < seg_end;
             page_vaddr += kern::mm::k_page_size) {
            auto page = kern::mm::alloc_pages(0, 0);
            if (!page.is_ok()) {
                return result<uint64_t, elf_error>::err(elf_error::out_of_memory);
            }
            uint8_t* page_virt = static_cast<uint8_t*>(kern::mm::phys_to_virt(page.value()));
            __builtin_memset(page_virt, 0, kern::mm::k_page_size);

            uint64_t page_end = page_vaddr + kern::mm::k_page_size;
            uint64_t copy_start = file_start > page_vaddr ? file_start : page_vaddr;
            uint64_t copy_end = file_end < page_end ? file_end : page_end;
            if (copy_start < copy_end) {
                __builtin_memcpy(page_virt + (copy_start - page_vaddr),
                                  elf_data + ph.p_offset + (copy_start - file_start),
                                  copy_end - copy_start);
            }

            auto mapped = map_page(pml4_phys, page_vaddr, page.value(), perm);
            if (!mapped.is_ok()) {
                kern::klog::printf("[elf_loader] map_failed page_vaddr=0x%lx err=%u\n",
                             static_cast<unsigned long>(page_vaddr),
                             static_cast<unsigned>(mapped.error()));
                return result<uint64_t, elf_error>::err(elf_error::map_failed);
            }
        }
    }

    return result<uint64_t, elf_error>::ok(eh.e_entry);
}

}  // namespace kern::arch::x86_64
