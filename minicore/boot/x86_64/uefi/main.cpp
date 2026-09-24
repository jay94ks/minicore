// Minicore UEFI 직접 부팅 진입점 - PN-7FBF255A 체크리스트 2/3/4번.
// 체크리스트 4번: EFI_SYSTEM_TABLE에서 ConOut으로 진행 상태를 찍고,
// BootServices->GetMemoryMap을 실제로 두 번 호출한다 - 먼저 크기
// 질의(EFI_BUFFER_TOO_SMALL), 그다음 정적 버퍼(gMemoryMapBuffer)로
// 데이터 모드 호출 - 받은 메모리맵을 DescriptorSize 보폭으로 순회해
// EfiConventionalMemory(일반 사용 가능 RAM) 페이지 수를 합산한다.
// AllocatePool 대신 정적 버퍼를 쓴 이유: 이 질의 하나만을 위해
// EFI_BOOT_SERVICES 서브셋을 AllocatePool까지 더 늘리지 않기 위함
// (RM-23F4B687 §4, PN-7FBF255A 체크리스트 4번이 "AllocatePool 또는
// 정적 버퍼"로 이미 열어 둔 선택지).
//
// GOP(그래픽 출력 프로토콜) 조회와 ExitBootServices() 핸드오프(체크
// 리스트 4번 나머지 + 5번)는 여전히 다음 증분 몫이다.
#include "efi/elf.h"
#include "efi/file.h"
#include "efi/memory.h"
#include "efi/system_table.h"
#include "efi/types.h"

namespace {

// UEFI에는 표준 라이브러리가 없어 64비트 값을 CHAR16 10진 문자열로
// 직접 변환한다 - GetMemoryMap이 돌려준 크기/페이지 수를 사람이 읽을
// 수 있게 ConOut에 찍기 위한 용도뿐이라 이 파일 안에만 필요한 만큼
// 최소로 둔다.
void kFormatUint64(unsigned long long value, CHAR16* out, unsigned int outCapacity) {
    CHAR16 digits[20];
    unsigned int digitCount = 0;
    if (value == 0) {
        digits[digitCount++] = u'0';
    }
    while (value > 0 && digitCount < 20) {
        digits[digitCount++] = static_cast<CHAR16>(u'0' + (value % 10));
        value /= 10;
    }
    unsigned int i = 0;
    for (; i < digitCount && i + 1 < outCapacity; ++i) {
        out[i] = digits[digitCount - 1 - i];
    }
    out[i] = u'\0';
}

void kPrint(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut, const CHAR16* text) {
    conOut->OutputString(conOut, const_cast<CHAR16*>(text));
}

void kPrintUint64(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut, unsigned long long value) {
    CHAR16 buf[24];
    kFormatUint64(value, buf, 24);
    conOut->OutputString(conOut, buf);
}

// 실측 확인(PN-7FBF255A 체크리스트 4번 - 크기 질의 모드 검증)한 필요
// 크기(6528/6336바이트, 부팅마다 약간 다름)보다 5배 가까이 넉넉한
// 정적 버퍼 - AllocatePool 없이도 항상 한 번에 담긴다.
constexpr unsigned long long kMemoryMapBufferCapacity = 32 * 1024;
unsigned char gMemoryMapBuffer[kMemoryMapBufferCapacity];

// [신규, PN-7FBF255A 체크리스트 5번 - 커널 ELF 로더 1단계: 파일
// 읽기만] 실측 크기(3,270,400바이트, 2026-09-22 WSL 빌드)보다
// 넉넉한 정적 버퍼 - AllocatePool/동적 배치 없이 이 단계에서는
// "읽어 들이기"까지만 검증한다. 실제 물리주소 배치(AllocatePages
// 기반 PT_LOAD 세그먼트 복사)는 다음 증분 몫.
constexpr unsigned long long kKernelElfBufferCapacity = 8 * 1024 * 1024;
unsigned char gKernelElfBuffer[kKernelElfBufferCapacity];

// [신규, PN-7FBF255A 체크리스트 5번 - UEFI 스테이지 로더 본체]
// SP-CC2B18C6(approved)가 확정한 재배치 메커니즘 - linker.ld의
// KERNEL_LMA(minicore/boot/x86_64/linker.ld와 반드시 일치)와
// boot.S의 마커(boot_marker_magic 등, §3-3)를 그대로 쓴다.
constexpr unsigned long long kKernelLma = 0x100000ULL;
constexpr unsigned long long kOneGiB = 1ULL << 30;
constexpr unsigned long long kBootMarkerMagic = 0x544F4F42434E494DULL;  // "MINCBOOT"
// ap_trampoline.S(PL-65C20380)가 고정으로 쓰는 물리 페이지 - AP
// 트램폴린과 재배치된 커널 이미지가 겹치면 SMP 부팅이 깨지므로 후보
// physicalBase 탐색에서 이 범위를 피한다(§3-2 4번 "안전 여유" 항목).
constexpr unsigned long long kApTrampolinePhys = 0x8000ULL;
constexpr unsigned long long kApTrampolineSafeLimit = 0x9000ULL;
constexpr unsigned long long kPageSize4K = 4096ULL;
constexpr unsigned long long kPageSize2M = 0x200000ULL;
constexpr unsigned long long kPagePresentWritable = 0x3ULL;
constexpr unsigned long long kPagePresentWritableHuge = 0x83ULL;
constexpr unsigned int kMaxLoadSegments = 8;  // 실측(2026-09-22) 5개 - 여유 포함

struct LoadSegment {
    unsigned long long paddr;   // KERNEL_LMA 기준 물리주소(링크 타임)
    unsigned long long offset;  // ELF 파일 안에서 이 세그먼트 시작 오프셋
    unsigned long long filesz;
    unsigned long long memsz;
};
LoadSegment gLoadSegments[kMaxLoadSegments];
unsigned int gLoadSegmentCount = 0;

// 마커 스캔으로 얻은 원시 값(KERNEL_LMA 기준 물리주소, physicalBaseDelta
// 보정 전) - SP-CC2B18C6 §3-3 [정정] 참고, 가상주소 아님.
bool gMarkerFound = false;
unsigned long long gRawPml4 = 0;
unsigned long long gRawEntry = 0;
// [신규, SP-CC2B18C6 §3-3 추가 정정] higher_half_entry가 kMain 인자를
// 읽을 때 쓰는 "offset saved_start_info" 절대주소 - 재배치 여부와
// 무관하게 항상 이 원본 주소를 가리키므로, 이 주소에 직접 값을
// 써야 kMain에 physicalBaseDelta/bootProtocol이 전달된다(boot.S
// 참고 - 재배치된 사본에 쓰면 안 됨). saved_boot_protocol은 바로
// 뒤 4바이트(중간 정렬 패딩 없음).
unsigned long long gRawSavedStartInfoAddr = 0;

// 이 로더 자신의 실행 위치가 낮은 1GiB 안에 있는지 - CR3 전환 직후에도
// 이 코드 자신이 계속 매핑돼 있어야 하므로(§3-2 5번) 새 페이지
// 테이블을 실제로 쓰기 전에 반드시 확인해야 하는 전제 조건이다.
bool gSelfBelowOneGiB = false;

// UEFI에는 freestanding 표준 라이브러리가 없어(이 파일에 memcpy/memset을
// 링크할 libc가 없음) 필요한 최소 바이트 복사/제로화를 직접 구현한다 -
// 커널 쪽 libkenv와 같은 이유, 이 파일 전용이라 별도 라이브러리로
// 분리하지 않는다.
void kCopyBytes(unsigned char* dest, const unsigned char* src, unsigned long long count) {
    for (unsigned long long i = 0; i < count; ++i) {
        dest[i] = src[i];
    }
}

void kZeroBytes(unsigned char* dest, unsigned long long count) {
    for (unsigned long long i = 0; i < count; ++i) {
        dest[i] = 0;
    }
}

// [TEMP 진단, PN-7FBF255A - ExitBootServices 이후 원인 파악용, 검증
// 끝나면 제거] ConOut은 ExitBootServices 성공 후 무효라 못 쓰지만,
// 이 프로젝트의 run-uefi.sh가 이미 같은 COM1(0x3F8)을 -serial file:
// 로 캡처하고 있어 - BootServices 호출 없이 raw I/O 포트로 직접 쓰면
// 명세 위반 없이 이 시점 이후에도 진단 로그를 남길 수 있다.
inline unsigned char kInb(unsigned short port) {
    unsigned char value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
inline void kOutb(unsigned short port, unsigned char value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
void kRawSerialWrite(const char* s) {
    while (*s) {
        while (!(kInb(0x3FD) & 0x20)) {
        }
        kOutb(0x3F8, static_cast<unsigned char>(*s));
        ++s;
    }
}

}  // namespace

extern "C" EFI_STATUS efi_main(EFI_HANDLE imageHandle, EFI_SYSTEM_TABLE* systemTable) {
    if (!systemTable) {
        return kEfiSuccess;
    }

    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* conOut = systemTable->ConOut;  // 아래에서 반복 참조 - nullptr일 수 있음, 매번 확인
    if (conOut) {
        kPrint(conOut, u"minicore: efi_main reached\r\n");
    }

    if (systemTable->BootServices) {
        // 1) 크기 질의 모드(UEFI 명세 그대로) - MemoryMapSize를
        // 실제보다 작게(0으로) 넘기면 항상 EFI_BUFFER_TOO_SMALL을
        // 반환하면서 MemoryMapSize를 "실제 필요한 크기"로 덮어써
        // 돌려준다 - 정적 버퍼가 실제로 충분한지 사전 확인하는
        // 용도로도 재사용한다(버퍼가 작으면 여기서 미리 알 수 있음).
        unsigned long long queriedSize = 0;
        unsigned long long mapKey = 0;
        unsigned long long descriptorSize = 0;
        unsigned int descriptorVersion = 0;
        EFI_STATUS queryStatus =
            systemTable->BootServices->GetMemoryMap(&queriedSize, nullptr, &mapKey, &descriptorSize, &descriptorVersion);

        if (conOut) {
            if (queryStatus == kEfiBufferTooSmall) {
                kPrint(conOut, u"minicore: GetMemoryMap size query ok, required=");
                kPrintUint64(conOut, queriedSize);
                kPrint(conOut, u"\r\n");
            } else {
                kPrint(conOut, u"minicore: GetMemoryMap size query unexpected status\r\n");
            }
        }

        // 2) 데이터 모드 - 정적 버퍼(kMemoryMapBufferCapacity)가 위
        // 질의 결과보다 충분히 크다고 확신하지 않고 항상 실측 확인한다.
        if (queryStatus == kEfiBufferTooSmall && queriedSize <= kMemoryMapBufferCapacity) {
            unsigned long long mapSize = kMemoryMapBufferCapacity;
            EFI_STATUS dataStatus = systemTable->BootServices->GetMemoryMap(&mapSize, gMemoryMapBuffer, &mapKey,
                                                                              &descriptorSize, &descriptorVersion);
            if (dataStatus == kEfiSuccess && descriptorSize > 0) {
                // [중요, UEFI 명세] sizeof(EFI_MEMORY_DESCRIPTOR)가
                // 아니라 펌웨어가 돌려준 descriptorSize를 보폭으로
                // 순회해야 한다(efi/memory.h 문서 주석 참고).
                const unsigned long long entryCount = mapSize / descriptorSize;
                unsigned long long conventionalPages = 0;
                for (unsigned long long i = 0; i < entryCount; ++i) {
                    const auto* desc =
                        reinterpret_cast<const EFI_MEMORY_DESCRIPTOR*>(gMemoryMapBuffer + i * descriptorSize);
                    if (desc->Type == kEfiConventionalMemory) {
                        conventionalPages += desc->NumberOfPages;
                    }
                }
                if (conOut) {
                    kPrint(conOut, u"minicore: memory map entries=");
                    kPrintUint64(conOut, entryCount);
                    kPrint(conOut, u" conventionalPages=");
                    kPrintUint64(conOut, conventionalPages);
                    kPrint(conOut, u"\r\n");
                }

                // [신규, PN-7FBF255A 체크리스트 5번 착수 조건] 이 코드
                // 자신의 물리 로드 주소(&efi_main, 코드 섹션 안의 실제
                // 주소)가 낮은 1GiB(boot.S가 커널 이미지를 배치하는
                // identity map 범위)에 들어오는지 실측 확인 - ExitBootServices
                // 이후 우리 자신의 페이지 테이블로 CR3를 전환하는 순간,
                // 그 전환을 수행 중인 바로 이 코드 자신이 새 테이블에서도
                // 계속 인출 가능해야 하므로(같은 물리 프레임을 가리키는
                // 매핑이 새 테이블에도 존재해야 함) 이 위치를 미리 알아야
                // 페이지 테이블 설계를 정할 수 있다.
                const auto imageAddr = reinterpret_cast<unsigned long long>(&efi_main);
                for (unsigned long long i = 0; i < entryCount; ++i) {
                    const auto* region =
                        reinterpret_cast<const EFI_MEMORY_DESCRIPTOR*>(gMemoryMapBuffer + i * descriptorSize);
                    const unsigned long long regionEnd = region->PhysicalStart + region->NumberOfPages * 4096ULL;
                    if (imageAddr >= region->PhysicalStart && imageAddr < regionEnd) {
                        if (conOut) {
                            kPrint(conOut, u"minicore: image phys~");
                            kPrintUint64(conOut, imageAddr);
                            kPrint(conOut, u" regionType=");
                            kPrintUint64(conOut, region->Type);
                            kPrint(conOut, u" regionBase=");
                            kPrintUint64(conOut, region->PhysicalStart);
                            kPrint(conOut, u" regionPages=");
                            kPrintUint64(conOut, region->NumberOfPages);
                            kPrint(conOut, u" belowOneGiB=");
                            kPrintUint64(conOut, imageAddr < kOneGiB ? 1 : 0);
                            kPrint(conOut, u"\r\n");
                        }
                        gSelfBelowOneGiB = imageAddr < kOneGiB;
                        break;
                    }
                }
            } else if (conOut) {
                kPrint(conOut, u"minicore: GetMemoryMap data-mode call unexpected status\r\n");
            }
        } else if (conOut && queryStatus == kEfiBufferTooSmall) {
            kPrint(conOut, u"minicore: static memory map buffer too small - increase kMemoryMapBufferCapacity\r\n");
        }

        // [신규, PN-7FBF255A 체크리스트 5번 - 커널 ELF 로더 1단계]
        // bootx64.efi와 minicore.elf는 링크 단계에서 전혀 연결된 적
        // 없는 별개 바이너리라(PE32+/COFF vs ELF, 각자 다른 툴체인) -
        // "GDT/페이지 테이블 전환 후 higher_half_entry로 점프"가
        // 의미를 가지려면 그 전에 이 UEFI 스텁이 커널 ELF 자신을 ESP
        // 에서 직접 읽어 들여야 한다(GRUB/Xen이 multiboot2/PVH 경로에서
        // 대신 해 주던 일). 이번 증분은 "읽어 들이기"까지만 검증하고,
        // PT_LOAD 세그먼트를 실제 물리주소로 배치하는 것(AllocatePages
        // 필요)은 다음 증분 몫이다.
        EFI_LOADED_IMAGE_PROTOCOL* loadedImage = nullptr;
        EFI_STATUS protoStatus = systemTable->BootServices->HandleProtocol(
            imageHandle, const_cast<EFI_GUID*>(&kEfiLoadedImageProtocolGuid), reinterpret_cast<void**>(&loadedImage));
        if (protoStatus != kEfiSuccess || !loadedImage) {
            if (conOut) {
                kPrint(conOut, u"minicore: HandleProtocol(LoadedImage) failed, status=");
                kPrintUint64(conOut, protoStatus);
                kPrint(conOut, u"\r\n");
            }
        } else {
            if (conOut) {
                kPrint(conOut, u"minicore: LoadedImage ok, DeviceHandle=");
                kPrintUint64(conOut, reinterpret_cast<unsigned long long>(loadedImage->DeviceHandle));
                kPrint(conOut, u"\r\n");
            }
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fileSystem = nullptr;
            protoStatus = systemTable->BootServices->HandleProtocol(
                loadedImage->DeviceHandle, const_cast<EFI_GUID*>(&kEfiSimpleFileSystemProtocolGuid),
                reinterpret_cast<void**>(&fileSystem));
            if (protoStatus != kEfiSuccess || !fileSystem) {
                if (conOut) {
                    kPrint(conOut, u"minicore: HandleProtocol(SimpleFileSystem) failed, status=");
                    kPrintUint64(conOut, protoStatus);
                    kPrint(conOut, u"\r\n");
                }
            } else {
                EFI_FILE_PROTOCOL* root = nullptr;
                protoStatus = fileSystem->OpenVolume(fileSystem, &root);
                if (protoStatus != kEfiSuccess || !root) {
                    if (conOut) {
                        kPrint(conOut, u"minicore: OpenVolume failed\r\n");
                    }
                } else {
                    EFI_FILE_PROTOCOL* kernelFile = nullptr;
                    // ESP 루트 바로 아래 - scripts/run-uefi.sh가
                    // \MINICORE.ELF로 배치한다(다음 증분).
                    protoStatus = root->Open(root, &kernelFile, const_cast<CHAR16*>(u"\\MINICORE.ELF"),
                                              kEfiFileModeRead, 0);
                    if (protoStatus != kEfiSuccess || !kernelFile) {
                        if (conOut) {
                            kPrint(conOut, u"minicore: Open(\\MINICORE.ELF) failed, status=");
                            kPrintUint64(conOut, protoStatus);
                            kPrint(conOut, u"\r\n");
                        }
                    } else {
                        unsigned long long readSize = kKernelElfBufferCapacity;
                        protoStatus = kernelFile->Read(kernelFile, &readSize, gKernelElfBuffer);
                        if (protoStatus == kEfiSuccess) {
                            const bool isElf = readSize >= 4 && gKernelElfBuffer[0] == 0x7F &&
                                                gKernelElfBuffer[1] == 'E' && gKernelElfBuffer[2] == 'L' &&
                                                gKernelElfBuffer[3] == 'F';
                            if (conOut) {
                                kPrint(conOut, u"minicore: kernel ELF read bytes=");
                                kPrintUint64(conOut, readSize);
                                kPrint(conOut, u" magicOk=");
                                kPrintUint64(conOut, isElf ? 1 : 0);
                                kPrint(conOut, u"\r\n");
                            }

                            // [신규, PN-7FBF255A 체크리스트 5번 - 커널 ELF 로더
                            // 2단계: 헤더 파싱만, AllocatePages 세그먼트 복사는
                            // 다음 증분] ELF64 헤더 + PT_LOAD 프로그램 헤더를
                            // 읽어 각 세그먼트의 물리 적재 주소(p_paddr)를 로그로
                            // 확인한다 - 아직 실제로 그 주소에 복사하지는 않는다
                            // (지금 그 물리 범위가 UEFI 자신의 usable 메모리와
                            // 겹치지 않는지조차 확인 전이라, 쓰기 전에 먼저 값만
                            // 읽어 검증).
                            if (isElf && readSize >= sizeof(Elf64Ehdr)) {
                                const auto* ehdr = reinterpret_cast<const Elf64Ehdr*>(gKernelElfBuffer);
                                const bool headerOk = ehdr->eIdent[4] == kElfClass64 &&
                                                       ehdr->eMachine == kElfMachineX86_64 && ehdr->ePhoff > 0 &&
                                                       ehdr->ePhnum > 0 &&
                                                       ehdr->ePhoff + static_cast<unsigned long long>(ehdr->ePhnum) *
                                                                          ehdr->ePhentsize <=
                                                           readSize;
                                if (conOut) {
                                    kPrint(conOut, u"minicore: ELF header class64=");
                                    kPrintUint64(conOut, ehdr->eIdent[4] == kElfClass64 ? 1 : 0);
                                    kPrint(conOut, u" machineOk=");
                                    kPrintUint64(conOut, ehdr->eMachine == kElfMachineX86_64 ? 1 : 0);
                                    kPrint(conOut, u" entry=");
                                    kPrintUint64(conOut, ehdr->eEntry);
                                    kPrint(conOut, u" phnum=");
                                    kPrintUint64(conOut, ehdr->ePhnum);
                                    kPrint(conOut, u"\r\n");
                                }
                                if (headerOk) {
                                    for (unsigned short i = 0; i < ehdr->ePhnum; ++i) {
                                        const auto* phdr = reinterpret_cast<const Elf64Phdr*>(
                                            gKernelElfBuffer + ehdr->ePhoff +
                                            static_cast<unsigned long long>(i) * ehdr->ePhentsize);
                                        if (phdr->pType != kElfProgramTypeLoad) {
                                            continue;
                                        }
                                        if (conOut) {
                                            kPrint(conOut, u"minicore: PT_LOAD paddr=");
                                            kPrintUint64(conOut, phdr->pPaddr);
                                            kPrint(conOut, u" vaddr=");
                                            kPrintUint64(conOut, phdr->pVaddr);
                                            kPrint(conOut, u" filesz=");
                                            kPrintUint64(conOut, phdr->pFilesz);
                                            kPrint(conOut, u" memsz=");
                                            kPrintUint64(conOut, phdr->pMemsz);
                                            kPrint(conOut, u"\r\n");
                                        }
                                        // [신규, PN-7FBF255A 체크리스트 5번] 세그먼트를
                                        // 나중에 physicalBase로 복사하려면 이 시점에
                                        // 저장해 둬야 한다 - ExitBootServices 이후엔
                                        // gKernelElfBuffer만 남고 이 파싱 컨텍스트(ehdr/
                                        // phdr 지역 포인터)는 스코프를 벗어난다.
                                        if (gLoadSegmentCount < kMaxLoadSegments) {
                                            gLoadSegments[gLoadSegmentCount].paddr = phdr->pPaddr;
                                            gLoadSegments[gLoadSegmentCount].offset = phdr->pOffset;
                                            gLoadSegments[gLoadSegmentCount].filesz = phdr->pFilesz;
                                            gLoadSegments[gLoadSegmentCount].memsz = phdr->pMemsz;
                                            ++gLoadSegmentCount;
                                        } else if (conOut) {
                                            kPrint(conOut,
                                                   u"minicore: too many PT_LOAD segments - increase kMaxLoadSegments\r\n");
                                        }
                                    }

                                    // [신규, PN-7FBF255A 체크리스트 5번] boot.S §3-3
                                    // 마커 스캔 - 파일 바이트(gKernelElfBuffer) 안에서
                                    // 직접 찾는다. .boot.data는 파일에 실제 바이트가
                                    // 있는 섹션이라(.boot.bss와 달리) 이 시점에 이미
                                    // 값이 존재하고, 마커에 저장된 두 값(pml4/
                                    // long_mode_entry)은 링크 타임 상수라 물리 배치
                                    // 전/후 어느 쪽에서 읽어도 바이트가 동일하다 -
                                    // ExitBootServices 이전(conOut 사용 가능)에 미리
                                    // 확인해 둘 수 있어 이 방식을 택했다. 파일 오프셋
                                    // 기준 정렬이 실제 물리 배치의 8바이트 정렬과 다를
                                    // 수 있어 1바이트 단위로 스캔한다(성능보다 정확성).
                                    for (unsigned long long off = 0; off + 32 <= readSize; ++off) {
                                        unsigned long long candidate = 0;
                                        kCopyBytes(reinterpret_cast<unsigned char*>(&candidate),
                                                   gKernelElfBuffer + off, 8);
                                        if (candidate != kBootMarkerMagic) {
                                            continue;
                                        }
                                        kCopyBytes(reinterpret_cast<unsigned char*>(&gRawPml4),
                                                   gKernelElfBuffer + off + 8, 8);
                                        kCopyBytes(reinterpret_cast<unsigned char*>(&gRawEntry),
                                                   gKernelElfBuffer + off + 16, 8);
                                        kCopyBytes(reinterpret_cast<unsigned char*>(&gRawSavedStartInfoAddr),
                                                   gKernelElfBuffer + off + 24, 8);
                                        gMarkerFound = true;
                                        break;
                                    }
                                    if (conOut) {
                                        kPrint(conOut, u"minicore: boot marker ");
                                        kPrint(conOut, gMarkerFound ? u"found" : u"NOT FOUND");
                                        if (gMarkerFound) {
                                            kPrint(conOut, u" rawPml4=");
                                            kPrintUint64(conOut, gRawPml4);
                                            kPrint(conOut, u" rawEntry=");
                                            kPrintUint64(conOut, gRawEntry);
                                            kPrint(conOut, u" rawSavedStartInfo=");
                                            kPrintUint64(conOut, gRawSavedStartInfoAddr);
                                        }
                                        kPrint(conOut, u"\r\n");
                                    }
                                }
                            }
                        } else if (conOut) {
                            kPrint(conOut, u"minicore: kernel ELF read failed, status=");
                            kPrintUint64(conOut, protoStatus);
                            kPrint(conOut, u"\r\n");
                        }
                        kernelFile->Close(kernelFile);
                    }
                }
            }
        }

        // [신규, PN-7FBF255A 체크리스트 5번 - ExitBootServices 핸드오프]
        // 커널 ELF 로더(파일 읽기+헤더 파싱)가 끝나 더 이상 Boot
        // Services가 필요 없어지는 지점 - 이제 ExitBootServices를
        // 실제로 호출한다. UEFI 명세 권장 패턴대로 바로 직전에
        // GetMemoryMap을 다시 불러 최신 MapKey를 확보한다(파일
        // Read() 등 그 사이 호출들이 내부적으로 메모리를 재배치해
        // 앞서 구한 MapKey가 이미 낡았을 수 있음) - 실패하면(다른
        // 뭔가가 그 사이 또 MapKey를 무효화한 경우) 재조회 후
        // 재시도한다(최대 3회, 명세 권장 패턴).
        if (conOut) {
            kPrint(conOut, u"minicore: exiting boot services\r\n");
        }
        // [신규, PN-7FBF255A 체크리스트 5번] 재배치할 커널 이미지가
        // 차지할 physicalBase 후보를 찾는다 - ExitBootServices 직전에
        // 새로 받은(가장 최신) 메모리맵을 그대로 재사용한다(별도
        // BootServices 호출 없이 로컬 버퍼만 스캔하므로 MapKey를
        // 무효화하지 않음 - GetMemoryMap과 ExitBootServices 사이에
        // 끼워 넣어도 안전하다). §3-1 제약(physicalBase+imageSpan
        // <= 1GiB) + AP 트램폴린(0x8000) 회피까지 여기서 확인한다.
        unsigned long long imageSpan = 0;
        for (unsigned int i = 0; i < gLoadSegmentCount; ++i) {
            const unsigned long long segEnd = (gLoadSegments[i].paddr - kKernelLma) + gLoadSegments[i].memsz;
            if (segEnd > imageSpan) {
                imageSpan = segEnd;
            }
        }
        const bool loaderPreconditionsOk = gMarkerFound && gLoadSegmentCount > 0 && gSelfBelowOneGiB;

        unsigned long long physicalBase = 0;
        bool physicalBaseFound = false;
        bool physicalBaseSearchDone = false;
        bool exitedBootServices = false;
        for (unsigned int attempt = 0; attempt < 3 && !exitedBootServices; ++attempt) {
            unsigned long long finalMapSize = kMemoryMapBufferCapacity;
            unsigned long long finalMapKey = 0;
            unsigned long long finalDescriptorSize = 0;
            unsigned int finalDescriptorVersion = 0;
            EFI_STATUS mapStatus = systemTable->BootServices->GetMemoryMap(
                &finalMapSize, gMemoryMapBuffer, &finalMapKey, &finalDescriptorSize, &finalDescriptorVersion);
            if (mapStatus != kEfiSuccess) {
                if (conOut) {
                    kPrint(conOut, u"minicore: final GetMemoryMap failed, status=");
                    kPrintUint64(conOut, mapStatus);
                    kPrint(conOut, u"\r\n");
                }
                break;
            }

            if (!physicalBaseSearchDone) {
                physicalBaseSearchDone = true;
                if (loaderPreconditionsOk && finalDescriptorSize > 0) {
                    const unsigned long long entryCount = finalMapSize / finalDescriptorSize;
                    for (unsigned long long i = 0; i < entryCount; ++i) {
                        const auto* region =
                            reinterpret_cast<const EFI_MEMORY_DESCRIPTOR*>(gMemoryMapBuffer + i * finalDescriptorSize);
                        if (region->Type != kEfiConventionalMemory) {
                            continue;
                        }
                        const unsigned long long candidate = region->PhysicalStart;
                        if (region->NumberOfPages * kPageSize4K < imageSpan) {
                            continue;
                        }
                        if (candidate + imageSpan > kOneGiB) {
                            continue;
                        }
                        if (candidate <= kApTrampolineSafeLimit && candidate + imageSpan > kApTrampolinePhys) {
                            continue;  // AP 트램폴린 고정 물리주소(0x8000)와 겹침
                        }
                        // [신규, SP-CC2B18C6 §3-3 추가 정정] saved_start_info 등
                        // "원본(비재배치)" 스크래치 주소가 [kKernelLma,
                        // kKernelLma+imageSpan) 범위 안에 있다 - 재배치
                        // 목적지가 이 범위와 겹치면 세그먼트 복사가 그
                        // 스크래치 값을 덮어쓰거나(복사가 나중이면) 반대로
                        // 스크래치 쓰기가 이미 복사된 커널 바이트 일부를
                        // 훼손할 수 있어(쓰기가 나중이면) 겹치지 않는
                        // 후보만 채택한다.
                        if (candidate < kKernelLma + imageSpan && candidate + imageSpan > kKernelLma) {
                            continue;
                        }
                        physicalBase = candidate;
                        physicalBaseFound = true;
                        break;
                    }
                }
                if (conOut) {
                    kPrint(conOut, u"minicore: physicalBase search ");
                    kPrint(conOut, physicalBaseFound ? u"found=" : u"NOT FOUND");
                    if (physicalBaseFound) {
                        kPrintUint64(conOut, physicalBase);
                    }
                    kPrint(conOut, u" imageSpan=");
                    kPrintUint64(conOut, imageSpan);
                    kPrint(conOut, u"\r\n");
                }
            }

            EFI_STATUS exitStatus = systemTable->BootServices->ExitBootServices(imageHandle, finalMapKey);
            if (exitStatus == kEfiSuccess) {
                exitedBootServices = true;
                break;
            }
            if (conOut) {
                kPrint(conOut, u"minicore: ExitBootServices failed, status=");
                kPrintUint64(conOut, exitStatus);
                kPrint(conOut, u" attempt=");
                kPrintUint64(conOut, attempt);
                kPrint(conOut, u"\r\n");
            }
        }

        // Boot Services는 이 시점부터 전부 호출 금지(명세) - ConOut도
        // 더 이상 유효하다는 보장이 없어 exitedBootServices==true 이후
        // 아무것도 찍지 않는다(아래 전부 무음 - 실패하면 hlt로 정직하게
        // 멈춘다는 이 파일의 기존 관례를 그대로 따른다). [TEMP 진단]
        // raw serial(BootServices 호출 아님, 명세 위반 없음)로 이
        // 시점 이후 진행 상황을 표시한다 - 원인 파악 끝나면 제거.
        kRawSerialWrite(exitedBootServices ? "[T:exit=1]" : "[T:exit=0]");
        const bool loaderReady = exitedBootServices && loaderPreconditionsOk && physicalBaseFound;
        kRawSerialWrite(loaderReady ? "[T:ready=1]" : "[T:ready=0]");
        if (loaderReady) {
            kRawSerialWrite("[T:copy-begin]");
            // [신규, PN-7FBF255A 체크리스트 5번] 각 PT_LOAD 세그먼트를
            // physicalBase 기준 새 위치로 복사 - UEFI x64는 자신의
            // usable RAM 전체를 identity map한 상태로 두므로 물리주소를
            // 그대로 포인터로 캐스팅해 쓸 수 있다는 게 이 설계의 전제
            // (SP-CC2B18C6 §3-2). memsz > filesz인 나머지(.bss, boot.S의
            // pml4 등 5개 페이지테이블 포함)는 0으로 채운다.
            const unsigned long long delta = physicalBase - kKernelLma;
            for (unsigned int i = 0; i < gLoadSegmentCount; ++i) {
                const LoadSegment& seg = gLoadSegments[i];
                auto* dest = reinterpret_cast<unsigned char*>(physicalBase + (seg.paddr - kKernelLma));
                kCopyBytes(dest, gKernelElfBuffer + seg.offset, seg.filesz);
                if (seg.memsz > seg.filesz) {
                    kZeroBytes(dest + seg.filesz, seg.memsz - seg.filesz);
                }
            }

            // boot.S .boot.bss 선언 순서(pml4->pdpt_low->pd_low->
            // pdpt_high->pd_high, 각 4096바이트 .align)를 그대로 이용해
            // 나머지 4개 테이블 주소를 pml4 기준 오프셋으로 계산한다
            // (SP-CC2B18C6 §3-2 4번).
            const unsigned long long actualPml4Addr = gRawPml4 + delta;
            const unsigned long long actualPdptLowAddr = actualPml4Addr + kPageSize4K;
            const unsigned long long actualPdLowAddr = actualPml4Addr + kPageSize4K * 2;
            const unsigned long long actualPdptHighAddr = actualPml4Addr + kPageSize4K * 3;
            const unsigned long long actualPdHighAddr = actualPml4Addr + kPageSize4K * 4;
            const unsigned long long actualEntry = gRawEntry + delta;
            kRawSerialWrite("[T:copy-done]");

            auto* pml4 = reinterpret_cast<unsigned long long*>(actualPml4Addr);
            auto* pdptLow = reinterpret_cast<unsigned long long*>(actualPdptLowAddr);
            auto* pdLow = reinterpret_cast<unsigned long long*>(actualPdLowAddr);
            auto* pdptHigh = reinterpret_cast<unsigned long long*>(actualPdptHighAddr);
            auto* pdHigh = reinterpret_cast<unsigned long long*>(actualPdHighAddr);

            // boot.S setup_page_tables와 정확히 같은 값 - PD_LOW는
            // physicalBase와 무관한 순수 물리 identity map(0..1GiB,
            // §3-1), PD_HIGH만 원본 공식(V - KERNEL_VMA, 즉 i*2MiB)에
            // +delta를 더해 물리적으로 재배치된 위치를 가리키게 한다
            // (paging.cpp의 pdptPhys 보정과 동일한 physicalBaseDelta
            // 패턴, SP-CC2B18C6 §2).
            pml4[0] = actualPdptLowAddr | kPagePresentWritable;
            pdptLow[0] = actualPdLowAddr | kPagePresentWritable;
            for (unsigned int i = 0; i < 512; ++i) {
                pdLow[i] = (static_cast<unsigned long long>(i) * kPageSize2M) | kPagePresentWritableHuge;
            }
            pml4[511] = actualPdptHighAddr | kPagePresentWritable;
            pdptHigh[510] = actualPdHighAddr | kPagePresentWritable;
            for (unsigned int i = 0; i < 512; ++i) {
                pdHigh[i] = ((static_cast<unsigned long long>(i) * kPageSize2M) + delta) | kPagePresentWritableHuge;
            }

            // [신규, SP-CC2B18C6 §3-3 추가 정정] higher_half_entry는
            // "movabs rax, offset saved_start_info"라는 링크 타임
            // 절대주소(재배치 무관, 항상 원본 물리주소)로 kMain 인자를
            // 읽는다 - 그래서 재배치된 사본이 아니라 이 원본 주소에
            // 직접 physicalBaseDelta/bootProtocol=2(kBootProtocolUefi,
            // kmain.cpp와 반드시 일치)를 써야 한다. 위 physicalBase
            // 탐색에서 이 원본 범위와 재배치 목적지가 겹치지 않게 이미
            // 배제했으므로 이 쓰기가 방금 복사한 커널 바이트를 훼손하지
            // 않는다. delta는 §3-1 제약(physicalBase+imageSpan<=1GiB)
            // 덕에 항상 32비트에 들어맞는다.
            auto* rawSavedStartInfo = reinterpret_cast<unsigned int*>(gRawSavedStartInfoAddr);
            auto* rawSavedBootProtocol = reinterpret_cast<unsigned int*>(gRawSavedStartInfoAddr + 4);
            *rawSavedStartInfo = static_cast<unsigned int>(delta);
            *rawSavedBootProtocol = 2;  // kBootProtocolUefi - kmain.cpp와 일치시켜야 함
            kRawSerialWrite("[T:jmp-imminent]");

            asm volatile(
                "mov %0, %%cr3\n"
                "jmp *%1\n"
                :
                : "r"(actualPml4Addr), "r"(actualEntry)
                : "memory");
        }

        // 여기 도달하면(loaderReady==false, 또는 위 jmp가 실행됐어야
        // 하는데 실행되지 않은 경우는 없음 - jmp는 절대 반환하지 않음)
        // 정직하게 멈춘다. exitedBootServices==false면 아직 firmware가
        // 살아있을 수도 있지만, 명세상 이 지점 이후 리턴은 금지이므로
        // (run-uefi.sh의 기존 관례) 어느 경우든 hlt로 대기한다.
        kRawSerialWrite("[T:hlt-fallback]");
        for (;;) {
            asm volatile("cli; hlt");
        }
    }

    return kEfiSuccess;
}
