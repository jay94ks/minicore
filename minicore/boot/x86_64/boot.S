/* x86-64 PVH/multiboot2 공용 진입점 (SP-8B6B8D25 §1, §2-A, §5 구현)
 *
 * 첫 구현 부팅 경로를 multiboot2에서 PVH로 바꿨었다(2026-09-14 - QEMU의
 * -kernel 멀티부트 로더가 (a) Multiboot2를 모르고(MB1만 지원, GRUB만
 * MB2를 이해) (b) 64비트 ELF 자체를 거부해서("give a 32bit one") 우리
 * higher-half 64비트 커널을 못 실었기 때문). PVH는 ELF 클래스를
 * 가리지 않고 QEMU/Xen이 네이티브로 직접 로드하므로 이 문제를 피해
 * 간다. multiboot2(GRUB 전용, DC-427BB6B2 결정)는 이후 별도로 이
 * 파일에 추가했다(PL-FC38956C) - PVH가 이미 검증해 둔 "GDT 적재 ->
 * 페이지 테이블 구성 -> long mode 전환 -> higher-half 점프" 자산을
 * `common_32bit_entry` 서브루틴으로 뽑아 두 진입점이 공유한다.
 *
 * PVH 진입 상태(Xen/QEMU 공통 "PVH direct boot ABI"): 32비트 보호
 * 모드, 페이징 꺼짐, 인터럽트 꺼짐, flat 세그먼트. EBX = struct
 * hvm_start_info(하이퍼바이저가 채움)의 물리 주소. 그 외 레지스터는
 * 정의되지 않음 - 멀티부트처럼 EAX 매직은 없다(struct 안의 magic
 * 필드로 대체 확인 가능, v1은 값만 넘기고 검증은 kMain에서 한다).
 *
 * multiboot2 진입 상태(GRUB): 32비트 보호모드, EAX=0x36d76289(매직),
 * EBX=multiboot2 정보 구조체(태그 기반 가변 길이)의 물리주소. 매직이
 * 안 맞으면(로더가 GRUB가 아니거나 손상됐다는 뜻) 시리얼도 없는 이
 * 시점엔 로그를 남길 방법이 없어 그냥 멈춘다.
 *
 * 두 진입점 모두 EBX(부팅 정보 구조체 물리주소)를 saved_start_info에,
 * 어느 프로토콜인지를 saved_boot_protocol에 저장한 뒤
 * common_32bit_entry로 합류한다 - 거기서: GDT 적재 -> 페이지 테이블
 * 구성(낮은 1GiB identity + 상위 1GiB 지점 higher-half, 둘 다 같은
 * 물리 1GiB를 가리킴) -> PAE/롱모드/페이징 활성화 -> 64비트 코드
 * 세그먼트로 far jump -> higher-half 링크 주소로 다시 점프 -> .bss
 * 제로 클리어 -> kMain(startInfoAddr, bootProtocol) 호출. kMain은
 * 절대 반환하지 않지만, 반환하면 hlt 루프로 정지한다.
 *
 * 링커 스크립트의 전역 ENTRY()/ELF e_entry는 여전히 pvh_start다(Xen/
 * QEMU의 PVH 로더는 e_entry가 아니라 .note.pvh ELF Note로 직접
 * 점프하므로 영향 없음) - GRUB은 e_entry를 그대로 쓰면 pvh_start로
 * 잘못 들어오게 되므로, 아래 멀티부트2 헤더에 Entry Address 태그
 * (type 3)를 넣어 GRUB 한정으로 mb2_start를 쓰게 한다.
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
/* 0=PVH, 1=multiboot2 - kMain의 두 번째 인자로 그대로 넘어간다. */
saved_boot_protocol:
    .long 0

/* [신규, SP-CC2B18C6 §3-3] UEFI 스테이지 로더(minicore/boot/x86_64/
 * uefi/main.cpp, PN-7FBF255A)가 별도 빌드 트리라 이 커널 ELF를
 * 재배치해 실은 뒤 컴파일 타임엔 알 수 없는 물리주소(pml4/
 * long_mode_entry)를 찾아야 한다 - 재배치된 바이트를 스캔해 이
 * 매직으로 위치를 찾고, 그 직후 16바이트를 읽어 진입에 필요한
 * 물리주소 2개를 얻는다. 나머지 4개 테이블(pdpt_low/pd_low/
 * pdpt_high/pd_high)은 여기 저장된 pml4 값 + N*4096으로 계산한다
 * (.boot.bss의 실제 선언 순서/정렬이 정확히 4096바이트 간격이라
 * 성립 - 위 .boot.bss 블록 참고). 이 두 값은 링크 타임 상수라
 * .boot.bss가 아니라 실제 바이트가 존재하는 .boot.data에 있어야
 * UEFI가 memcpy한 결과물 안에 그대로 들어있다. */
.align 8
.global boot_marker_magic
boot_marker_magic:
    .quad 0x544F4F42434E494D    /* "MINCBOOT" ASCII, 리틀엔디안 배치 */
boot_marker_pml4_addr:
    .quad pml4                  /* KERNEL_LMA 기준 물리주소(링크 타임 상수) */
boot_marker_entry_addr:
    .quad long_mode_entry       /* 위와 동일 - SP-CC2B18C6 §3-3 정정대로 가상주소 아님 */
/* [신규, SP-CC2B18C6 §3-3 추가 정정, 2026-09-24] higher_half_entry가
 * kMain에 인자를 넘기려고 읽는 saved_start_info/saved_boot_protocol도
 * 이 심볼과 마찬가지로 "movabs rax, offset saved_start_info"라는
 * 링크 타임 절대주소(=이 필드 자신의 KERNEL_LMA 기준 물리주소, 실행
 * 위치가 재배치돼도 바뀌지 않는 상수)로 읽힌다 - 즉 재배치된 실행
 * 위치가 어디든, 항상 이 "원본(비재배치)" 주소를 읽고 쓴다. 그래서
 * UEFI 스테이지 로더가 physicalBaseDelta/bootProtocol=2를 kMain에
 * 전달하려면, 재배치한 사본이 아니라 바로 이 원본 물리주소에 값을
 * 써야 한다(physicalBaseFound 탐색 시 이 주소가 포함된 원본 이미지
 * 범위[KERNEL_LMA, KERNEL_LMA+imageSpan)와 재배치 목적지가 겹치지
 * 않게 미리 배제해 둬야 그 스크래치 값이 재배치 복사에 덮어써지지
 * 않는다 - main.cpp 참고). saved_boot_protocol은 이 필드 바로 뒤
 * 4바이트(중간에 .align 없음, 위 선언 순서 그대로)라 별도 필드로
 * 마커에 추가하지 않는다. */
boot_marker_saved_start_info_addr:
    .quad saved_start_info
/* [신규, SP-CC2B18C6 §3-3 두 번째 추가 정정, 2026-09-24, 실측으로
 * 발견] long_mode_entry의 첫 줄들("mov ax, 0x10; mov ds, ax; ...")은
 * gdt64가 이미 로드돼 있다는 전제(GRUB/PVH는 enable_long_mode가
 * lgdt [gdt64_ptr]를 미리 해 둠)로 짠 코드다 - UEFI 스테이지 로더는
 * 이 32비트 준비 코드를 전혀 거치지 않고 CR3만 바꾼 뒤 곧장
 * long_mode_entry로 점프하므로, GDTR이 여전히 UEFI 자신의 GDT를
 * 가리킨 채로 "mov ds, ax"(선택자 0x10 = gdt64 안 데이터 세그먼트
 * 인덱스)를 실행하게 되어 #GP(General Protection Fault, UEFI의
 * GDT에서 그 인덱스가 우리가 기대한 것과 다르므로)가 발생한다(실측
 * 확인 - QEMU -d int 로그의 "v=0d e=0010" 항목, 에러코드의 GDT
 * 인덱스 필드가 정확히 2=오프셋0x10과 일치). UEFI 로더가 점프 직전
 * 자체적으로 lgdt를 해야 하므로 gdt64 자신의 주소도 필요하다 -
 * limit(=gdt64_end-gdt64-1=23, 엔트리 3개 고정이라 링크 타임
 * 상수라서 이 마커에 안 실어도 됨)와 달리 base는 재배치 시
 * physicalBaseDelta 보정이 필요한 물리주소라 마커에 추가한다. */
boot_marker_gdt64_addr:
    .quad gdt64

.align 8
gdt32:
    .quad 0                      /* null */
    .quad 0x00CF9A000000FFFF     /* 0x08: code, 32bit, flat */
    .quad 0x00CF92000000FFFF     /* 0x10: data, 32bit, flat */
gdt32_end:
/* ap_trampoline.S(SMP AP 기동, PL-65C20380)도 이 진짜 GDT로 갈아타야
 * 해서 .global이 필요하다 - AP는 자기만의 임시 GDT로 32비트에 막
 * 들어온 뒤 곧바로 이걸로 다시 로드한다. */
.global gdt32_ptr
gdt32_ptr:
    .word gdt32_end - gdt32 - 1
    .long gdt32

.align 8
gdt64:
    .quad 0                      /* null */
    .quad 0x00AF9A000000FFFF     /* 0x08: code, 64bit (L=1) */
    .quad 0x00CF92000000FFFF     /* 0x10: data */
gdt64_end:
.global gdt64_ptr
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

/* ---------------------------------------------------------------- 32비트 진입 코드 */

.section .boot.text, "ax"
.code32

/* ---------------------------------------------------------------- multiboot2 헤더
 *
 * GRUB이 파일 첫 32KiB 안에서 8바이트 정렬로 스캔해 찾는다 - .boot.text
 * 맨 앞(이 섹션에서 코드보다 먼저 나오는 첫 데이터)에 둬서 ELF 헤더/
 * 프로그램 헤더/.note.pvh 다음, 다른 어떤 코드보다도 앞서 파일 앞쪽에
 * 오게 한다. ELF 프로그램 헤더가 이미 올바른 물리 적재 주소(AT())를
 * 담고 있어 GRUB의 ELF 로더가 그대로 쓸 수 있다 - 그래서 별도 Address
 * 태그(type 2)는 필요 없고, e_entry(=pvh_start, PVH 경로 전용) 대신
 * mb2_start로 진입시키는 Entry Address 태그(type 3)만 추가하면 된다.
 */
.align 8
.global multiboot2_header
multiboot2_header:
    .long 0xE85250D6                                    /* magic */
    .long 0                                             /* architecture: i386(보호모드 32비트 진입) */
    .long mb2_header_end - multiboot2_header            /* header_length */
    .long -(0xE85250D6 + 0 + (mb2_header_end - multiboot2_header))  /* checksum */

    .align 8
    .word 3            /* type: entry address */
    .word 0            /* flags */
    .long 12           /* size */
    .long mb2_start    /* entry_addr */

    .align 8
    .word 0            /* type: end */
    .word 0            /* flags */
    .long 8            /* size */
mb2_header_end:

.global pvh_start
pvh_start:
    cli
    mov esp, offset boot_stack_top

    /* struct hvm_start_info 물리 주소(ebx)를 나중에 kMain에 넘기기
     * 위해 메모리에 저장해 둔다 - setup_page_tables가 edi를
     * 스크래치로 쓴다. */
    mov [saved_start_info], ebx
    mov dword ptr [saved_boot_protocol], 0   /* 0 = PVH */
    jmp common_32bit_entry

.global mb2_start
mb2_start:
    cli
    /* GRUB이 EAX에 넣어 주는 매직 - 안 맞으면(로더가 GRUB이 아니거나
     * 손상) 이 시점엔 로그를 남길 방법이 없어 그냥 멈춘다. */
    cmp eax, 0x36d76289
    jne .Lmb2_bad_magic
    mov esp, offset boot_stack_top
    mov [saved_start_info], ebx
    mov dword ptr [saved_boot_protocol], 1   /* 1 = multiboot2 */
    jmp common_32bit_entry
.Lmb2_bad_magic:
    cli
    hlt
    jmp .Lmb2_bad_magic

common_32bit_entry:
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
 *   - 물리/가상 0x0        ~ 0x3FFFFFFF (1GiB) : identity map
 *   - 가상 0xFFFFFFFF80000000 ~ +1GiB          : 위와 같은 물리 1GiB
 * 두 매핑 모두 PD 하나(512개 2MiB 페이지, PS 비트)로 끝낸다 - PT
 * 레벨 불필요. 이 1GiB 한도는 kernel/page_frame_allocator.cpp의
 * kMappedLimit과 반드시 맞춰야 한다(관계도 기록됨).
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

    /* pd_low[0..511] = 2MiB identity 페이지 512개(0 ~ 1GiB) */
    mov edi, offset pd_low
    mov ecx, 512
    mov eax, 0x83
.Lfill_pd_low:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .Lfill_pd_low

    /* pml4[511] -> pdpt_high (0xFFFFFFFF80000000의 PML4 인덱스) */
    mov eax, offset pdpt_high
    or eax, 0x3
    mov [pml4 + 511*8], eax

    /* pdpt_high[510] -> pd_high (0xFFFFFFFF80000000의 PDPT 인덱스) */
    mov eax, offset pd_high
    or eax, 0x3
    mov [pdpt_high + 510*8], eax

    /* pd_high[0..511] = 같은 물리 1GiB(0 ~ 1GiB)를 higher-half로 매핑 */
    mov edi, offset pd_high
    mov ecx, 512
    mov eax, 0x83
.Lfill_pd_high:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .Lfill_pd_high

    ret

/* ap_trampoline.S(SMP AP 기동)도 재사용한다 - 코어별 CR3/CR4/EFER/CR0
 * 설정이라 AP마다 다시 실행해야 하지만, 페이지 테이블 자체(pml4)는
 * BSP가 이미 만들어 둔 걸 그대로 가리키기만 한다(다시 만들지 않음).
 *
 * SSE/FXSAVE 베이스라인도 여기서 코어마다 같이 켠다(SP-83A07867 §8,
 * PN-F258698E 조사 중 실측 확인 - long mode 진입 전까지는 CR4.OSFXSR/
 * OSXMMEXCPT, CR0.MP/EM/NE 중 아무것도 설정돼 있지 않았다). x86_64
 * System V ABI는 SSE2를 기본으로 요구해 clang이 보통의 float/double
 * 연산에도 이미 SSE 명령을 내고 있었는데, CR4.OSFXSR=0인 채로 SSE
 * 명령을 실행하면 즉시 #UD(Invalid Opcode)다 - 지금까지 커널이 SSE를
 * 실제로 밟는 경로가 없어서 드러나지 않았을 뿐이다. CR0.MP=1/EM=0은
 * FPU/SSE 명령이 트랩 없이 실제 실행되게 하고(EM=1이면 모든 FPU 명령이
 * #UD), CR0.TS는 스케줄러의 lazy 저장/복원(scheduler.cpp의 kSyncFpu,
 * #NM 핸들러)이 태스크 디스패치마다 동적으로 켜고 끈다 - 여기서는
 * 항상 0으로 시작한다(TS=1로 시작하면 그 사이 실행되는 어떤 FPU/SSE
 * 명령도 아직 준비 안 된 #NM 핸들러를 건드릴 위험). CR0.NE=1은 레거시
 * #FERR IRQ13 대신 네이티브 #MF 예외 보고를 쓴다(PIC/IRQ13 배선 자체가
 * 없는 이 커널에 맞는 선택). */
.global enable_long_mode
enable_long_mode:
    mov eax, offset pml4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5          /* PAE */
    or eax, 1 << 9          /* OSFXSR - FXSAVE/FXRSTOR 및 SSE 명령 허용 */
    or eax, 1 << 10         /* OSXMMEXCPT - SIMD 부동소수점 예외(#XM) 허용 */
    mov cr4, eax

    mov ecx, 0xC0000080      /* IA32_EFER */
    rdmsr
    or eax, 1 << 8           /* LME */
    wrmsr

    mov eax, cr0
    or eax, 1 << 31          /* PG */
    and eax, ~(1 << 2)       /* EM 해제 - FPU/SSE 명령을 에뮬레이션 트랩하지 않음 */
    or eax, 1 << 1           /* MP - WAIT/FWAIT가 TS를 존중하게 함(lazy FPU 전제조건) */
    or eax, 1 << 5           /* NE - 네이티브 x87 에러 보고(#MF), 레거시 IRQ13 안 씀 */
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

    /* System V AMD64 호출 규약의 첫 인자(rdi)로 부팅 정보 구조체(PVH면
     * hvm_start_info, multiboot2면 그 정보 구조체)의 물리 주소를,
     * 두 번째 인자(rsi)로 어느 프로토콜인지(0=PVH, 1=multiboot2)를
     * 넘긴다 - 32비트 mov는 상위 32비트를 0으로 확장하므로 그대로
     * 유효한 64비트 인자가 된다. */
    movabs rax, offset saved_start_info
    mov edi, [rax]
    movabs rax, offset saved_boot_protocol
    mov esi, [rax]
    call kMain

.Lhang:
    cli
    hlt
    jmp .Lhang
