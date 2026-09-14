/* 유저랜드 실행 파일 링커 스크립트(SP-8B6B8D25 §5-A 확정)
 *
 * 코드 공간은 0x400000(4MiB)부터 시작 - 비-PIE 고정 베이스(v1은
 * ET_DYN/재배치 지원 안 함, minicore/libs/libelf가 이 전제로 파싱).
 * 스택은 프로세스 로더가 유저 주소공간(128TiB, §5-3) 끝점부터 별도로
 * 매핑한다 - 이 링커 스크립트는 코드/데이터/BSS만 다룬다.
 *
 * PHDRS로 R+X(.text)/R(.rodata)/R+W(.data+.bss) 세그먼트를 분리한다 -
 * 커널 링커 스크립트(minicore/arch/x86_64/linker.ld)와 같은 이유
 * (전부 하나의 RWX 세그먼트로 뭉개지 않음). NX(실행 금지) 비트는
 * 아직 Paging에 없어 커널 로더가 실제로 그 권한 차이를 전부 강제하진
 * 못하지만(PN-58501EAA 참고), ELF 자체는 표준대로 분리해 둔다.
 */

ENTRY(_start)

USERLAND_BASE = 0x400000;

PHDRS
{
    utext   PT_LOAD FLAGS(5);   /* R+X */
    urodata PT_LOAD FLAGS(4);   /* R */
    udata   PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS
{
    . = USERLAND_BASE;

    .text ALIGN(4K) : { *(.text .text.*) } :utext

    .rodata ALIGN(4K) : { *(.rodata .rodata.*) } :urodata

    .data ALIGN(4K) : { *(.data .data.*) } :udata

    .bss ALIGN(4K) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :udata

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
