/* x86-64 Xen PVH 진입점 (SP-8B6B8D25 §1, §2-A, §5 구현)
 *
 * 첫 구현 부팅 경로를 multiboot2에서 PVH로 바꿨다(2026-09-14, 설계자
 * 지시 - DC-427BB6B2/DS-D4E5C451 갱신 예정) - QEMU의 -kernel
 * 멀티부트 로더가 (a) Multiboot2를 모르고(MB1만 지원, GRUB만 MB2를
 * 이해) (b) 64비트 ELF 자체를 거부해서("give a 32bit one") 우리
 * higher-half 64비트 커널을 못 실었다. PVH는 ELF 클래스를 가리지
 * 않고 QEMU/Xen이 네이티브로 직접 로드하므로 이 문제를 피해간다.
 * multiboot2 지원은 나중에 별도로 추가한다(GRUB 체인로더 경유,
 * DC-427BB6B2 결정은 유지).
 *
 * PVH 진입 상태(Xen/QEMU 공통 "PVH direct boot ABI"): 32비트 보호
 * 모드, 페이징 꺼짐, 인터럽트 꺼짐, flat 세그먼트. EBX = struct
 * hvm_start_info(하이퍼바이저가 채움)의 물리 주소. 그 외 레지스터는
 * 정의되지 않음 - 멀티부트처럼 EAX 매직은 없다(struct 안의 magic
 * 필드로 대체 확인 가능, v1은 값만 넘기고 검증은 kMain에서 한다).
 *
 * 여기서 하는 일: 우리 GDT 적재 -> 페이지 테이블 구성(낮은 2MiB
 * identity + 상위 2GiB 지점 higher-half, 둘 다 같은 물리 2MiB를
 * 가리킴) -> PAE/롱모드/페이징 활성화 -> 64비트 코드 세그먼트로
 * far jump -> higher-half 링크 주소로 다시 점프 -> .bss 제로
 * 클리어 -> kMain 호출. kMain은 절대 반환하지 않지만, 반환하면
 * hlt 루프로 정지한다.
 *
 * 커널 그 자체만 이 특권 모드(ring 0)에서 계속 구동된다 - devmgr/
 * fs/net/tty 같은 "커널 서비스"는 전부 유저랜드에서 구동된다
 * (SP-8B6B8D25 §2-A, DS-D4E5C451).
 */

.intel_syntax noprefix

/* ---------------------------------------------------------------- PVH ELF Note
 *
 * Xen의 "Xen" 네임스페이스, type=XEN_ELFNOTE_PHYS32_ENTRY(18) 노트 -
 * desc(4바이트)가 32비트 진입점의 물리 주소다. QEMU/Xen 둘 다 이
 * 노트를 찾아 그 주소로 직접 점프한다(부트로더도, ELF 클래스 제약도
 * 없음). 커스텀 링커 스크립트를 쓰므로 PHDRS에서 PT_NOTE 세그먼트를
 * 명시해야 실제로 실릴 note로 나온다.
 */
.section .note.pvh, "a", @note
.align 4
    .long 4f - 3f       /* namesz */
    .long 2f - 1f        /* descsz */
    .long 18             /* type: XEN_ELFNOTE_PHYS32_ENTRY */
3: .asciz "Xen"
4: .align 4
1: .long pvh_start
2: .align 4

/* ---------------------------------------------------------------- 낮은 주소(.boot) 예약 데이터 */

.section .boot.bss, "aw", @nobits
.align 16
boot_stack_bottom:
    .skip 16384
boot_stack_top:

.align 4096
pml4:
    .skip 4096
pdpt_low:
    .skip 4096
pd_low:
    .skip 4096
pdpt_high:
    .skip 4096
pd_high:
    .skip 4096

.section .boot.data, "aw"
.align 8
/* setup_page_tables/enable_long_mode도 edi를 스크래치로 쓰므로,
 * higher_half_entry에서 kMain에 넘길 때까지 메모리에 따로 저장해
 * 둔다(스택은 higher_half_entry가 rsp를 다시 초기화하므로 못 믿는다). */
saved_start_info:
    .long 0

.align 8
gdt32:
    .quad 0                      /* null */
    .quad 0x00CF9A000000FFFF     /* 0x08: code, 32bit, flat */
    .quad 0x00CF92000000FFFF     /* 0x10: data, 32bit, flat */
gdt32_end:
gdt32_ptr:
    .word gdt32_end - gdt32 - 1
    .long gdt32

.align 8
gdt64:
    .quad 0                      /* null */
    .quad 0x00AF9A000000FFFF     /* 0x08: code, 64bit (L=1) */
    .quad 0x00CF92000000FFFF     /* 0x10: data */
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

/* ---------------------------------------------------------------- 32비트 진입 코드 */

.section .boot.text, "ax"
.code32
.global pvh_start
pvh_start:
    cli
    mov esp, offset boot_stack_top

    /* struct hvm_start_info 물리 주소(ebx)를 나중에 kMain에 넘기기
     * 위해 메모리에 저장해 둔다 - setup_page_tables가 edi를
     * 스크래치로 쓴다. */
    mov [saved_start_info], ebx

    lgdt [gdt32_ptr]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    /* clang의 통합 어셈블러가 far jmp의 즉치(seg:offset) 형식과
     * push 즉치의 32비트 인코딩을 못 받아들여서(16비트로 잘못
     * 재배치하려 함), 레지스터에 실어 push+retf로 우회한다. */
    mov eax, 0x08
    push eax
    mov eax, offset .Lreload_cs32
    push eax
    retf
.Lreload_cs32:

    call setup_page_tables
    call enable_long_mode

    lgdt [gdt64_ptr]
    mov eax, 0x08
    push eax
    mov eax, offset long_mode_entry
    push eax
    retf

/* edi에 4096바이트 0으로 채움(테이블 하나) - edi/esi/eax/ecx 보존 */
zero_page:
    push eax
    push ecx
    push edi
    xor eax, eax
    mov ecx, 1024
    rep stosd
    pop edi
    pop ecx
    pop eax
    ret

/* PML4/PDPT/PD를 구성한다:
 *   - 물리/가상 0x0        ~ 0x1FFFFF (2MiB) : identity map (부트 코드용)
 *   - 가상 0xFFFFFFFF80000000 ~ +2MiB        : 위와 같은 물리 2MiB (커널용)
 * 두 매핑 모두 2MiB 페이지(PS 비트) 하나로 끝낸다 - PT 레벨 불필요.
 */
setup_page_tables:
    mov edi, offset pml4
    call zero_page
    mov edi, offset pdpt_low
    call zero_page
    mov edi, offset pd_low
    call zero_page
    mov edi, offset pdpt_high
    call zero_page
    mov edi, offset pd_high
    call zero_page

    /* pml4[0] -> pdpt_low */
    mov eax, offset pdpt_low
    or eax, 0x3
    mov [pml4], eax

    /* pdpt_low[0] -> pd_low */
    mov eax, offset pd_low
    or eax, 0x3
    mov [pdpt_low], eax

    /* pd_low[0] = 2MiB identity page @0x0 */
    mov dword ptr [pd_low], 0x83

    /* pml4[511] -> pdpt_high (0xFFFFFFFF80000000의 PML4 인덱스) */
    mov eax, offset pdpt_high
    or eax, 0x3
    mov [pml4 + 511*8], eax

    /* pdpt_high[510] -> pd_high (0xFFFFFFFF80000000의 PDPT 인덱스) */
    mov eax, offset pd_high
    or eax, 0x3
    mov [pdpt_high + 510*8], eax

    /* pd_high[0] = 같은 2MiB 물리 프레임 @0x0 */
    mov dword ptr [pd_high], 0x83

    ret

enable_long_mode:
    mov eax, offset pml4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5          /* PAE */
    mov cr4, eax

    mov ecx, 0xC0000080      /* IA32_EFER */
    rdmsr
    or eax, 1 << 8           /* LME */
    wrmsr

    mov eax, cr0
    or eax, 1 << 31          /* PG */
    mov cr0, eax

    ret

/* ---------------------------------------------------------------- 64비트 낮은 주소 트램폴린 */

.code64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* 64비트 모드에서만 64비트 절대주소 즉치값을 쓸 수 있다 - 이
     * movabs로 higher_half_entry의 "진짜"(높은) 링크 주소를 얻어
     * 그리로 넘어간다. */
    movabs rax, offset higher_half_entry
    jmp rax

/* ---------------------------------------------------------------- higher-half 진입(.text, 일반 커널 코드 영역) */

.section .text
.code64
.extern kMain
.global higher_half_entry
higher_half_entry:
    /* C++ 정적 스토리지(전역/정적 변수)는 0 초기화를 보장해야 한다 -
     * 부트로더가 .bss를 0으로 채워준다는 보장이 없으므로 우리가 직접
     * 지운다. */
    movabs rdi, offset __bss_start
    movabs rcx, offset __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb

    /* 낮은 부트 스택은 identity map으로 계속 유효하다 - v1은 이걸
     * 그대로 커널 스택으로 쓴다. */
    movabs rsp, offset boot_stack_top

    /* System V AMD64 호출 규약의 첫 정수 인자(rdi)로 struct
     * hvm_start_info의 물리 주소를 넘긴다 - 32비트 mov는 상위 32비트를
     * 0으로 확장하므로 그대로 유효한 64비트 인자가 된다. */
    movabs rax, offset saved_start_info
    mov edi, [rax]
    call kMain

.Lhang:
    cli
    hlt
    jmp .Lhang
