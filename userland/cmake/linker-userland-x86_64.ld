/* 유저랜드 실행 파일 링커 스크립트(SP-8B6B8D25 §5-A 확정)
 *
 * 코드 공간은 0x400000(4MiB)부터 시작 - 비-PIE 고정 베이스(v1은
 * ET_DYN/재배치 지원 안 함, minicore/libs/libelf가 이 전제로 파싱).
 * 스택은 프로세스 로더가 유저 주소공간(128TiB, §5-3) 끝점부터 별도로
 * 매핑한다 - 이 링커 스크립트는 코드/데이터/BSS만 다룬다.
 *
 * PHDRS로 R+X(.text)/R(.rodata)/R+W(.data+.bss) 세그먼트를 분리한다 -
 * 커널 링커 스크립트(minicore/boot/x86_64/linker.ld, 2026-09-24부로
 * 구 minicore/arch/x86_64/linker.ld)와 같은 이유
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
    utls    PT_TLS  FLAGS(4);   /* R - PN-22E5E9E7 항목5, 커널 링커
                                   스크립트(minicore/boot/x86_64/linker.ld)
                                   §4.2와 정확히 같은 관례 - .tdata/.tbss는
                                   :udata에도 동시에 속한다(PT_LOAD가 실제
                                   로드, PT_TLS는 그 안에서 TLS 템플릿
                                   범위만 가리키는 메타데이터) */
}

SECTIONS
{
    . = USERLAND_BASE;

    .text ALIGN(4K) : { *(.text .text.*) } :utext

    .rodata ALIGN(4K) : { *(.rodata .rodata.*) } :urodata

    .data ALIGN(4K) : { *(.data .data.*) } :udata

    /* thread_local 템플릿(PN-22E5E9E7 항목5, SP-29D652AA §5.1) -
     * 커널 쪽 kTlsTemplateStart/kTlsTemplateTdataEnd/kTlsTemplateEnd와
     * 같은 경계 심볼 이름을 쓰되, 이 파일은 유저 실행 파일 하나당
     * 독립적으로 링크되므로(각자 별도 ELF) 심볼 충돌이 없다 - 커널이
     * 이 심볼을 직접 참조하지 않고(elf::Image가 PT_TLS 세그먼트
     * 헤더의 vaddr/filesz/memsz만 읽음, process.cpp 참고) 오직 이
     * 바이너리 자신의 링크 결과를 검증할 때(readelf 등)만 쓰는
     * 진단용 심볼이다.
     *
     * [실측 정정, 2026-09-18] `ALIGN(8)`(TLS 변수 자체에 필요한 정렬)
     * 이었다가 `ALIGN(4K)`로 교체 - `.data`가 비어 있으면(devmgr처럼
     * thread_local이 아예 없는 바이너리) lld가 빈 `.data` 출력 섹션을
     * 완전히 드롭해 그 `ALIGN(4K)`가 무효화되는데, 이 `.tdata`는
     * 경계 심볼 대입 때문에 항상(내용이 비어도) 실체화된다 - 그러면
     * `.tdata`가 `:udata` 세그먼트의 사실상 첫 섹션이 되어 그 세그먼트
     * 전체의 `p_vaddr`가 페이지 경계 밖(직전 `.text` 세그먼트와 같은
     * 페이지)에서 시작하는 사고가 실측으로 발생했다 - `Process::
     * execImage()`의 `elf::loadIntoAddressSpace()`가 세그먼트를 페이지
     * 단위로 내림/올림해 매핑하는데, 이 어긋난 시작 주소 때문에 `.text`
     * 세그먼트가 이미 매핑한 페이지와 `:udata` 세그먼트가 겹쳐
     * `registerFixedRegion()`이 거부 - execImage 자체가 실패했다(init/
     * devmgr/fs 전부 재현, QEMU 실측). `.tdata`/`.bss`처럼 페이지
     * 정렬을 명시하면 `.data`의 실체화 여부와 무관하게 `:udata`가
     * 항상 새 페이지에서 시작한다. */
    .tdata ALIGN(4K) :
    {
        kTlsTemplateStart = .;
        *(.tdata .tdata.*)
        kTlsTemplateTdataEnd = .;
    } :udata :utls

    .tbss ALIGN(8) :
    {
        *(.tbss .tbss.*)
        kTlsTemplateEnd = .;
    } :udata :utls

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
