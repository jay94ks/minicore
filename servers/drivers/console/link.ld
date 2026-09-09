/*
 * console 드라이버 링커 스크립트 (docs/plan/system-servers-bringup.md §M17).
 * servers/drivers/ps2/link.ld와 완전히 같은 이유로 같은 값을 쓴다.
 */

ENTRY(_start)

CONSOLE_BASE = 0x0000000010000000;

SECTIONS
{
    . = CONSOLE_BASE;

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
