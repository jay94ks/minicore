/*
 * svcmgr-demo-unit 링커 스크립트 (user-service-manager.md §M40).
 * servers/procsrv/link.ld와 완전히 같은 이유로 같은 값을 쓴다 —
 * 서로 다른 주소공간이라 같은 링크 주소(0x10000000)를 써도 절대
 * 겹치지 않는다. elf_loader.cpp가 ET_EXEC만 지원하므로 여기서도
 * 그대로 강제한다.
 */

ENTRY(_start)

SVCMGR_DEMO_UNIT_BASE = 0x0000000010000000;

SECTIONS
{
    . = SVCMGR_DEMO_UNIT_BASE;

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
