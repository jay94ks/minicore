/*
 * ps2 드라이버 링커 스크립트 (docs/plan/system-servers-bringup.md §M14).
 * init/initrun/link.ld·servers/procsrv/link.ld와 완전히 같은 이유로
 * 같은 값을 쓴다 — 서로 다른 주소공간이라 같은 링크 주소(0x10000000)
 * 를 써도 절대 겹치지 않는다.
 */

ENTRY(_start)

PS2_BASE = 0x0000000010000000;

SECTIONS
{
    . = PS2_BASE;

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
