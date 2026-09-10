/*
 * minicore SDK 공용 링커 스크립트 (real-libc-syscall-layer.md §M38,
 * docs/design/build-system.md ADR-190). 저장소 안 userland/musl-hello/
 * link.ld(§M28)를 그대로 가져왔다 — 그 파일 자체가 musl-hello에만
 * 특화된 내용이 없었다(같은 자리·같은 관례로 저장소 밖 어떤
 * minicore 프로그램에도 그대로 쓸 수 있다).
 *
 * ET_EXEC, 베이스 주소 0x10000000(kernel/arch/x86_64/page_table.cpp의
 * 저지대 항등 매핑 [0, 8MiB)와 겹치지 않는 자리, docs/design/
 * kernel-memory.md ADR-160의 고정 가상주소 슬롯 배치를 따른다).
 *
 * musl의 crt1.c(__libc_start_main.c)가 전역 생성자 배열(.init_array/
 * .fini_array)의 경계를 __init_array_start/__init_array_end 심볼로
 * 참조한다 — C만 쓰는 프로그램은 실제로 비어 있어도 심볼 자체는
 * 링크에 필요해서 이 두 섹션을 명시적으로 정의하고
 * PROVIDE_HIDDEN으로 경계를 잡는다.
 */

ENTRY(_start)

MINICORE_SDK_PROGRAM_BASE = 0x0000000010000000;

SECTIONS
{
    . = MINICORE_SDK_PROGRAM_BASE;

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
