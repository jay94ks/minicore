/*
 * x86_64 Multiboot2 진입 스텁 (docs/spec/boot.md §1.1,
 * docs/spec/virtual-memory-layout.md §2.1).
 *
 * 흐름: _start32(32비트 보호모드, 부트로더가 진입, EAX/EBX에 Multiboot2
 * 매직/info 포인터) — EAX/EBX를 mb2_magic/mb2_info_addr(.boot.bss)에
 * 저장 — 자체 GDT 적재, 최소 항등 매핑(물리 [0,8MiB)) 구성, long mode
 * 진입 — ljmp로 _start64(64비트)로 전환 — 임시 낮은 스택으로
 * boot_setup_paging()(paging_setup.cpp, physmap+커널 이미지 higher-half
 * 매핑 구성) 호출 — 커널 이미지 안의 진짜 스택으로 전환 후
 * kernel_main()(higher-half)로 점프. kernel_main이 mb2_magic/
 * mb2_info_addr을 읽어 boot_info를 구성한다(boot_info_x86_64.cpp,
 * boot.md §2~3). 1단계의 임시 항등 매핑은 정리하지 않고 그대로
 * 둔다(ADR-078).
 */

.section .multiboot2_header, "a"
.balign 8
mb2_header_start:
    .long 0xE85250D6                                            /* magic */
    .long 0                                                     /* architecture: i386 */
    .long mb2_header_end - mb2_header_start                     /* header_length */
    .long -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start)) /* checksum */
    .balign 8
    .word 0                                                     /* end tag: type */
    .word 0                                                     /* end tag: flags */
    .long 8                                                     /* end tag: size */
mb2_header_end:

/* ---- GDT: null, code32, data32, code64, data64, user_data64, user_code64
 * (docs/spec 없음 — 부트 스텁 내부 구현 세부, x86_64 아키텍처 매뉴얼의
 * 세그먼트 디스크립터 인코딩을 그대로 따른다).
 *
 * user_data64/user_code64(0x28/0x30, DPL=3)는 M8(kernel-bootstrap.md,
 * boot.md §4)이 유저모드로 진입할 때 IRETQ/SYSRET가 쓴다 — 부팅 시점엔
 * 아무도 이 두 엔트리를 적재하지 않는다(harmless). 순서가 중요하다:
 * SYSRET(64비트)는 CS = STAR[63:48]+16, SS = STAR[63:48]+8로 계산하므로
 * "data 먼저, 그 바로 다음이 code" 순서를 지켜야 한다(Intel SDM의 잘
 * 알려진 SYSRET 규약) — syscall.cpp가 STAR를 이 배치에 맞춰 설정한다.
 * ---- */
.section .boot.rodata, "a"
.balign 8
gdt64_start:
    .quad 0x0000000000000000   /* 0x00: null */
    .quad 0x00CF9A000000FFFF   /* 0x08: code32, flat, base=0 limit=4G */
    .quad 0x00CF92000000FFFF   /* 0x10: data32, flat, base=0 limit=4G */
    .quad 0x00A09A0000000000   /* 0x18: code64, L=1 */
    .quad 0x0000920000000000   /* 0x20: data64 */
    .quad 0x0000F20000000000   /* 0x28: user_data64, DPL=3 */
    .quad 0x00A0FA0000000000   /* 0x30: user_code64, DPL=3, L=1 */
gdt64_end:

.global gdt64_pointer   /* M10(ap_trampoline.S) — AP가 64비트 진입 후 이 진짜 GDT로 다시 적재한다. */
gdt64_pointer:
    .word gdt64_end - gdt64_start - 1
    .quad gdt64_start

/* Xen/PVH ELF Note (XEN_ELFNOTE_PHYS32_ENTRY=18) — QEMU 개발 편의용
 * 직접 부팅 경로. QEMU 내장 -kernel 로더는 Multiboot2도, 64비트
 * ELF(EM_X86_64)도 지원하지 않는다(둘 다 요구하는 이 커널은 그 경로로
 * 못 띄운다) — 대신 PVH 진입 규약(32비트 보호모드, 페이징 꺼짐, 플랫
 * 세그먼트, GDT/IDT/스택은 커널이 직접 구성)을 지원하며 _start32가
 * 이미 정확히 그 상태를 가정하고 작성되어 있어 그대로 재사용 가능하다.
 * 진짜 부팅 경로(GRUB Multiboot2, ADR-017)는 위 멀티부트2 헤더가
 * 그대로 맡는다 — 이 노트는 QEMU 검증 전용이다(ADR-114). */
.section .note.xen_pvh, "a", @note
.balign 4
    .long 4                      /* namesz: "Xen\0" */
    .long 4                      /* descsz: 32비트 진입 물리주소 */
    .long 18                     /* type: XEN_ELFNOTE_PHYS32_ENTRY */
    .ascii "Xen\0"
    .long _start32

.set SEL_CODE32, 0x08
.set SEL_DATA32, 0x10
.set SEL_CODE64, 0x18
.set SEL_DATA64, 0x20
.set SEL_USER_DATA64, 0x28  /* M8 — usermode.S/syscall.cpp가 쓴다. */
.set SEL_USER_CODE64, 0x30

/* PAGE_PRESENT_RW(중간 레벨: pml4[0], low_pdpt[0])는 USER 비트(0x04)도
 * 켠다 — x86 페이징은 유저모드 접근을 허용하려면 "모든" 레벨이 U/S
 * 비트를 갖고 있어야 한다(AND 관계). M8 전까지는 이 저지대 항등
 * 매핑이 부팅 시점 페이지테이블 구성용(커널 전용)이라 필요 없었지만,
 * page_table.cpp의 create_address_space_root()가 GDT 접근을 위해
 * pml4[0]을 모든 새 주소공간에 공유해야 하게 되면서(M8) 그 아래
 * 새로 만들어지는 유저 매핑(예: ELF 로더가 매핑하는 initrun 코드
 * 페이지)이 이 두 중간 레벨 때문에 막히는 문제를 실제로 겪었다(리프
 * PTE 자체는 USER 비트가 정확히 켜져 있어도, 부모 레벨이 없으면
 * 전체 경로가 supervisor 전용으로 취급된다) — low_pd[0..3](물리
 * [0,8MiB) 항등 매핑 리프 자신)은 이 상수를 안 쓰므로(PAGE_PRESENT_RW_PS)
 * 여전히 supervisor 전용으로 남는다, 의도한 그대로. */
.set PAGE_PRESENT_RW, 0x07
.set PAGE_PRESENT_RW_PS, 0x83
.set CR4_PAE, 0x20
.set CR0_PG, 0x80000000
.set MSR_EFER, 0xC0000080
.set EFER_LME_NXE, 0x900   /* bit8(LME) | bit11(NXE) */

.section .boot.text, "ax"
.code32
.global _start32
.type _start32, @function
_start32:
    cli

    /* boot.md §1.1: 부트로더가 EAX=매직/EBX=Multiboot2 info 물리주소로
     * 진입한다(PVH 경로는 이 매직과 절대 일치하지 않는다, ADR-114) —
     * kernel_main(boot_info_x86_64.cpp)이 나중에 읽을 수 있도록 값이
     * 지워지기 전에 저장해 둔다. GDT/세그먼트 재적재는 AX만 건드리지만
     * 그래도 가장 먼저, 다른 어떤 것도 eax/ebx를 쓰기 전에 저장한다. */
    movl %eax, mb2_magic
    movl %ebx, mb2_info_addr

    /* pml4/low_pdpt/low_pd(.boot.bss, 연속 배치)를 명시적으로 0으로
     * 채운다. NOBITS 섹션은 ELF 관례상 로더가 0으로 채워 줘야 하지만,
     * 이 커널을 실제로 띄우는 qboot.rom(PVH 직접 부팅 경로, ADR-114)이
     * 그 관례를 지키는지는 보장할 수 없다 — 실제로 M8에서 low_pd의
     * (아래서 명시적으로 채우는 [0,3] 밖) 미기록 엔트리에 쓰레기 값이
     * 남아 있어, 새 주소공간을 만들 때 그 쓰레기를 유효한 다음 레벨
     * 페이지테이블 포인터로 오인하는 문제를 실제로 겪었다(ELF 로더가
     * 4KiB 페이지를 매핑하려는 가상주소가 low_pd의 명시적으로 채운
     * [0,3] 범위 밖에 있어서 처음 노출됐다) — 그래서 이 가정 자체를
     * 없앤다. */
    mov $pml4, %edi
    xor %eax, %eax
    mov $(3 * 4096 / 4), %ecx
    rep stosl

    lgdt (gdt64_pointer)

    mov $SEL_DATA32, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    /* low_pd[0..3] = 물리 [0, 8MiB)에 대한 2MiB 항등 매핑 4개. */
    mov $low_pd, %edi
    mov $PAGE_PRESENT_RW_PS, %eax
    mov $4, %ecx
1:
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b

    /* low_pdpt[0] -> low_pd */
    mov $low_pdpt, %edi
    mov $low_pd, %eax
    or $PAGE_PRESENT_RW, %eax
    mov %eax, (%edi)
    movl $0, 4(%edi)

    /* pml4[0] -> low_pdpt */
    mov $pml4, %edi
    mov $low_pdpt, %eax
    or $PAGE_PRESENT_RW, %eax
    mov %eax, (%edi)
    movl $0, 4(%edi)

    mov $pml4, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $CR4_PAE, %eax
    mov %eax, %cr4

    mov $MSR_EFER, %ecx
    rdmsr
    or $EFER_LME_NXE, %eax
    wrmsr

    mov %cr0, %eax
    or $CR0_PG, %eax
    mov %eax, %cr0

    ljmp $SEL_CODE64, $_start64

.code64
.global _start64
.type _start64, @function
_start64:
    mov $SEL_DATA64, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    /* boot_setup_paging() 호출 전용 임시 스택 — 아직 higher-half가
     * 매핑되지 않았으므로 .boot 안의 낮은 스택을 쓴다. */
    movabs $low_boot_stack_top, %rsp

    call boot_setup_paging

    /* 이제 커널 이미지(.text 이하)가 higher-half에 매핑되었다 —
     * 진짜 스택으로 전환하고 kernel_main으로 점프한다. */
    movabs $kernel_boot_stack_top, %rsp
    movabs $kernel_main, %rax
    jmp *%rax

.section .boot.bss, "aw", @nobits
.balign 4
.global mb2_magic
mb2_magic:
    .skip 4
.global mb2_info_addr
mb2_info_addr:
    .skip 4

.balign 4096
.global pml4
pml4:
    .skip 4096
.global low_pdpt   /* M12(ADR-145) — page_table.cpp::create_address_space_root()가
                       [0,8MiB) 항등 매핑 내용만 복사해 가려고 직접 읽는다. */
low_pdpt:
    .skip 4096
.global low_pd
low_pd:
    .skip 4096
.balign 16
low_boot_stack:
    .skip 4096
low_boot_stack_top:

/* kernel_main용 진짜 스택 — 커널 이미지(.bss)에 위치해 higher-half
 * 매핑이 끝난 뒤에만 유효하다. 정식 스레드 스택(2GiB 슬롯 영역)은
 * M5에서 도입된다 — 이것은 그 전까지 쓰는 부트스트랩 전용 스택이다. */
.section .bss
.balign 16
.global kernel_boot_stack
kernel_boot_stack:
    .skip 16384
kernel_boot_stack_top:
