/*
 * M21 선점 검증용 데모 두 프로세스(busy/counter)의 링커 스크립트.
 * init/initrun/link.ld와 완전히 같은 이유로 같은 값을 쓴다 — 각
 * 프로세스는 자기만의 주소공간에서 실행되므로 링크 베이스가 겹쳐도
 * (initrun/shell/login과도) 문제되지 않는다.
 */

ENTRY(_start)

PREEMPT_DEMO_BASE = 0x0000000010000000;

SECTIONS
{
    . = PREEMPT_DEMO_BASE;

    .text ALIGN(4K) : {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    .bss ALIGN(4K) (NOLOAD) : {
        *(COMMON)
        *(.bss .bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.*)
    }
}
