/*
 * initrun 링커 스크립트 (docs/plan/kernel-bootstrap.md M8).
 *
 * 커널(kernel/arch/x86_64/link.ld)과 달리 higher-half가 아니다 —
 * initrun은 평범한 유저 프로세스로, 고정된 낮은 가상주소(ET_EXEC,
 * 재배치 없음)에 링크한다. kernel/arch/x86_64/elf_loader.cpp가 이
 * 최소 로더 하나만 지원한다(동적 링킹/PIE 미지원) — 그래서 여기서
 * ET_EXEC를 강제한다.
 */

ENTRY(_start)

/* ET_EXEC(비-PIE 정적 실행파일)는 링커 스크립트 지시자가 아니라 링크
 * 플래그로 결정된다 — CMakeLists.txt가 -static만 주고 -pie를 주지
 * 않으므로(그리고 common.cmake가 전역으로 -fno-pic를 이미 강제한다)
 * ld.lld는 기본적으로 ET_EXEC를 만든다(elf_loader.cpp가 이것만
 * 지원한다).
 *
 * 0x400000(관례적인 "낮은" 유저 베이스)이 아니라 0x10000000을 쓰는
 * 이유: kernel/arch/x86_64/page_table.cpp의 create_address_space_root()가
 * boot.S의 저지대 항등 매핑(물리/가상 [0, 8MiB), 2MiB 거대 페이지)을
 * 모든 새 주소공간에 그대로 복사한다(GDT가 그 안에 있어서 필요하다,
 * page_table.hpp 상단 주석) — initrun이 0x400000에 링크되면 이 범위와
 * 정확히 겹쳐, ELF 로더의 map_page()가 이미 존재하는 2MiB 거대 페이지
 * PDE를 다음 레벨 테이블로 착각해 매핑이 깨진다(직접 겪은 문제 —
 * map_page 자신은 거대 페이지 분할을 지원하지 않는다, 이 마일스톤
 * 범위 밖). 8MiB보다 훨씬 위인 이 주소는 그 범위와 절대 겹치지 않는다. */
INITRUN_BASE = 0x0000000010000000;

SECTIONS
{
    . = INITRUN_BASE;

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
