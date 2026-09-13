/* higher-half x86-64 커널 링커 스크립트 (SP-8B6B8D25 §5)
 *
 * 부트로더/하이퍼바이저는 커널을 물리 주소 1MiB에 32비트 보호 모드로
 * 올린다(PVH direct boot ABI - 2026-09-14부로 첫 구현 대상, 이전
 * boot.S 헤더 주석 참고). .boot 섹션(PVH 진입 코드)은 그 낮은
 * 물리/가상 주소 그대로 링크해 페이징이 꺼진 상태에서도 실행
 * 가능하게 하고, 나머지(.text/.rodata/.data/.bss)는 상위 절반 가상
 * 주소(KERNEL_VMA)로 링크하되 물리적으로는 .boot 바로 뒤에 이어
 * 붙인다(AT() 로 로드 주소를 낮게 고정) - 커널 자신이 페이지 테이블을
 * 다 갖춘 뒤 상위 주소로 점프해 넘어간다.
 *
 * PHDRS를 직접 쓰는 이유: .note.pvh(PVH ELF Note)가 실제로 PT_NOTE
 * 세그먼트로 나오려면 커스텀 SECTIONS를 쓰는 한 명시적으로 선언해야
 * 한다(기본 스크립트의 자동 추론에 맡기면 조용히 빠질 수 있음).
 */

ENTRY(pvh_start)

KERNEL_LMA = 1M;
KERNEL_VMA = 0xFFFFFFFF80000000;

PHDRS
{
    note   PT_NOTE FLAGS(4);        /* R */
    boot   PT_LOAD FLAGS(5);        /* R+X */
    ktext  PT_LOAD FLAGS(5);        /* R+X */
    krodata PT_LOAD FLAGS(4);       /* R */
    kdata  PT_LOAD FLAGS(6);        /* R+W */
}

SECTIONS
{
    . = KERNEL_LMA;
    kernel_phys_start = .;

    .note.pvh : AT(ADDR(.note.pvh)) { KEEP(*(.note.pvh)) } :note

    .boot ALIGN(4K) : AT(ADDR(.boot))
    {
        *(.boot.text)
        *(.boot.data)
        *(.boot.bss)
    } :boot

    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text .text.*)
    } :ktext

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata .rodata.*)
    } :krodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
    } :kdata

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :kdata

    kernel_end = .;
    /* page_frame_allocator.cpp가 커널 자신의 물리 범위를 예약(usable
     * 메모리에서 제외)하는 데 쓴다 - kernel_end는 higher-half VMA라서
     * 그대로는 물리 주소가 아니다. */
    kernel_phys_end = kernel_end - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
    }
}
