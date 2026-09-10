/*
 * user-service-delegate-test 링커 스크립트 (user-service-manager.md
 * §M43). userland/svcmgr-ctl-test/link.ld와 완전히 같은 이유로 같은
 * 값을 쓴다 — 서로 다른 주소공간이라 기준 주소가 겹쳐도 문제없다.
 */

ENTRY(_start)

USER_SERVICE_DELEGATE_TEST_BASE = 0x0000000010000000;

SECTIONS
{
    . = USER_SERVICE_DELEGATE_TEST_BASE;

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
