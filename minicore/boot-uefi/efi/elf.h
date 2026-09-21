// [신규, PN-7FBF255A 체크리스트 5번 - 커널 ELF 로더 2단계: 헤더 파싱]
// efi_main이 ESP에서 읽어 들인 minicore.elf(efi/file.h가 이미 전체를
// 정적 버퍼로 읽어 옴)의 ELF64 헤더/프로그램 헤더 최소 서브셋 - 커널은
// 이미 고정 링크 주소로 빌드돼 있어 재배치/동적 링킹이 전혀 없으므로,
// PT_LOAD 세그먼트를 각자의 p_paddr로 그대로 복사하는 데 필요한
// 필드까지만 선언한다(RM-23F4B687 §4, efi/system_table.h와 동일 원칙).
//
// System V ABI ELF64 명세 그대로 - UEFI 프로토콜과 달리 이 구조체는
// 포인터로 캐스팅해 읽기만 하고 별도 함수 테이블도 없어 오프셋 규칙만
// 명세와 맞으면 된다.
#ifndef MINICORE_BOOT_UEFI_EFI_ELF_H
#define MINICORE_BOOT_UEFI_EFI_ELF_H

struct Elf64Ehdr {
    unsigned char eIdent[16];  // [0..3]=매직(0x7F,'E','L','F'), [4]=클래스(2=64비트), [5]=엔디안(1=리틀)
    unsigned short eType;
    unsigned short eMachine;  // 62 = EM_X86_64
    unsigned int eVersion;
    unsigned long long eEntry;
    unsigned long long ePhoff;  // 프로그램 헤더 테이블의 파일 오프셋
    unsigned long long eShoff;
    unsigned int eFlags;
    unsigned short eEhsize;
    unsigned short ePhentsize;  // 프로그램 헤더 엔트리 하나의 크기(보통 56)
    unsigned short ePhnum;      // 프로그램 헤더 엔트리 개수
    unsigned short eShentsize;
    unsigned short eShnum;
    unsigned short eShstrndx;
};

constexpr unsigned char kElfClass64 = 2;
constexpr unsigned short kElfMachineX86_64 = 62;

// PT_LOAD(=1) - 실제로 메모리에 적재해야 하는 세그먼트. 이 로더가
// 관심 있는 유일한 타입(재배치/동적 링킹 섹션 등은 전부 무시).
constexpr unsigned int kElfProgramTypeLoad = 1;

struct Elf64Phdr {
    unsigned int pType;
    unsigned int pFlags;
    unsigned long long pOffset;  // 파일 안에서 이 세그먼트 데이터의 시작 오프셋
    unsigned long long pVaddr;
    unsigned long long pPaddr;  // 실제로 복사해 넣을 물리주소(boot.S가 배치하는 것과 동일 저지대)
    unsigned long long pFilesz;  // 파일에 실제로 있는 바이트 수(이후 pMemsz까지는 0으로 채움 - .bss)
    unsigned long long pMemsz;
    unsigned long long pAlign;
};

#endif  // MINICORE_BOOT_UEFI_EFI_ELF_H
