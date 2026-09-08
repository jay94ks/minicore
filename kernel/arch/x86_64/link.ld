/*
 * x86_64 커널 링커 스크립트 (docs/spec/virtual-memory-layout.md §2/§3).
 *
 * .boot 섹션(멀티부트2 헤더 + 32비트 진입 스텁 + 임시 항등 매핑용 페이지
 * 테이블)만 VMA==LMA로 저지대 물리주소에 링크한다 — 부트로더가 이
 * 지점으로 점프할 때는 아직 페이징이 꺼져 있으므로 물리주소로만 접근
 * 가능해야 한다. 그 뒤의 모든 섹션(.text 이후)은 higher-half 가상주소
 * (KERNEL_VIRT_OFFSET) 기준으로 링크되지만, 물리적으로는 .boot 바로
 * 뒤에 연속 적재된다(AT() 지시자) — _start64가 이 물리주소 범위를
 * 커널 이미지 영역(0xFFFFFFFF80000000)에 매핑한 뒤에야 실행이 그
 * 쪽으로 넘어간다.
 */

ENTRY(_start32)

KERNEL_VIRT_OFFSET = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE    = 0x00100000; /* 1MiB — 멀티부트 표준 안전 적재 주소 */

/* PHDRS를 명시하는 이유: .note.xen_pvh(ADR-114, QEMU PVH 직접 부팅용)가
 * 정확히 자기 바이트 범위만 담은 PT_NOTE 프로그램 헤더를 가져야
 * QEMU가 이를 찾는다 — PHDRS 없이 기본 자동 생성에 맡기면 다른
 * 섹션과 섞여 PT_NOTE가 아예 만들어지지 않는다. PHDRS를 한 번
 * 쓰면 모든 출력 섹션에 소속 세그먼트를 명시해야 한다. */
PHDRS
{
    boot   PT_LOAD FLAGS(7);   /* 페이징 켜기 전 저지대 코드 — 권한 분리 의미 없음 */
    note   PT_NOTE FLAGS(4);
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R */
    data   PT_LOAD FLAGS(6);   /* R+W */
    bss    PT_LOAD FLAGS(6);   /* R+W */
}

SECTIONS
{
    . = KERNEL_PHYS_BASE;

    .boot ALIGN(4K) : {
        KEEP(*(.multiboot2_header))
        *(.boot.text)
        *(.boot.rodata)
        *(.boot.data)
    } :boot

    .note.xen_pvh ALIGN(4) : {
        KEEP(*(.note.xen_pvh))
    } :boot :note

    .boot.bss ALIGN(4K) (NOLOAD) : AT(ADDR(.boot.bss)) {
        *(.boot.bss)
    } :boot

    . += KERNEL_VIRT_OFFSET;

    _image_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_OFFSET) {
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    } :text

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_OFFSET) {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    } :rodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_OFFSET) {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } :data

    .bss ALIGN(4K) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VIRT_OFFSET) {
        _bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        _bss_end = .;
    } :bss

    _image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.gnu.*)
    }
}
