/*
 * userland/musl-hello 링커 스크립트 (real-libc-syscall-layer.md §M28).
 *
 * init/initrun/link.ld과 같은 이유로 ET_EXEC, 같은 베이스 주소
 * (0x10000000 — kernel/arch/x86_64/page_table.cpp의 저지대 항등 매핑
 * [0, 8MiB)와 겹치지 않는 자리, 그 파일 상단 주석 참고).
 *
 * initrun/서버들과 다른 점: musl의 crt1.c(__libc_start_main.c)가
 * 전역 생성자 배열(.init_array/.fini_array)의 경계를
 * __init_array_start/__init_array_end 심볼로 참조한다(이 프로그램은
 * C뿐이라 실제로는 비어 있지만, 심볼 자체는 링크에 필요하다) — 그래서
 * 이 두 섹션을 명시적으로 정의하고 PROVIDE_HIDDEN으로 경계를 잡는다.
 */

ENTRY(_start)

MUSL_HELLO_BASE = 0x0000000010000000;

SECTIONS
{
    . = MUSL_HELLO_BASE;

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
