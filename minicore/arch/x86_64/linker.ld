/* higher-half x86-64 커널 링커 스크립트 (SP-8B6B8D25 §5)
 *
 * 부트로더/하이퍼바이저는 커널을 물리 주소 1MiB에 32비트 보호 모드로
 * 올린다(PVH direct boot ABI - 2026-09-14부로 첫 구현 대상, 이전
 * boot.S 헤더 주석 참고). .boot 섹션(PVH 진입 코드)은 그 낮은
 * 물리/가상 주소 그대로 링크해 페이징이 꺼진 상태에서도 실행
 * 가능하게 하고, 나머지(.text/.rodata/.data/.bss)는 상위 절반 가상
 * 주소(KERNEL_VMA)로 링크하되 물리적으로는 .boot 바로 뒤에 이어
 * 붙인다(AT() 로 로드 주소를 낮게 고정) - 커널 자신이 페이지 테이블을
 * 다 갖춘 뒤 상위 주소로 점프해 넘어간다.
 *
 * PHDRS를 직접 쓰는 이유: .note.pvh(PVH ELF Note)가 실제로 PT_NOTE
 * 세그먼트로 나오려면 커스텀 SECTIONS를 쓰는 한 명시적으로 선언해야
 * 한다(기본 스크립트의 자동 추론에 맡기면 조용히 빠질 수 있음).
 *
 * .ap_trampoline16(SMP AP 기동, PL-65C20380)은 실행 주소(VMA)가
 * 물리주소 0x8000이어야 한다 - INIT-SIPI-SIPI의 SIPI 벡터가 "물리
 * 주소/4096"이고, 이 코드는 AP가 16비트 실모드로 막 깨어난 순간
 * 실행할 코드라 링크 주소가 실행 주소와 정확히 같아야 `offset 심볼`
 * 형태를 그대로 쓸 수 있다(boot.S와 동일한 이유). **다만 이 섹션을
 * 물리주소 0x8000에 대응하는 별도 PT_LOAD 세그먼트로 만들면 QEMU의
 * PVH 직접 부팅 로더가 그 세그먼트를 게스트 메모리에 아예 안
 * 올린다(2026-09-14 실측으로 발견 - `xp` 모니터 명령으로 0x8000이
 * 전부 0임을 확인, ELF 파일 자체엔 올바른 바이트가 있었음 - 로더가
 * 저지대/추가 PT_LOAD를 걸러내는 것으로 보임).** 그래서 VMA=0x8000은
 * 유지하되, LMA(파일에 실제로 저장되는 위치)는 이미 정상 로드됨이
 * 검증된 :boot 세그먼트 바로 뒤에 붙인다 - 커널이 부팅 후 이 바이트를
 * memcpy로 진짜 0x8000에 복사한다(Smp::startApCores, AP 기동 직전
 * 1회). 0-2MiB는 kernel/page_frame_allocator.cpp의 kLowReservedEnd가
 * kernel_phys_start/End와 무관하게 이미 통째로 예약해 두므로(BIOS
 * 영역 + 커널 자신) 복사 대상 0x8000 자체는 별도 예약이 필요 없다.
 */

ENTRY(pvh_start)

KERNEL_LMA = 1M;
KERNEL_VMA = 0xFFFFFFFF80000000;
AP_TRAMPOLINE_VADDR = 0x8000;

PHDRS
{
    note    PT_NOTE FLAGS(4);       /* R */
    boot    PT_LOAD FLAGS(5);       /* R+X */
    aptramp PT_LOAD FLAGS(5);       /* R+X - VMA(0x8000)와 LMA(:boot 뒤)가 크게 다름(.text 등과 같은 패턴, 방향만 반대) */
    ktext   PT_LOAD FLAGS(5);       /* R+X */
    krodata PT_LOAD FLAGS(4);       /* R */
    kdata   PT_LOAD FLAGS(6);       /* R+W */
}

SECTIONS
{
    . = KERNEL_LMA;
    kernel_phys_start = .;

    .note.pvh : AT(ADDR(.note.pvh)) { KEEP(*(.note.pvh)) } :note

    .boot ALIGN(4K) : AT(ADDR(.boot))
    {
        *(.boot.text)
        *(.boot.data)
        *(.boot.bss)
    } :boot

    /* VMA만 0x8000으로 강제(위 설명 참고) - LMA는 .boot 바로 뒤에
     * 이어붙여 :boot 세그먼트의 일부로 실제로 로드되게 한다. 이 섹션
     * 블록 "안"에서의 "."은 VMA(0x8000쪽) 기준이라 실제 로드 위치를
     * 가리키는 심볼은 블록 밖에서 별도로 계산해야 한다 - 그래서
     * ap_trampoline16_lma/_size를 블록 앞뒤에 둔다(smp.cpp가 이
     * 값들로 진짜 0x8000에 memcpy한다). */
    ap_trampoline16_lma = ADDR(.boot) + SIZEOF(.boot);
    .ap_trampoline16 AP_TRAMPOLINE_VADDR : AT(ap_trampoline16_lma)
    {
        KEEP(*(.ap_trampoline16))
    } :aptramp
    ap_trampoline16_size = SIZEOF(.ap_trampoline16);

    . = ap_trampoline16_lma + ap_trampoline16_size;

    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text .text.*)
    } :ktext

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata .rodata.*)
    } :krodata

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
    } :kdata

    /* [신규, 2026-09-17, x86_64-elf-gcc 크로스컴파일 툴체인 교체(PN-22E5E9E7
     * 대응) 중 실측 발견] 전역 C++ 생성자(.init_array) - 지금까지 clang은
     * self-참조 포인터 초기화(예: libkcont::Node의 `prev = this;`)를
     * 정적 재배치(non-PIE라 링크 타임에 절대주소로 확정 가능)로 처리해
     * .init_array 콜백 자체가 아예 생기지 않았지만, GCC는 같은 패턴을
     * "동적 초기화가 필요함"으로 판단해 .init_array 항목을 만든다 - 이
     * 커널이 지금까지 .init_array를 전혀 실행한 적이 없어서(부팅 시퀀스
     * 어디에도 순회 코드가 없었음) 그 항목들이 그냥 방치돼 해당 전역
     * 객체가 초기화되지 않은 채로(.bss 그대로 0) 남을 위험이 있었다.
     * 링커 스크립트에 출력 섹션 자체가 없었던 게 진짜 원인 - 이제 명시적으로
     * 만들고, kmain.cpp가 부팅 극초반(다른 어떤 코드보다도 먼저) 순회해
     * 호출한다(kRunGlobalConstructors). */
    .init_array ALIGN(8) : AT(ADDR(.init_array) - KERNEL_VMA)
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :kdata

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :kdata

    kernel_end = .;
    /* page_frame_allocator.cpp가 커널 자신의 물리 범위를 예약(usable
     * 메모리에서 제외)하는 데 쓴다 - kernel_end는 higher-half VMA라서
     * 그대로는 물리 주소가 아니다. */
    kernel_phys_end = kernel_end - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
    }
}
