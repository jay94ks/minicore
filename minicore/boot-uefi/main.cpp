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
                            kPrintUint64(conOut, imageAddr < (1ULL << 30) ? 1 : 0);
                            kPrint(conOut, u"\r\n");
                        }
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
    }

    return kEfiSuccess;
}
