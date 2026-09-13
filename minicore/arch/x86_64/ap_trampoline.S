/* SMP AP(BSP 이외 코어) 기동 트램폴린 (PL-65C20380)
 *
 * BSP가 Lapic::sendStartupIpi()로 SIPI를 보내면, AP는 그 벡터*4096
 * 물리주소(예: 0x8000)에서 16비트 실모드로 실행을 시작한다(세그먼트
 * 레지스터 외 나머지는 정의되지 않음). 여기서 하는 일: 최소 임시
 * GDT로 32비트 보호모드 진입 -> boot.S가 이미 만들어 둔 진짜(공유)
 * GDT/페이지테이블로 갈아탐(단, setup_page_tables는 다시 안 부름 -
 * 이미 다른 코어가 쓰고 있는 페이지 테이블을 재구성하면 안 됨) ->
 * long mode 전환(boot.S의 enable_long_mode 재사용, 코어별 CR3/CR4/
 * EFER/CR0 설정이라 매번 실행해야 함) -> higher-half의 kApMain(C++)
 * 호출.
 *
 * ap_trampoline_start(16비트 진입 코드)와 그 임시 GDT만 물리주소
 * 0x8000에 고정 배치해야 한다(linker.ld :aptramp, VMA=LMA=0x8000 -
 * 16비트 실모드는 1MiB 밖을 못 가리키므로 이 임시 GDT는 boot.S의
 * 진짜 gdt32_ptr(1MiB 근처, 실모드에서 안 닿음)를 대신한다). 일단
 * 32비트 보호모드에 들어가면 flat 세그먼트라 더 이상 1MiB 제약이
 * 없으므로, 나머지(32비트 스텁/64비트 진입)는 평범한 .boot.text/
 * .text에 둬도 된다(link-time 주소를 그대로 오프셋으로 참조 가능).
 */

.intel_syntax noprefix

/* ---------------------------------------------------------------- 16비트 실모드 진입(물리 0x8000 고정) */

.section .ap_trampoline16, "ax"
.code16
.global ap_trampoline_start
ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    /* AP는 SIPI 직후 ESP/SP가 정의되지 않은 채로 시작한다(실측으로
     * 발견한 진짜 버그, 2026-09-14 - 이걸 안 하면 바로 다음 32비트
     * 스텁의 push+retf가 쓰레기 스택에 접근해 엉뚱한 곳으로 튀거나
     * 조용히 멈춘다). ap_boot_stack_top(higher-half, paging 필요)은
     * 아직 못 쓴다 - paging이 켜지기 전까지는 이 낮은 임시 스택만
     * 쓴다(ap_long_mode_entry에서 진짜 스택으로 갈아탐).
     * 16비트 real mode라 SP는 16비트 값만 담을 수 있어서, 이 임시
     * 스택은 반드시 0x8000 트램폴린 자신의 낮은 페이지 안(<0x10000)에
     * 있어야 한다 - boot.S의 gdt32_ptr 등 1MiB대 주소는 여기서 아예
     * 못 가리킨다(같은 이유로 임시 GDT도 따로 둔 것). */
    mov sp, offset ap_temp_stack_top

    lgdt [ap_temp_gdt_ptr]

    mov eax, cr0
    or eax, 1        /* PE */
    mov cr0, eax

    /* 16비트 코드 세그먼트 안에서의 32비트 오프셋 far jmp는 clang
     * 통합 어셈블러가 못 받아들여서(boot.S 기존 코멘트와 동일 문제),
     * opcode(0xEA)+4바이트 오프셋+2바이트 세그먼트를 직접 찍는다.
     * **0x66(오퍼랜드 크기 오버라이드) 프리픽스가 반드시 있어야
     * 한다** - 없으면 CPU가 여전히 16비트 오프셋(2바이트)짜리
     * far jmp로 해석해서 목표 주소가 완전히 엉뚱해진다(실측으로
     * 발견 - 이 프리픽스를 빼먹었을 때 AP가 전혀 응답하지 않았다:
     * 잘못된 세그먼트 셀렉터로 점프해 그 순간 즉시 죽어버림). */
    .byte 0x66, 0xEA
    .long ap_trampoline_32bit
    .word 0x08

.align 16
ap_temp_gdt:
    .quad 0                       /* null */
    .quad 0x00CF9A000000FFFF      /* 0x08: code, 32bit, flat */
    .quad 0x00CF92000000FFFF      /* 0x10: data, 32bit, flat */
ap_temp_gdt_end:
ap_temp_gdt_ptr:
    .word ap_temp_gdt_end - ap_temp_gdt - 1
    .long ap_temp_gdt

/* 16비트 real mode SP에 담을 수 있어야 하므로(<0x10000) 반드시 이
 * 낮은 트램폴린 페이지 안에 둔다 - 순차 기동(AP 하나씩)이라 여러
 * AP가 공유해도 안전하다. */
.align 16
ap_temp_stack_bottom:
    .skip 256
ap_temp_stack_top:

/* ---------------------------------------------------------------- 32비트 스텁(평범한 저지대 .boot.text) */

.section .boot.text, "ax"
.code32
ap_trampoline_32bit:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    /* 16비트에서 넘어온 esp의 상위 16비트는 정의돼 있지 않을 수
     * 있다(리얼모드에선 sp만 건드렸음) - 32비트 전체를 다시 확실히
     * 채운다. */
    mov esp, offset ap_temp_stack_top

    /* 여기부터는 완전한 32비트 flat 모드라 boot.S의 진짜(공유) GDT/
     * 페이지테이블/long mode 루틴을 그대로 오프셋으로 부를 수 있다.
     * setup_page_tables는 일부러 안 부른다 - BSP가 이미 만든 페이지
     * 테이블을 지금 이 순간 다른 코어(BSP 자신 포함)가 쓰고 있어서
     * 다시 만들면(zero_page부터 다시 돎) 그 코어를 망가뜨린다. */
    lgdt [gdt32_ptr]

    mov eax, 0x08
    push eax
    mov eax, offset .Lap_reload_cs32
    push eax
    retf
.Lap_reload_cs32:

    call enable_long_mode

    lgdt [gdt64_ptr]

    mov eax, 0x08
    push eax
    mov eax, offset ap_long_mode_entry
    push eax
    retf

/* ---------------------------------------------------------------- 64비트 낮은 주소 트램폴린(.boot.text 계속, boot.S의 long_mode_entry와 같은 이유)
 *
 * 32비트 스텁이 `mov eax, offset ap_long_mode_entry`(32비트 절대
 * 즉치)로 이 주소를 참조하므로, boot.S의 long_mode_entry와 마찬가지로
 * 낮은 주소(.boot.text, ~1MiB대)에 그대로 둬야 한다 - .text(higher-
 * half, 링크 주소가 0xFFFFFFFF80000000대)로 옮기면 32비트 즉치에
 * 안 들어간다. kApMain 호출은 상관없다 - mcmodel=kernel이 커널을
 * 항상 최상위 2GiB 안에 두는 걸 보장해서, 이 낮은 주소에서 그리로
 * 가는 CALL rel32(상대 변위)는 어차피 2GiB 범위 안에 들어온다. */
.code64
.extern kApMain
.global ap_long_mode_entry
ap_long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* BSP가 이 AP를 깨우기 직전에 채워 둔 스택/코어 인덱스를 읽는다
     * (Smp::startApCores, 순차 기동이라 AP 하나당 이 슬롯을 공유해도
     * 안전함) - .bss 제로 클리어나 kMain 호출은 절대 하지 않는다
     * (BSP가 이미 살아있는 커널 상태를 공유해서 쓰는 중이므로). */
    movabs rax, offset ap_boot_stack_top
    mov rsp, [rax]
    movabs rax, offset ap_boot_index
    mov edi, [rax]
    call kApMain

.Lap_hang:
    cli
    hlt
    jmp .Lap_hang

/* ---------------------------------------------------------------- BSP-AP 핸드오프 스크래치(.boot.data) */

.section .boot.data, "aw"
.align 8
.global ap_boot_stack_top
ap_boot_stack_top:
    .quad 0
.global ap_boot_index
ap_boot_index:
    .long 0
