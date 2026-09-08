/*
 * AP(Application Processor) 트램폴린 (docs/plan/smp-fpu-bringup.md §M10,
 * ADR-055). BSP가 부팅 후 smp.cpp에서 이 블록의 바이트를 물리주소
 * AP_TRAMPOLINE_BASE(smp.hpp::k_ap_trampoline_phys, 반드시 이 값과
 * 일치)로 memcpy한 뒤 INIT-SIPI-SIPI를 보낸다 — SIPI 벡터 필드가
 * 8비트라 대상 물리주소가 1MiB 미만·4KiB 정렬이어야 하는데(Intel MP
 * 초기화 절차), 이 커널의 실제 이미지(.boot 포함)는 전부 1MiB 이상에
 * 링크되어 있어(link.ld KERNEL_PHYS_BASE=0x100000) 그 범위 밖에 별도로
 * 마련한 자리다.
 *
 * 이 블록은 "어디에 링크되었는지"와 무관하게 항상 AP_TRAMPOLINE_BASE로
 * 복사된 채로만 실행된다는 전제로 작성한다 — 그래서 이 블록 **내부**의
 * 절대 주소(대상 offset)는 전부 `AP_TRAMPOLINE_BASE + (라벨 -
 * ap_trampoline_start)`로 직접 계산한다(같은 섹션 안 심벌 차이는 실제
 * 링크 위치와 무관하게 항상 올바른 상수다) — 링커가 이 섹션을 실제로
 * 어디에 두는지는 신경 쓰지 않는다. 이 블록 **밖**의 심벌(pml4 등)은
 * 정상적인 절대주소 참조라 그대로 쓴다(32비트 모드에서는 항상
 * 절대주소 — boot.S의 기존 관례와 동일). 64비트 모드에서는 심벌
 * 참조가 기본적으로 %rip 상대 인코딩이 될 수 있어(GAS 64비트 모드
 * 관례) 이 블록 안에서는 위험하므로, 64비트로 들어가는 즉시(단
 * 두 명령) 이 블록을 완전히 벗어나 higher-half의 평범한 코드
 * (ap_entry64_highhalf, 이 파일 하단)로 넘어간다 — 그 지점부터는
 * 실제 링크 주소에서 그대로 실행되므로 %rip 상대 참조도 안전하다.
 */

.set AP_TRAMPOLINE_BASE, 0x8000   /* smp.hpp::k_ap_trampoline_phys와 반드시 일치 */

/* 로컬(임시) GDT 셀렉터 — gdt_selectors.hpp의 실제 커널 GDT와 같은
 * 인코딩·순서를 그대로 복제했다(boot.S의 gdt64_start 참고) — 64비트
 * 진입 직후 실제 GDT로 다시 lgdt하기 전까지만 쓴다. */
.set LOCAL_SEL_CODE32, 0x08
.set LOCAL_SEL_DATA32, 0x10
.set LOCAL_SEL_CODE64, 0x18
.set LOCAL_SEL_DATA64, 0x20

.set CR4_PAE, 0x20
.set CR0_PE, 0x1
.set CR0_PG, 0x80000000
.set MSR_EFER, 0xC0000080
.set EFER_LME_NXE, 0x900

.section .rodata.ap_trampoline, "a"
.balign 16
.global ap_trampoline_start
ap_trampoline_start:

.code16
ap16_entry:
    cli
    mov %cs, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $0x1000, %sp   /* 이 페이지(4KiB) 맨 끝 — 아래 어떤 명령도 실제로
                         * 스택을 쓰지 않지만 관례적 안전장치. */

    /* 로컬 GDTR을 레지스터 간접 addressing으로 로드한다 — 심벌을
     * 직접 메모리 오퍼랜드로 쓰면 GAS가 계산하는 변위가 "이 섹션이
     * 실제로 링크된 주소"를 기준으로 할 수 있어(이 블록은 어차피
     * 그 주소에서 실행되지 않는다) 위험하다. bx에 상수(같은 섹션 안
     * 차이값)를 넣고 [ds:bx]로 접근하면 항상 AP_TRAMPOLINE_BASE
     * 기준으로 정확하다. */
    mov $(gdt_ptr_local - ap_trampoline_start), %bx
    lgdt (%bx)

    mov %cr0, %eax
    or $CR0_PE, %eax
    mov %eax, %cr0

    ljmp $LOCAL_SEL_CODE32, $(AP_TRAMPOLINE_BASE + ap32_entry - ap_trampoline_start)

.code32
ap32_entry:
    mov $LOCAL_SEL_DATA32, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    /* pml4는 이 블록 밖의 진짜 심벌(boot.S) — 32비트 모드의 심벌 참조는
     * 항상 절대주소라 안전하다(boot.S의 _start32가 이미 같은 방식으로
     * low_pd 등을 참조한다). BSP가 이미 이 페이지테이블을 완전히
     * 구성해 둔 뒤라(kernel_main이 AP를 깨우는 시점은 그보다 한참
     * 뒤) physmap·커널 이미지·저지대 항등 매핑이 모두 준비돼 있다 —
     * AP는 새로 아무것도 만들지 않고 그대로 재사용한다. */
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

    ljmp $LOCAL_SEL_CODE64, $(AP_TRAMPOLINE_BASE + ap64_entry - ap_trampoline_start)

.code64
ap64_entry:
    /* 이 블록(재배치된 저지대 사본)에서 할 일은 여기까지다 — 심벌
     * 절대주소를 movabs 즉시값으로 얻어(위치 무관, boot.S의 _start64가
     * kernel_main으로 넘어가는 것과 같은 기법) higher-half의 평범한
     * 코드로 완전히 벗어난다. */
    movabs $ap_entry64_highhalf, %rax
    jmp *%rax

/* gdt_local/gdt_ptr_local은 ap_trampoline_end **이전**에 와야 한다 —
 * smp.cpp가 [ap_trampoline_start, ap_trampoline_end) 범위만 물리주소
 * AP_TRAMPOLINE_BASE로 복사하므로, 이 뒤에 두면 ap16_entry의 lgdt가
 * 읽는 GDTR/GDT 자체가 목적지 페이지에 복사되지 않은 채(전부 0인
 * 미기록 메모리) 남아 lgdt가 base=0/limit=0을 적재해버린다 — 실제로
 * 이 순서 실수로 AP가 lgdt 직후의 ljmp에서 곧바로 #GP→#DF→트리플폴트로
 * 죽는 것을 QEMU 트레이스(MINICORE_QEMU_TRACE=1)로 확인했다. */
.balign 8
gdt_local:
    .quad 0x0000000000000000   /* null */
    .quad 0x00CF9A000000FFFF   /* code32, flat */
    .quad 0x00CF92000000FFFF   /* data32, flat */
    .quad 0x00A09A0000000000   /* code64, L=1 */
    .quad 0x0000920000000000   /* data64 */
gdt_local_end:

gdt_ptr_local:
    .word gdt_local_end - gdt_local - 1
    .long AP_TRAMPOLINE_BASE + gdt_local - ap_trampoline_start

.global ap_trampoline_end
ap_trampoline_end:

/*
 * higher-half 착지점 — 여기부터는 평범한 커널 코드다(실제 링크 주소
 * 그대로 실행되므로 %rip 상대 참조도 안전). g_ap_boot_stack_top/
 * g_ap_boot_cpu_index는 smp.cpp가 정의하고, BSP가 각 AP를 깨우기
 * 직전에 채워 둔다(smp.cpp의 순차 기동 — 한 번에 AP 하나씩만 대상이라
 * 전역 변수 하나로 충분하다).
 */
.section .text
.global ap_entry64_highhalf
ap_entry64_highhalf:
    mov $LOCAL_SEL_DATA64, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* 진짜 커널 GDT로 다시 적재한다 — 이제부터 이 AP는 BSP와 완전히
     * 같은 GDT/셀렉터를 쓴다(로컬 임시 GDT는 더 이상 참조하지 않는다).
     * gdt64_pointer는 boot.S 심벌 — %rip 상대 참조가 지금은 안전하다
     * (이 함수 자체가 실제 링크 주소에서 실행 중이므로). */
    lgdt gdt64_pointer(%rip)

    movq g_ap_boot_stack_top(%rip), %rsp
    movl g_ap_boot_cpu_index(%rip), %edi

    call ap_main

    /* ap_main은 정상적으로 돌아오지 않는다(내부에서 arch_idle_halt로
     * 끝난다) — 그래도 돌아오면 여기서 멈춘다(관례적 안전장치). */
1:  cli
    hlt
    jmp 1b
