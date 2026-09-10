/*
 * userland/echo 링커 스크립트 (docs/plan/musl-userland-porting.md
 * §M52, ADR-221). userland/musl-hello/link.ld와 완전히 같은 이유로
 * 같은 값을 쓴다.
 */

ENTRY(_start)

ECHO_BASE = 0x0000000010000000;

SECTIONS
{
    . = ECHO_BASE;

    .text ALIGN(4K) : {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    .init_array ALIGN(8) : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(.init_array .init_array.*))
        PROVIDE_HIDDEN(__init_array_end = .);
    }

    .fini_array ALIGN(8) : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(.fini_array .fini_array.*))
        PROVIDE_HIDDEN(__fini_array_end = .);
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
