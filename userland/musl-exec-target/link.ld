/*
 * userland/musl-exec-target 링커 스크립트(real-libc-syscall-layer.md
 * §M32) — userland/musl-hello/link.ld와 완전히 동일한 이유·베이스
 * 주소(각자 독립된 주소공간에서 실행되므로 겹칠 일이 없다).
 */

ENTRY(_start)

MUSL_EXEC_TARGET_BASE = 0x0000000010000000;

SECTIONS
{
    . = MUSL_EXEC_TARGET_BASE;

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
