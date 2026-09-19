#include "ahci.h"

#include "libcenv/mem.h"
#include "libmc/pnp.h"
#include "libmc/syscall.h"

namespace ahci {

namespace {

// --- HBA(전역) 레지스터 오프셋(AHCI 1.3.1 사양 §3.1) ---
constexpr mc::uint64_t kRegCap = 0x00;   // Host Capabilities
constexpr mc::uint64_t kRegGhc = 0x04;   // Global Host Control
constexpr mc::uint64_t kRegPi = 0x0C;    // Ports Implemented
constexpr mc::uint64_t kPortRegionBase = 0x100;
constexpr mc::uint64_t kPortRegionStride = 0x80;

// --- 포트별 레지스터 오프셋(포트 블록 시작 기준, §3.3) ---
constexpr mc::uint64_t kPortClb = 0x00;
constexpr mc::uint64_t kPortClbu = 0x04;
constexpr mc::uint64_t kPortFb = 0x08;
constexpr mc::uint64_t kPortFbu = 0x0C;
constexpr mc::uint64_t kPortIs = 0x10;
constexpr mc::uint64_t kPortCmd = 0x18;
constexpr mc::uint64_t kPortTfd = 0x20;
constexpr mc::uint64_t kPortSsts = 0x28;
constexpr mc::uint64_t kPortSerr = 0x30;
constexpr mc::uint64_t kPortCi = 0x38;

// CAP 비트(§3.1.1)
constexpr mc::uint32_t kCapNpMask = 0x1F;    // bits4:0 - Number of Ports - 1
constexpr mc::uint32_t kCapNcsShift = 8;
constexpr mc::uint32_t kCapNcsMask = 0x1F;   // bits12:8 - Number of Command Slots - 1
constexpr mc::uint32_t kCapS64a = 1u << 31;  // 64비트 주소 지정 지원

// GHC 비트(§3.1.2)
constexpr mc::uint32_t kGhcAe = 1u << 31;  // AHCI Enable

// PxCMD 비트(§3.3.7)
constexpr mc::uint32_t kPortCmdSt = 1u << 0;   // Start
constexpr mc::uint32_t kPortCmdFre = 1u << 4;  // FIS Receive Enable
constexpr mc::uint32_t kPortCmdFr = 1u << 14;  // FIS Receive Running
constexpr mc::uint32_t kPortCmdCr = 1u << 15;  // Command List Running

// PxTFD 비트(§3.3.8)
constexpr mc::uint32_t kPortTfdErr = 1u << 0;

// PxSSTS.DET(§3.3.10) - 3이면 장치 있음 + 통신 확립.
constexpr mc::uint32_t kPortSstsDetMask = 0x0F;
constexpr mc::uint32_t kPortSstsDetPresent = 0x3;

constexpr mc::uint64_t kDmaPageSize = 4096;

// v1 폴링 상한 - 실측 후 조정 대상(RM-23F4B687 §4). 인터럽트 배선
// (§3.3)이 아직 없어 스핀 폴링으로 완료를 확인한다.
constexpr mc::uint32_t kPollIterations = 20000000;

volatile mc::uint32_t* kReg32(mc::uint64_t baseVirtAddr, mc::uint64_t offset) {
    return reinterpret_cast<volatile mc::uint32_t*>(baseVirtAddr + offset);
}

// --- 커맨드 리스트/커맨드 테이블/FIS 구조체(§3.1의 계층도가 가리키는
// 실제 온-메모리 레이아웃, AHCI 사양 §4.2/§5.3/§4.3) ---

// 커맨드 헤더 - 커맨드 리스트의 슬롯 하나(32바이트, 사양 §4.2.2).
struct CommandHeader {
    mc::uint32_t dw0;  // bits4:0=CFL(FIS 길이, DWORD 단위), bit6=W(쓰기), bits31:16=PRDTL(PRDT 개수)
    mc::uint32_t prdbc;  // 전송된 바이트 수(하드웨어가 갱신) - out
    mc::uint32_t ctbaLow;
    mc::uint32_t ctbaHigh;
    mc::uint32_t reserved[4];
};
static_assert(sizeof(CommandHeader) == 32, "AHCI 사양 §4.2.2 - 커맨드 헤더는 32바이트 고정");

// PRDT(Physical Region Descriptor Table) 엔트리(16바이트, 사양 §4.2.3.3).
struct PrdtEntry {
    mc::uint32_t dbaLow;
    mc::uint32_t dbaHigh;
    mc::uint32_t reserved;
    mc::uint32_t dw3;  // bits21:0=byte count-1, bit31=I(완료 시 인터럽트)
};
static_assert(sizeof(PrdtEntry) == 16, "AHCI 사양 §4.2.3.3 - PRDT 엔트리는 16바이트 고정");

// 커맨드 테이블 레이아웃(사양 §4.2.3) - CFIS(64바이트 예약) + ACMD(16,
// 이 드라이버는 안 씀) + 예약(48) 다음 오프셧 0x80부터 PRDT 배열.
constexpr mc::uint64_t kCmdTablePrdtOffset = 0x80;

// Register Host-to-Device FIS(20바이트, 사양 §10.3.4).
struct RegH2dFis {
    mc::uint8_t fisType;     // 0x27
    mc::uint8_t pmportAndC;  // bit7=1(Command)
    mc::uint8_t command;
    mc::uint8_t features0;
    mc::uint8_t lba0, lba1, lba2, device;
    mc::uint8_t lba3, lba4, lba5, features1;
    mc::uint8_t countLow, countHigh, icc, control;
    mc::uint8_t reserved[4];
};
static_assert(sizeof(RegH2dFis) == 20, "AHCI 사양 §10.3.4 - Register H2D FIS는 20바이트 고정");

constexpr mc::uint8_t kFisTypeRegH2d = 0x27;
constexpr mc::uint8_t kAtaCommandIdentifyDevice = 0xEC;

// AllocDmaBuffer 왕복 하나를 묶어 둔 헬퍼 - virt/phys/handle 세 값을
// 전부 호출부에 돌려준다(PxCLB류 레지스터에는 물리주소, 실제 메모리
// 접근에는 가상주소 둘 다 필요하므로).
struct DmaAlloc {
    mc::uint64_t virtAddr = 0;
    mc::uint64_t physAddr = 0;
    mc::uint32_t handle = 0;
};

bool kAllocDma(mc::uint64_t sizeBytes, bool use32Bit, DmaAlloc* out) {
    mc::AllocDmaBufferArgs args;
    args.sizeBytes = sizeBytes;
    args.physAddrLimit = use32Bit ? 32 : 0;
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointAllocDmaBuffer, &args);
    if (token != 0) {
        mc::wait(token);
    }
    if (args.error != mc::ChannelError::None) {
        return false;
    }
    out->virtAddr = args.virtualAddr;
    out->physAddr = args.physicalAddr;
    out->handle = args.handle;
    return true;
}

}  // namespace

bool AhciPort::init(mc::uint64_t hbaVirtAddr, mc::uint32_t portIndex, mc::uint32_t slotCount, bool use32BitDma) {
    _portRegBase = hbaVirtAddr + kPortRegionBase + static_cast<mc::uint64_t>(portIndex) * kPortRegionStride;
    _slotCount = slotCount;
    _use32BitDma = use32BitDma;

    // §3.3.7 포트 시작 절차(사양) - PxCLB/PxFB를 새로 채우기 전에 이미
    // 실행 중이면 먼저 정지시킨다(부팅 직후라 보통 이미 꺼져 있지만
    // 방어적으로 확인).
    volatile mc::uint32_t* cmd = kReg32(_portRegBase, kPortCmd);
    if (*cmd & (kPortCmdSt | kPortCmdFre)) {
        *cmd &= ~static_cast<mc::uint32_t>(kPortCmdSt | kPortCmdFre);
        for (mc::uint32_t i = 0; i < kPollIterations; ++i) {
            if (!(*cmd & (kPortCmdCr | kPortCmdFr))) {
                break;
            }
        }
    }

    // §3.1 "커맨드 리스트(1페이지)/FIS 수신 버퍼(1페이지)를 확보" -
    // AllocDmaBuffer는 항상 4KiB 단위로 올림하므로 실제 필요량(커맨드
    // 리스트: slotCount*32바이트, FIS 수신: 256바이트)만 넘겨도 한
    // 페이지가 그대로 배정된다 - 페이지 정렬 자체가 사양이 요구하는
    // 1KB/256바이트 정렬을 자동으로 만족한다.
    DmaAlloc clb;
    DmaAlloc fb;
    if (!kAllocDma(kDmaPageSize, _use32BitDma, &clb)) {
        return false;
    }
    if (!kAllocDma(kDmaPageSize, _use32BitDma, &fb)) {
        return false;
    }
    memset(reinterpret_cast<void*>(clb.virtAddr), 0, kDmaPageSize);
    memset(reinterpret_cast<void*>(fb.virtAddr), 0, kDmaPageSize);

    _clbVirtAddr = clb.virtAddr;
    _fbVirtAddr = fb.virtAddr;
    _clbDmaHandle = clb.handle;
    _fbDmaHandle = fb.handle;

    // PxCLB/PxCLBU, PxFB/PxFBU에 물리주소를 채운다(사양 §3.3.1/§3.3.3) -
    // 포트가 정지된 상태에서만 안전하게 쓸 수 있다(위에서 이미 정지
    // 확인).
    *kReg32(_portRegBase, kPortClb) = static_cast<mc::uint32_t>(clb.physAddr & 0xFFFFFFFFu);
    *kReg32(_portRegBase, kPortClbu) = static_cast<mc::uint32_t>(clb.physAddr >> 32);
    *kReg32(_portRegBase, kPortFb) = static_cast<mc::uint32_t>(fb.physAddr & 0xFFFFFFFFu);
    *kReg32(_portRegBase, kPortFbu) = static_cast<mc::uint32_t>(fb.physAddr >> 32);

    // 이전 세션이 남긴 오류/인터럽트 상태 비우기(사양 §10.1.2 포트
    // 초기화 절차) - PxSERR/PxIS는 write-1-to-clear.
    *kReg32(_portRegBase, kPortSerr) = 0xFFFFFFFFu;
    *kReg32(_portRegBase, kPortIs) = 0xFFFFFFFFu;

    // FIS 수신 엔진을 먼저 켠 뒤(FRE) 커맨드 리스트 처리도 켠다(ST) -
    // 사양이 요구하는 순서(§10.1.2) 그대로. 장치가 실제로 붙어 있는지
    // 여부와 무관하게 포트 자체는 항상 이 상태로 둔다(핫플러그 대비,
    // §3.4는 이 계획 범위 밖이지만 엔진을 꺼 둘 이유도 없다).
    *cmd |= kPortCmdFre;
    *cmd |= kPortCmdSt;

    return true;
}

namespace {

void kFreeDma(mc::uint32_t handle) {
    mc::FreeDmaBufferArgs args;
    args.handle = handle;
    mc::SyscallToken t = mc::submit(mc::kSyscallEndpointFreeDmaBuffer, &args);
    if (t != 0) {
        mc::wait(t);
    }
}

}  // namespace

// [SP-C2670F69 §3.1, PN-4E6EA13D/PN-F60E405A A 공유] Register H2D FIS +
// PRDT 엔트리 1개 + 커맨드 헤더(슬롯 0)를 구성해 발급하고 완료까지
// 폴링한다 - IDENTIFY DEVICE/READ DMA EXT/WRITE DMA EXT 전부 이
// 골격 하나로 표현된다(ATA 사양 §7 각 커맨드가 공통으로 쓰는 Register
// H2D FIS 포맷 덕분). lba/sectorCount가 무의미한 커맨드(IDENTIFY 등)는
// 0으로 넘기면 된다 - LBA48 필드(lba0-5)/Count(16비트)를 그대로 채워도
// 장치가 그 값을 무시하는 커맨드라 안전하다.
bool AhciPort::issueAtaCommand(mc::uint8_t command, mc::uint64_t lba, mc::uint32_t sectorCount, bool isWrite,
                                mc::uint64_t dataPhysAddr, mc::uint32_t dataBytes) {
    // 커맨드 테이블(슬롯 0 전용, CFIS + PRDT 1개) - 페이지 하나면
    // CFIS(0x80 예약 영역) + PRDT 엔트리 1개(16바이트)를 넉넉히 담는다.
    DmaAlloc cmdTable;
    if (!kAllocDma(kDmaPageSize, _use32BitDma, &cmdTable)) {
        return false;
    }
    memset(reinterpret_cast<void*>(cmdTable.virtAddr), 0, kDmaPageSize);

    // CFIS 영역(커맨드 테이블 오프셋 0) - Register H2D FIS(사양 §10.3.4).
    // LBA48 모드 - device 레지스터는 드라이브/헤드 비트 없이 그대로
    // 0(LBA 모드 자체는 커맨드 종류(EXT 접미) 자체가 암시).
    auto* fis = reinterpret_cast<RegH2dFis*>(cmdTable.virtAddr);
    *fis = RegH2dFis{};
    fis->fisType = kFisTypeRegH2d;
    fis->pmportAndC = 0x80;  // bit7=1(Command), PM port=0
    fis->command = command;
    fis->device = 0;
    fis->lba0 = static_cast<mc::uint8_t>(lba & 0xFF);
    fis->lba1 = static_cast<mc::uint8_t>((lba >> 8) & 0xFF);
    fis->lba2 = static_cast<mc::uint8_t>((lba >> 16) & 0xFF);
    fis->lba3 = static_cast<mc::uint8_t>((lba >> 24) & 0xFF);
    fis->lba4 = static_cast<mc::uint8_t>((lba >> 32) & 0xFF);
    fis->lba5 = static_cast<mc::uint8_t>((lba >> 40) & 0xFF);
    fis->countLow = static_cast<mc::uint8_t>(sectorCount & 0xFF);
    fis->countHigh = static_cast<mc::uint8_t>((sectorCount >> 8) & 0xFF);

    // PRDT 엔트리 1개(오프셋 0x80) - dw3의 byte count 필드는 "실제
    // 길이-1"(사양 §4.2.3.3).
    auto* prdt = reinterpret_cast<PrdtEntry*>(cmdTable.virtAddr + kCmdTablePrdtOffset);
    prdt[0] = PrdtEntry{};
    prdt[0].dbaLow = static_cast<mc::uint32_t>(dataPhysAddr & 0xFFFFFFFFu);
    prdt[0].dbaHigh = static_cast<mc::uint32_t>(dataPhysAddr >> 32);
    prdt[0].dw3 = dataBytes - 1;  // 인터럽트 비트(I)는 안 씀(폴링 방식)

    // 커맨드 헤더(슬롯 0) - CFL은 DWORD 단위 FIS 길이(20바이트/4=5),
    // PRDTL=1, W는 전송 방향(호스트->장치면 1).
    auto* header = reinterpret_cast<CommandHeader*>(_clbVirtAddr);
    header[0] = CommandHeader{};
    header[0].dw0 = 5u;  // CFL=5
    if (isWrite) {
        header[0].dw0 |= (1u << 6);  // W
    }
    header[0].dw0 |= (1u << 16);  // PRDTL=1
    header[0].ctbaLow = static_cast<mc::uint32_t>(cmdTable.physAddr & 0xFFFFFFFFu);
    header[0].ctbaHigh = static_cast<mc::uint32_t>(cmdTable.physAddr >> 32);

    // 슬롯 0의 이전 오류 상태를 비우고(PxSERR write-1-to-clear) 발급한다
    // (PxCI 비트0 세팅, 사양 §5.5).
    *kReg32(_portRegBase, kPortSerr) = 0xFFFFFFFFu;
    *kReg32(_portRegBase, kPortCi) |= 1u;

    // 완료 폴링 - PxCI 비트0이 하드웨어에 의해 클리어되면 발급된
    // 커맨드가 완료된 것이다(§5.5, 인터럽트 미배선이라 스핀 폴링).
    // 그 사이 PxTFD.ERR/BSY로 오류를 함께 감시한다(§3.4의 "포트 오류는
    // TFD로 감지" 원칙 그대로 - COMRESET 등 전체 오류 복구 절차는
    // 이 v1 최소 검증 범위 밖).
    bool completed = false;
    bool ioError = false;
    for (mc::uint32_t i = 0; i < kPollIterations; ++i) {
        const mc::uint32_t ci = *kReg32(_portRegBase, kPortCi);
        if (!(ci & 1u)) {
            completed = true;
            break;
        }
        const mc::uint32_t tfd = *kReg32(_portRegBase, kPortTfd);
        if (tfd & kPortTfdErr) {
            ioError = true;
            break;
        }
    }

    kFreeDma(cmdTable.handle);
    return completed && !ioError;
}

bool AhciPort::probeWithIdentify(PortProbeResult* outResult) {
    *outResult = PortProbeResult{};

    volatile mc::uint32_t* ssts = kReg32(_portRegBase, kPortSsts);
    if ((*ssts & kPortSstsDetMask) != kPortSstsDetPresent) {
        return false;  // 장치 없음 - AhciController::probeFirstDevice가 다음 포트로 넘어간다
    }
    outResult->devicePresent = true;

    // IDENTIFY DEVICE 응답 버퍼(512바이트, ATA 사양 고정 크기) - LBA/
    // Count는 IDENTIFY에 의미 없어 0으로 넘긴다.
    DmaAlloc identifyBuf;
    if (!kAllocDma(512, _use32BitDma, &identifyBuf)) {
        return false;
    }

    const bool ok = issueAtaCommand(kAtaCommandIdentifyDevice, 0, 0, false, identifyBuf.physAddr, 512);
    if (ok) {
        auto* words = reinterpret_cast<mc::uint16_t*>(identifyBuf.virtAddr);
        for (mc::uint32_t i = 0; i < 256; ++i) {
            outResult->identifyData[i] = words[i];
        }
        outResult->identifySucceeded = true;
    }

    kFreeDma(identifyBuf.handle);
    return outResult->identifySucceeded;
}

bool AhciPort::readSectors(mc::uint64_t lba, mc::uint32_t count, void* outBuf) {
    volatile mc::uint32_t* ssts = kReg32(_portRegBase, kPortSsts);
    if ((*ssts & kPortSstsDetMask) != kPortSstsDetPresent || count == 0) {
        return false;
    }

    const mc::uint64_t bytes = static_cast<mc::uint64_t>(count) * 512u;
    DmaAlloc dataBuf;
    if (!kAllocDma(bytes, _use32BitDma, &dataBuf)) {
        return false;
    }

    // READ DMA EXT(0x25, LBA48) - 장치->호스트 전송이라 W=0.
    const bool ok = issueAtaCommand(0x25, lba, count, false, dataBuf.physAddr, static_cast<mc::uint32_t>(bytes));
    if (ok) {
        memcpy(outBuf, reinterpret_cast<const void*>(dataBuf.virtAddr), bytes);
    }

    kFreeDma(dataBuf.handle);
    return ok;
}

bool AhciPort::writeSectors(mc::uint64_t lba, mc::uint32_t count, const void* buf) {
    volatile mc::uint32_t* ssts = kReg32(_portRegBase, kPortSsts);
    if ((*ssts & kPortSstsDetMask) != kPortSstsDetPresent || count == 0) {
        return false;
    }

    const mc::uint64_t bytes = static_cast<mc::uint64_t>(count) * 512u;
    DmaAlloc dataBuf;
    if (!kAllocDma(bytes, _use32BitDma, &dataBuf)) {
        return false;
    }
    memcpy(reinterpret_cast<void*>(dataBuf.virtAddr), buf, bytes);

    // WRITE DMA EXT(0x35, LBA48) - 호스트->장치 전송이라 W=1.
    const bool ok = issueAtaCommand(0x35, lba, count, true, dataBuf.physAddr, static_cast<mc::uint32_t>(bytes));

    kFreeDma(dataBuf.handle);
    return ok;
}

bool AhciController::init(mc::uint64_t mmioVirtAddr) {
    _mmioVirtAddr = mmioVirtAddr;

    volatile mc::uint32_t* ghc = kReg32(mmioVirtAddr, kRegGhc);
    *ghc |= kGhcAe;

    const mc::uint32_t cap = *kReg32(mmioVirtAddr, kRegCap);
    const mc::uint32_t numPorts = (cap & kCapNpMask) + 1;
    _slotCount = ((cap >> kCapNcsShift) & kCapNcsMask) + 1;
    const bool use32BitDma = (cap & kCapS64a) == 0;

    _portsImplemented = *kReg32(mmioVirtAddr, kRegPi);

    for (mc::uint32_t i = 0; i < numPorts && i < 32; ++i) {
        if (!(_portsImplemented & (1u << i))) {
            continue;
        }
        _portInitialized[i] = _ports[i].init(mmioVirtAddr, i, _slotCount, use32BitDma);
    }
    return true;
}

bool AhciController::probeFirstDevice(PortProbeResult* outResult) {
    for (mc::uint32_t i = 0; i < 32; ++i) {
        if (!_portInitialized[i]) {
            continue;
        }
        if (_ports[i].probeWithIdentify(outResult)) {
            return true;
        }
    }
    return false;
}

}  // namespace ahci
