#include "ahci.h"

#include "async_task.h"
#include "dma_buffer.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "pnp.h"
#include "scheduler.h"
#include "task.h"

namespace ahci {

namespace {

using kernel::uint16_t;
using kernel::uint32_t;
using kernel::uint64_t;
using kernel::uint8_t;

// --- HBA(전역) 레지스터 오프셋(AHCI 1.3.1 사양 §3.1) ---
constexpr uint64_t kRegCap = 0x00;   // Host Capabilities
constexpr uint64_t kRegGhc = 0x04;   // Global Host Control
constexpr uint64_t kRegPi = 0x0C;    // Ports Implemented
constexpr uint64_t kPortRegionBase = 0x100;
constexpr uint64_t kPortRegionStride = 0x80;

// --- 포트별 레지스터 오프셋(포트 블록 시작 기준, §3.3) ---
constexpr uint64_t kPortClb = 0x00;
constexpr uint64_t kPortClbu = 0x04;
constexpr uint64_t kPortFb = 0x08;
constexpr uint64_t kPortFbu = 0x0C;
constexpr uint64_t kPortIs = 0x10;
constexpr uint64_t kPortCmd = 0x18;
constexpr uint64_t kPortTfd = 0x20;
constexpr uint64_t kPortSsts = 0x28;
constexpr uint64_t kPortSerr = 0x30;
constexpr uint64_t kPortSact = 0x34;  // [PN-A401DDF9] Serial ATA Active - NCQ 커맨드 전용(§3.3.10 인접)
constexpr uint64_t kPortCi = 0x38;

// [PN-A401DDF9] acquireSlot()이 사용 가능한 슬롯을 못 찾았을 때.
constexpr uint32_t kAhciInvalidSlot = 0xFFFFFFFFu;

// CAP 비트(§3.1.1)
constexpr uint32_t kCapNpMask = 0x1F;    // bits4:0 - Number of Ports - 1
constexpr uint32_t kCapNcsShift = 8;
constexpr uint32_t kCapNcsMask = 0x1F;   // bits12:8 - Number of Command Slots - 1
constexpr uint32_t kCapS64a = 1u << 31;  // 64비트 주소 지정 지원

// GHC 비트(§3.1.2)
constexpr uint32_t kGhcAe = 1u << 31;  // AHCI Enable

// PxCMD 비트(§3.3.7)
constexpr uint32_t kPortCmdSt = 1u << 0;   // Start
constexpr uint32_t kPortCmdFre = 1u << 4;  // FIS Receive Enable
constexpr uint32_t kPortCmdFr = 1u << 14;  // FIS Receive Running
constexpr uint32_t kPortCmdCr = 1u << 15;  // Command List Running

// PxTFD 비트(§3.3.8)
// [가설 검증 후 원복, 2026-09-20, PN-584DB994 §갱신14] 한때 이 비트가
// "ERROR 레지스터 사본(비트7:0)/STATUS 레지스터 사본(비트15:8)"
// 순서라 생각해 STS.ERR을 비트8로 옮기는 수정을 시도했으나, 실측
// 재현(40회)에서 크래시율이 5%대→35%(14/40)로 오히려 크게 악화돼
// 명백히 틀린 가설로 반증됐다 - Linux ahci.h가 `PORT_TFDATA`의
// **하위 바이트**를 시프트 없이 그대로 `ATA_BUSY(0x80)`/`ATA_DRQ(0x08)`
// 와 비교하는 것으로 미뤄, 실제로는 **하위 바이트(비트7:0)가 STATUS
// 레지스터 사본**이고 원래 코드의 비트0(STATUS.ERR)이 처음부터
// 맞았다 - 원복.
constexpr uint32_t kPortTfdErr = 1u << 0;

// PxSSTS.DET(§3.3.10) - 3이면 장치 있음 + 통신 확립.
constexpr uint32_t kPortSstsDetMask = 0x0F;
constexpr uint32_t kPortSstsDetPresent = 0x3;

constexpr uint64_t kDmaPageSize = 4096;

// v1 폴링 상한 - 실측 후 조정 대상(RM-23F4B687 §4). 인터럽트 배선
// (§3.3)이 아직 없어 스핀 폴링으로 완료를 확인한다.
constexpr uint32_t kPollIterations = 20000000;

volatile uint32_t* kReg32(uint64_t baseVirtAddr, uint64_t offset) {
    return reinterpret_cast<volatile uint32_t*>(baseVirtAddr + offset);
}

// --- 커맨드 리스트/커맨드 테이블/FIS 구조체(§3.1의 계층도가 가리키는
// 실제 온-메모리 레이아웃, AHCI 사양 §4.2/§5.3/§4.3) ---

// 커맨드 헤더 - 커맨드 리스트의 슬롯 하나(32바이트, 사양 §4.2.2).
struct CommandHeader {
    uint32_t dw0;  // bits4:0=CFL(FIS 길이, DWORD 단위), bit6=W(쓰기), bits31:16=PRDTL(PRDT 개수)
    uint32_t prdbc;  // 전송된 바이트 수(하드웨어가 갱신) - out
    uint32_t ctbaLow;
    uint32_t ctbaHigh;
    uint32_t reserved[4];
};
static_assert(sizeof(CommandHeader) == 32, "AHCI 사양 §4.2.2 - 커맨드 헤더는 32바이트 고정");

// PRDT(Physical Region Descriptor Table) 엔트리(16바이트, 사양 §4.2.3.3).
struct PrdtEntry {
    uint32_t dbaLow;
    uint32_t dbaHigh;
    uint32_t reserved;
    uint32_t dw3;  // bits21:0=byte count-1, bit31=I(완료 시 인터럽트)
};
static_assert(sizeof(PrdtEntry) == 16, "AHCI 사양 §4.2.3.3 - PRDT 엔트리는 16바이트 고정");

// 커맨드 테이블 레이아웃(사양 §4.2.3) - CFIS(64바이트 예약) + ACMD(16,
// 이 드라이버는 안 씀) + 예약(48) 다음 오프셧 0x80부터 PRDT 배열.
constexpr uint64_t kCmdTablePrdtOffset = 0x80;

// Register Host-to-Device FIS(20바이트, 사양 §10.3.4).
struct RegH2dFis {
    uint8_t fisType;     // 0x27
    uint8_t pmportAndC;  // bit7=1(Command)
    uint8_t command;
    uint8_t features0;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, features1;
    uint8_t countLow, countHigh, icc, control;
    uint8_t reserved[4];
};
static_assert(sizeof(RegH2dFis) == 20, "AHCI 사양 §10.3.4 - Register H2D FIS는 20바이트 고정");

constexpr uint8_t kFisTypeRegH2d = 0x27;
constexpr uint8_t kAtaCommandIdentifyDevice = 0xEC;
constexpr uint8_t kAtaCommandReadDmaExt = 0x25;
constexpr uint8_t kAtaCommandWriteDmaExt = 0x35;
constexpr uint8_t kAtaCommandFlushCacheExt = 0xEA;
// [PN-A401DDF9, ATA8-ACS] READ/WRITE FPDMA QUEUED - NCQ 전용 커맨드.
// Register H2D FIS 필드 배치가 DMA EXT류와 다르다(아래 submitAtaCommand
// 참고 - Sector Count는 Features 필드로, Count 필드는 커맨드 태그로).
constexpr uint8_t kAtaCommandReadFpdmaQueued = 0x60;
constexpr uint8_t kAtaCommandWriteFpdmaQueued = 0x61;

// AllocDmaBuffer 왕복 하나를 묶어 둔 헬퍼 - virt/phys/handle 세 값을
// 전부 호출부에 돌려준다(PxCLB류 레지스터에는 물리주소, 실제 메모리
// 접근에는 가상주소 둘 다 필요하므로).
struct DmaAlloc {
    uint64_t virtAddr = 0;
    uint64_t physAddr = 0;
    uint32_t handle = 0;
};

// [전환, 2026-09-20, SP-43331889/QU-5FC58B06] 트랩(mc::submit/wait)
// 대신 kernel::kAllocDmaBufferSync()/kFreeDmaBufferSync()를 직접
// 호출한다 - fs 자신이 이미 커널 안에 있어 트랩 자체가 무의미
// (devmgr과 동일한 이유). 이 fs KernelThread 자기 자신을
// `SharedPtr<kernel::Task>`로 얻어 매 호출마다 caller로 넘긴다
// (dma_buffer.h 문서 주석 참고 - 커널 모드 분기는 아직 설계 미정이라
// 매번 `ChannelError::NotSupported`로 실패하지만, 이 호출 자체는
// 구조적으로 완전하다).
kernel::SharedPtr<kernel::Task> kCurrentFsTask() {
    auto* self = static_cast<kernel::KernelThread*>(kernel::Scheduler::currentTask());
    return self->weakAsTask().lock();
}

bool kAllocDma(uint64_t sizeBytes, bool use32Bit, DmaAlloc* out) {
    kernel::ChannelError error = kernel::ChannelError::None;
    kernel::kAllocDmaBufferSync(kCurrentFsTask(), sizeBytes, use32Bit ? 32 : 0, &out->virtAddr, &out->physAddr,
                                 &out->handle, &error);
    return error == kernel::ChannelError::None;
}

void kFreeDma(uint32_t handle) {
    kernel::ChannelError error = kernel::ChannelError::None;
    kernel::kFreeDmaBufferSync(kCurrentFsTask(), handle, &error);
}

// [PN-A401DDF9, SP-C2670F69 §3.5] AhciPort::submitAtaCommand()가 슬롯
// 배정과 실제 발급(레지스터 세팅)까지 전부 동기적으로 끝낸 뒤, 이 args를
// 채워 AsyncTask로 제출한다 - 아래 AhciCommandHandler::onExec은 오직
// "언제 끝나는지"만 폴링하고(매 반복 co_await kernel::AsyncTaskCoroYield{}
// 로 리액터에 한 턴 양보 - **[정정, 2026-09-22, PN-A0CEF82D 실측 발견]**
// 원래 여기 raw kernel::AsyncTask::yield()를 썼었는데, 이 onExec은 진짜
// 코루틴이라 그 스택풀 전용 프리미티브를 쓰면 무한 대기가 났다 - 자세한
// 원인/정정은 async_task.h의 AsyncTaskCoroYield 문서 주석 참고),
// 완료되면 데이터 복사/DMA 반납/슬롯 반납까지 마무리한다. 이 구조체
// 자체는 submitAtaCommand(생산자)가 GenericSlabAllocator로 힙 할당하고
// AhciCommandHandler(소비자, onExec/onCancel 양쪽)가 해제한다 -
// "생성/해제 전부 처리기 책임"(async_task.h) 원칙을 이 드라이버 전체를
// 하나의 처리기로 보고 그대로 지킨다.
struct AhciCommandArgs {
    AhciPort* port = nullptr;
    uint64_t portRegBase = 0;
    uint32_t slotIndex = 0;
    bool useNcq = false;
    bool isRead = false;    // true=READ(완료 후 dataVirtAddr->callerBuf로 복사)
    bool hasData = false;
    uint64_t dataVirtAddr = 0;
    uint32_t dataHandle = 0;
    uint32_t cmdTableHandle = 0;
    void* callerBuf = nullptr;  // READ일 때만 사용
    uint64_t byteCount = 0;
    fs::BlockIoResult* outResult = nullptr;
};

class AhciCommandHandler : public kernel::AsyncTaskHandler {
public:
    kernel::AsyncExecCoro onExec(kernel::AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<AhciCommandArgs*>(argsRaw);
        const uint32_t slotBit = 1u << args->slotIndex;
        bool ioError = false;
        for (;;) {
            const uint32_t pending = args->useNcq ? (*kReg32(args->portRegBase, kPortSact) & slotBit)
                                                   : (*kReg32(args->portRegBase, kPortCi) & slotBit);
            if (!pending) {
                break;
            }
            const uint32_t tfd = *kReg32(args->portRegBase, kPortTfd);
            if (tfd & kPortTfdErr) {
                ioError = true;
                break;
            }
            // [수정, 2026-09-22, PN-A0CEF82D/QU-CC8A31F6] 이 onExec은
            // 진짜 코루틴이라(co_return이 있어 컴파일러가 코루틴으로
            // 변환) raw AsyncTask::yield()(kContextSwitch 기반, 전용
            // 스택 필요)를 여기서 호출하면 실측으로 확인된 무한
            // 대기가 발생했다 - 코루틴 전용 짝인 AsyncTaskCoroYield로
            // 교체(async_task.h 문서 주석 참고, 매 반복 레지스터를
            // 다시 읽어야 하는 이 패턴에 정확히 맞는 프리미티브).
            co_await kernel::AsyncTaskCoroYield{};
        }

        if (!ioError && args->isRead && args->hasData) {
            memcpy(args->callerBuf, reinterpret_cast<const void*>(args->dataVirtAddr), args->byteCount);
        }
        if (args->hasData) {
            kFreeDma(args->dataHandle);
        }
        kFreeDma(args->cmdTableHandle);
        args->port->releaseSlot(args->slotIndex);
        args->outResult->ok = !ioError;
        kernel::GenericSlabAllocator::free(args, sizeof(AhciCommandArgs));
        co_return;
    }
    void onFailure(kernel::AsyncTask*) override {}
    // [PN-A401DDF9] 소유 Task(사실상 fs 자신, essential KernelService라
    // Kill로 죽지 않음 - kFinalizeProcessTermination 문서 참고)가 완료
    // 전에 취소되는 극단적 경로 대비 - 진행 중인 하드웨어 커맨드 자체를
    // 어보트할 표준 절차는 이번 증분 범위 밖이라(§3.4 핫플러그/에러
    // 복구와 함께 후속) 최소한 자원 누수만 막는다. outResult는 호출부가
    // 이미 사라졌을 수 있어 건드리지 않는다.
    void onCancel(kernel::AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<AhciCommandArgs*>(argsRaw);
        if (args->hasData) {
            kFreeDma(args->dataHandle);
        }
        kFreeDma(args->cmdTableHandle);
        args->port->releaseSlot(args->slotIndex);
        kernel::GenericSlabAllocator::free(args, sizeof(AhciCommandArgs));
    }
};

AhciCommandHandler gAhciCommandHandler;
kernel::AsyncTaskSubjectCode gAhciCommandSubjectCode = 0;
bool gAhciCommandHandlerRegistered = false;

void kEnsureAhciCommandHandlerRegistered() {
    if (!gAhciCommandHandlerRegistered) {
        gAhciCommandSubjectCode = kernel::AsyncCallbackRegistry::registerHandler(&gAhciCommandHandler);
        gAhciCommandHandlerRegistered = true;
    }
}

}  // namespace

bool AhciPort::init(uint64_t hbaVirtAddr, uint32_t portIndex, uint32_t slotCount, bool use32BitDma) {
    _portRegBase = hbaVirtAddr + kPortRegionBase + static_cast<uint64_t>(portIndex) * kPortRegionStride;
    _slotCount = slotCount;
    _use32BitDma = use32BitDma;

    // §3.3.7 포트 시작 절차(사양) - PxCLB/PxFB를 새로 채우기 전에 이미
    // 실행 중이면 먼저 정지시킨다(부팅 직후라 보통 이미 꺼져 있지만
    // 방어적으로 확인).
    volatile uint32_t* cmd = kReg32(_portRegBase, kPortCmd);
    if (*cmd & (kPortCmdSt | kPortCmdFre)) {
        *cmd &= ~static_cast<uint32_t>(kPortCmdSt | kPortCmdFre);
        for (uint32_t i = 0; i < kPollIterations; ++i) {
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
    *kReg32(_portRegBase, kPortClb) = static_cast<uint32_t>(clb.physAddr & 0xFFFFFFFFu);
    *kReg32(_portRegBase, kPortClbu) = static_cast<uint32_t>(clb.physAddr >> 32);
    *kReg32(_portRegBase, kPortFb) = static_cast<uint32_t>(fb.physAddr & 0xFFFFFFFFu);
    *kReg32(_portRegBase, kPortFbu) = static_cast<uint32_t>(fb.physAddr >> 32);

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

// [SP-C2670F69 §3.1] Register H2D FIS + (dataBytes>0이면) PRDT 엔트리
// 1개 + 커맨드 헤더(슬롯 free-list에서 빌린 슬롯 하나)를 구성해 발급하고
// 완료까지 폴링한다 - IDENTIFY DEVICE/READ DMA EXT/WRITE DMA EXT/FLUSH
// CACHE EXT 전부 이 골격 하나로 표현된다(ATA 사양 §7 각 커맨드가 공통으로
// 쓰는 Register H2D FIS 포맷 덕분). lba/sectorCount가 무의미한 커맨드
// (IDENTIFY/FLUSH 등)는 0으로 넘기면 된다. dataBytes==0이면 데이터 전송이
// 없는 커맨드(FLUSH)로 간주해 PRDT 자체를 생략한다(PRDTL=0).
// [갱신, 2026-09-22, PN-A401DDF9] 이 함수는 항상 슬롯 0을 하드코딩했으나,
// submitAtaCommand()가 도입한 슬롯 free-list와 같은 하드웨어 자원(커맨드
// 리스트/PxCI)을 공유하므로 - 이 함수(동기 경로: probeWithIdentify/
// flushCache)도 acquireSlot()/releaseSlot()으로 슬롯을 빌려야 한다.
// 안 그러면 flushCache()가 진행 중인 비동기 READ/WRITE와 같은 슬롯을
// 동시에 덮어쓸 수 있다.
bool AhciPort::issueAtaCommand(uint8_t command, uint64_t lba, uint32_t sectorCount, bool isWrite,
                                uint64_t dataPhysAddr, uint32_t dataBytes) {
    const uint32_t slotIndex = acquireSlot();
    if (slotIndex == kAhciInvalidSlot) {
        return false;  // 모든 슬롯이 진행 중인 비동기 커맨드로 사용 중
    }

    DmaAlloc cmdTable;
    if (!kAllocDma(kDmaPageSize, _use32BitDma, &cmdTable)) {
        releaseSlot(slotIndex);
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
    fis->lba0 = static_cast<uint8_t>(lba & 0xFF);
    fis->lba1 = static_cast<uint8_t>((lba >> 8) & 0xFF);
    fis->lba2 = static_cast<uint8_t>((lba >> 16) & 0xFF);
    fis->lba3 = static_cast<uint8_t>((lba >> 24) & 0xFF);
    fis->lba4 = static_cast<uint8_t>((lba >> 32) & 0xFF);
    fis->lba5 = static_cast<uint8_t>((lba >> 40) & 0xFF);
    fis->countLow = static_cast<uint8_t>(sectorCount & 0xFF);
    fis->countHigh = static_cast<uint8_t>((sectorCount >> 8) & 0xFF);

    const bool hasData = dataBytes > 0;
    if (hasData) {
        // PRDT 엔트리 1개(오프셋 0x80) - dw3의 byte count 필드는 "실제
        // 길이-1"(사양 §4.2.3.3).
        auto* prdt = reinterpret_cast<PrdtEntry*>(cmdTable.virtAddr + kCmdTablePrdtOffset);
        prdt[0] = PrdtEntry{};
        prdt[0].dbaLow = static_cast<uint32_t>(dataPhysAddr & 0xFFFFFFFFu);
        prdt[0].dbaHigh = static_cast<uint32_t>(dataPhysAddr >> 32);
        prdt[0].dw3 = dataBytes - 1;  // 인터럽트 비트(I)는 안 씀(폴링 방식)
    }

    // 커맨드 헤더(위에서 빌린 슬롯) - CFL은 DWORD 단위 FIS 길이(20바이트/
    // 4=5), PRDTL은 데이터 전송이 있을 때만 1, W는 전송 방향(호스트->
    // 장치면 1).
    auto* header = reinterpret_cast<CommandHeader*>(_clbVirtAddr) + slotIndex;
    *header = CommandHeader{};
    header->dw0 = 5u;  // CFL=5
    if (isWrite) {
        header->dw0 |= (1u << 6);  // W
    }
    if (hasData) {
        header->dw0 |= (1u << 16);  // PRDTL=1
    }
    header->ctbaLow = static_cast<uint32_t>(cmdTable.physAddr & 0xFFFFFFFFu);
    header->ctbaHigh = static_cast<uint32_t>(cmdTable.physAddr >> 32);

    // 이 슬롯의 이전 오류 상태를 비우고(PxSERR write-1-to-clear) 발급한다
    // (해당 슬롯의 PxCI 비트 세팅, 사양 §5.5).
    const uint32_t slotBit = 1u << slotIndex;
    *kReg32(_portRegBase, kPortSerr) = 0xFFFFFFFFu;
    *kReg32(_portRegBase, kPortCi) |= slotBit;

    // 완료 폴링 - 이 슬롯의 PxCI 비트가 하드웨어에 의해 클리어되면
    // 발급된 커맨드가 완료된 것이다(§5.5, 인터럽트 미배선이라 스핀
    // 폴링). 그 사이 PxTFD.ERR/BSY로 오류를 함께 감시한다(§3.4의
    // "포트 오류는 TFD로 감지" 원칙 그대로 - COMRESET 등 전체 오류
    // 복구 절차는 이 v1 최소 검증 범위 밖).
    bool completed = false;
    bool ioError = false;
    for (uint32_t i = 0; i < kPollIterations; ++i) {
        const uint32_t ci = *kReg32(_portRegBase, kPortCi);
        if (!(ci & slotBit)) {
            completed = true;
            break;
        }
        const uint32_t tfd = *kReg32(_portRegBase, kPortTfd);
        if (tfd & kPortTfdErr) {
            ioError = true;
            break;
        }
    }

    kFreeDma(cmdTable.handle);
    releaseSlot(slotIndex);
    return completed && !ioError;
}

bool AhciPort::probeWithIdentify(PortProbeResult* outResult) {
    *outResult = PortProbeResult{};

    volatile uint32_t* ssts = kReg32(_portRegBase, kPortSsts);
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
        auto* words = reinterpret_cast<uint16_t*>(identifyBuf.virtAddr);
        for (uint32_t i = 0; i < 256; ++i) {
            outResult->identifyData[i] = words[i];
        }
        outResult->identifySucceeded = true;
    }

    kFreeDma(identifyBuf.handle);
    return outResult->identifySucceeded;
}

// [PN-A401DDF9, SP-C2670F69 §3.5] IDENTIFY 워드76 bit8(NCQ 지원)/워드75
// bits4:0(장치 큐 깊이-1)로 이 포트가 실제로 몇 개의 슬롯을 동시에 쓸 수
// 있는지 확정한다. 미지원이면 기존과 동일하게 1개(슬롯 0)로 유지된다 -
// ATA 프로토콜 자체가 비-NCQ 커맨드의 동시 실행을 허용하지 않기 때문에
// (여러 슬롯에 동시에 비-NCQ 커맨드를 발급해도 장치가 하나씩만 처리한다는
// 보장이 없음) 별도 분기가 아니라 이 값 하나로 자연스럽게 좁힌다.
void AhciPort::configureNcq(const uint16_t* identifyData) {
    constexpr uint32_t kIdWordQueueDepth = 75;        // bits4:0 = 큐 깊이 - 1
    constexpr uint32_t kIdWordSataCapabilities = 76;  // bit8 = NCQ 지원

    _ncqSupported = (identifyData[kIdWordSataCapabilities] & (1u << 8)) != 0;
    if (!_ncqSupported) {
        _usableSlotCount = 1;
        return;
    }
    const uint32_t deviceDepth = (identifyData[kIdWordQueueDepth] & 0x1Fu) + 1u;
    uint32_t usable = deviceDepth < _slotCount ? deviceDepth : _slotCount;
    if (usable == 0) {
        usable = 1;  // 방어적 - IDENTIFY가 이상값을 준 경우도 슬롯 0은 항상 쓸 수 있어야 함
    }
    if (usable > kMaxCommandSlots) {
        usable = kMaxCommandSlots;
    }
    _usableSlotCount = usable;
}

uint32_t AhciPort::acquireSlot() {
    for (uint32_t i = 0; i < _usableSlotCount; ++i) {
        if (!_slotUsed[i]) {
            _slotUsed[i] = true;
            return i;
        }
    }
    return kAhciInvalidSlot;
}

void AhciPort::releaseSlot(uint32_t slotIndex) {
    if (slotIndex < kMaxCommandSlots) {
        _slotUsed[slotIndex] = false;
    }
}

// [PN-A401DDF9, SP-C2670F69 §3.5] submitReadSectors/submitWriteSectors가
// 공유하는 실제 비동기 발급 로직 - issueAtaCommand()와 "FIS/PRDT/커맨드
// 헤더 구성" 자체는 겹치지만, NCQ 커맨드(FPDMA QUEUED)는 필드 배치가
// 달라(Sector Count가 Features로, Count 필드는 태그로) 별도로 구현한다.
// 슬롯을 빌리고 DMA 버퍼를 확보해 레지스터까지 세팅한 뒤(전부 동기,
// MMIO 왕복 몇 번뿐이라 빠름) 완료 폴링만 AhciCommandHandler(위 익명
// 네임스페이스)에게 넘기고 즉시 반환한다.
kernel::AsyncTask* AhciPort::submitAtaCommand(uint8_t command, uint64_t lba, uint32_t sectorCount, bool isWrite,
                                               bool isRead, void* callerBuf, uint32_t dataBytes,
                                               fs::BlockIoResult* outResult) {
    volatile uint32_t* ssts = kReg32(_portRegBase, kPortSsts);
    if ((*ssts & kPortSstsDetMask) != kPortSstsDetPresent) {
        return nullptr;
    }
    const uint32_t slotIndex = acquireSlot();
    if (slotIndex == kAhciInvalidSlot) {
        return nullptr;  // 동시 발급 상한 도달 - 호출부가 나중에 재시도
    }

    DmaAlloc cmdTable;
    if (!kAllocDma(kDmaPageSize, _use32BitDma, &cmdTable)) {
        releaseSlot(slotIndex);
        return nullptr;
    }
    memset(reinterpret_cast<void*>(cmdTable.virtAddr), 0, kDmaPageSize);

    const bool hasData = dataBytes > 0;
    DmaAlloc dataBuf;
    if (hasData) {
        if (!kAllocDma(dataBytes, _use32BitDma, &dataBuf)) {
            kFreeDma(cmdTable.handle);
            releaseSlot(slotIndex);
            return nullptr;
        }
        if (isWrite) {
            memcpy(reinterpret_cast<void*>(dataBuf.virtAddr), callerBuf, dataBytes);
        }
    }

    const bool useNcq =
        command == kAtaCommandReadFpdmaQueued || command == kAtaCommandWriteFpdmaQueued;

    auto* fis = reinterpret_cast<RegH2dFis*>(cmdTable.virtAddr);
    *fis = RegH2dFis{};
    fis->fisType = kFisTypeRegH2d;
    fis->pmportAndC = 0x80;
    fis->command = command;
    fis->lba0 = static_cast<uint8_t>(lba & 0xFF);
    fis->lba1 = static_cast<uint8_t>((lba >> 8) & 0xFF);
    fis->lba2 = static_cast<uint8_t>((lba >> 16) & 0xFF);
    fis->lba3 = static_cast<uint8_t>((lba >> 24) & 0xFF);
    fis->lba4 = static_cast<uint8_t>((lba >> 32) & 0xFF);
    fis->lba5 = static_cast<uint8_t>((lba >> 40) & 0xFF);
    if (useNcq) {
        // [ATA8-ACS READ/WRITE FPDMA QUEUED] Sector Count는 Features
        // 필드(0/1)로, Count 필드는 대신 커맨드 태그(이 슬롯 번호,
        // bits7:3)를 나른다 - device 레지스터 bit6(LBA)도 명시적으로
        // 세운다(비-NCQ 경로는 기존 관례대로 0 유지, 실기기 재현 불가라
        // 굳이 건드리지 않음).
        fis->device = 0x40;
        fis->features0 = static_cast<uint8_t>(sectorCount & 0xFF);
        fis->features1 = static_cast<uint8_t>((sectorCount >> 8) & 0xFF);
        fis->countLow = static_cast<uint8_t>((slotIndex << 3) & 0xF8u);
        fis->countHigh = 0;
    } else {
        fis->device = 0;
        fis->countLow = static_cast<uint8_t>(sectorCount & 0xFF);
        fis->countHigh = static_cast<uint8_t>((sectorCount >> 8) & 0xFF);
    }

    if (hasData) {
        auto* prdt = reinterpret_cast<PrdtEntry*>(cmdTable.virtAddr + kCmdTablePrdtOffset);
        prdt[0] = PrdtEntry{};
        prdt[0].dbaLow = static_cast<uint32_t>(dataBuf.physAddr & 0xFFFFFFFFu);
        prdt[0].dbaHigh = static_cast<uint32_t>(dataBuf.physAddr >> 32);
        prdt[0].dw3 = dataBytes - 1;
    }

    auto* header = reinterpret_cast<CommandHeader*>(_clbVirtAddr) + slotIndex;
    *header = CommandHeader{};
    header->dw0 = 5u;
    if (isWrite) {
        header->dw0 |= (1u << 6);
    }
    if (hasData) {
        header->dw0 |= (1u << 16);
    }
    header->ctbaLow = static_cast<uint32_t>(cmdTable.physAddr & 0xFFFFFFFFu);
    header->ctbaHigh = static_cast<uint32_t>(cmdTable.physAddr >> 32);

    *kReg32(_portRegBase, kPortSerr) = 0xFFFFFFFFu;
    const uint32_t slotBit = 1u << slotIndex;
    if (useNcq) {
        // 사양 순서 - PxSACT를 PxCI보다 먼저 세운다.
        *kReg32(_portRegBase, kPortSact) |= slotBit;
    }
    *kReg32(_portRegBase, kPortCi) |= slotBit;

    auto* args = static_cast<AhciCommandArgs*>(kernel::GenericSlabAllocator::alloc(sizeof(AhciCommandArgs)));
    if (!args) {
        // 슬랩 고갈 - 이미 하드웨어에 발급된 커맨드를 되돌릴 표준 절차가
        // 없어(어보트는 이번 증분 범위 밖) 이 슬롯은 안전을 위해 반납하지
        // 않고 그냥 잃는다(release하면 다음 acquireSlot()이 아직 진행
        // 중인 이 커맨드와 같은 슬롯을 다른 요청에 내줘 커맨드 리스트를
        // 덮어쓰는 훨씬 심각한 손상으로 이어질 수 있음) - 극히 드문
        // 경로(GenericSlabAllocator 완전 고갈)라 v1은 이 손실을 감수한다.
        return nullptr;
    }
    args->port = this;
    args->portRegBase = _portRegBase;
    args->slotIndex = slotIndex;
    args->useNcq = useNcq;
    args->isRead = isRead;
    args->hasData = hasData;
    args->dataVirtAddr = dataBuf.virtAddr;
    args->dataHandle = dataBuf.handle;
    args->cmdTableHandle = cmdTable.handle;
    args->callerBuf = callerBuf;
    args->byteCount = dataBytes;
    args->outResult = outResult;

    kEnsureAhciCommandHandlerRegistered();
    return kernel::AsyncTask::submit(gAhciCommandSubjectCode, 0, args, /*autoFree=*/false);
}

kernel::AsyncTask* AhciPort::submitReadSectors(uint64_t lba, uint32_t count, void* outBuf,
                                                fs::BlockIoResult* outResult) {
    if (count == 0) {
        return nullptr;
    }
    const uint64_t bytes = static_cast<uint64_t>(count) * 512u;
    const uint8_t command = _ncqSupported ? kAtaCommandReadFpdmaQueued : kAtaCommandReadDmaExt;
    return submitAtaCommand(command, lba, count, /*isWrite=*/false, /*isRead=*/true, outBuf,
                             static_cast<uint32_t>(bytes), outResult);
}

kernel::AsyncTask* AhciPort::submitWriteSectors(uint64_t lba, uint32_t count, const void* buf,
                                                 fs::BlockIoResult* outResult) {
    if (count == 0) {
        return nullptr;
    }
    const uint64_t bytes = static_cast<uint64_t>(count) * 512u;
    const uint8_t command = _ncqSupported ? kAtaCommandWriteFpdmaQueued : kAtaCommandWriteDmaExt;
    // submitAtaCommand의 callerBuf는 WRITE일 때 그 자리에서 읽기만
    // 하므로(발급 전 데이터 버퍼로 복사) const_cast가 안전하다.
    return submitAtaCommand(command, lba, count, /*isWrite=*/true, /*isRead=*/false, const_cast<void*>(buf),
                             static_cast<uint32_t>(bytes), outResult);
}

bool AhciPort::flushCache() {
    volatile uint32_t* ssts = kReg32(_portRegBase, kPortSsts);
    if ((*ssts & kPortSstsDetMask) != kPortSstsDetPresent) {
        return false;
    }
    // FLUSH CACHE EXT(0xEA) - 데이터 전송이 없는 커맨드(dataBytes=0).
    return issueAtaCommand(kAtaCommandFlushCacheExt, 0, 0, false, 0, 0);
}

bool AhciController::init(uint64_t mmioVirtAddr) {
    _mmioVirtAddr = mmioVirtAddr;

    volatile uint32_t* ghc = kReg32(mmioVirtAddr, kRegGhc);
    *ghc |= kGhcAe;

    const uint32_t cap = *kReg32(mmioVirtAddr, kRegCap);
    const uint32_t numPorts = (cap & kCapNpMask) + 1;
    _slotCount = ((cap >> kCapNcsShift) & kCapNcsMask) + 1;
    const bool use32BitDma = (cap & kCapS64a) == 0;

    _portsImplemented = *kReg32(mmioVirtAddr, kRegPi);

    for (uint32_t i = 0; i < numPorts && i < 32; ++i) {
        if (!(_portsImplemented & (1u << i))) {
            continue;
        }
        _portInitialized[i] = _ports[i].init(mmioVirtAddr, i, _slotCount, use32BitDma);
    }
    return true;
}

bool AhciController::probeFirstDevice(PortProbeResult* outResult, AhciPort** outPort) {
    for (uint32_t i = 0; i < 32; ++i) {
        if (!_portInitialized[i]) {
            continue;
        }
        if (_ports[i].probeWithIdentify(outResult)) {
            *outPort = &_ports[i];
            return true;
        }
    }
    return false;
}

namespace {

// ATA-8 ACS IDENTIFY DEVICE 워드 위치(전부 사양 표준 - 커널/AHCI
// 고유 값 아님).
constexpr uint32_t kIdWordLba28Low = 60;
constexpr uint32_t kIdWordLba28High = 61;
constexpr uint32_t kIdWordLba48Bit = 83;   // bit10 - LBA48 지원 여부
constexpr uint32_t kIdWordLba48Base = 100;  // 100-103, 64비트 리틀엔디안 워드
constexpr uint32_t kIdWordPhysLogicalSector = 106;  // bit14=1(유효), bit12=1(논리섹터>256워드)
constexpr uint32_t kIdWordLogicalSectorSizeLow = 117;
constexpr uint32_t kIdWordLogicalSectorSizeHigh = 118;

}  // namespace

void AhciBlockDevice::init(AhciPort* port, const uint16_t* identifyData) {
    _port = port;

    // §3.0 "blockSize() - 보통 512 또는 4096" - word106 bit14(유효 비트)
    // +bit12(논리 섹터가 256워드/512바이트보다 큼)가 둘 다 설 때만
    // words117-118(32비트, 워드 단위)을 실제 크기로 쓴다. 아니면 표준
    // 512바이트 섹터(사양 기본값)로 남긴다.
    _blockSize = 512;
    const uint16_t physLogical = identifyData[kIdWordPhysLogicalSector];
    if ((physLogical & (1u << 14)) && (physLogical & (1u << 12))) {
        const uint32_t sectorWords = static_cast<uint32_t>(identifyData[kIdWordLogicalSectorSizeLow]) |
                                          (static_cast<uint32_t>(identifyData[kIdWordLogicalSectorSizeHigh]) << 16);
        if (sectorWords > 0) {
            _blockSize = sectorWords * 2;
        }
    }

    // §3.0 "blockCount()" - LBA48을 지원하면 words100-103(64비트), 아니면
    // words60-61(32비트, LBA28)로 계산한다(ATA-8 ACS 표준 필드).
    if (identifyData[kIdWordLba48Bit] & (1u << 10)) {
        _blockCount = static_cast<uint64_t>(identifyData[kIdWordLba48Base]) |
                      (static_cast<uint64_t>(identifyData[kIdWordLba48Base + 1]) << 16) |
                      (static_cast<uint64_t>(identifyData[kIdWordLba48Base + 2]) << 32) |
                      (static_cast<uint64_t>(identifyData[kIdWordLba48Base + 3]) << 48);
    } else {
        _blockCount = static_cast<uint64_t>(identifyData[kIdWordLba28Low]) |
                      (static_cast<uint64_t>(identifyData[kIdWordLba28High]) << 16);
    }

    // [PN-A401DDF9, SP-C2670F69 §3.5] blockSize/blockCount와 같은
    // IDENTIFY 응답에서 NCQ 지원 여부/큐 깊이도 이 시점에 확정한다.
    _port->configureNcq(identifyData);
}

kernel::AsyncTask* AhciBlockDevice::submitReadBlocks(uint64_t lba, void* buf, uint32_t count,
                                                      fs::BlockIoResult* outResult) {
    return _port->submitReadSectors(lba, count, buf, outResult);
}

kernel::AsyncTask* AhciBlockDevice::submitWriteBlocks(uint64_t lba, const void* buf, uint32_t count,
                                                       fs::BlockIoResult* outResult) {
    return _port->submitWriteSectors(lba, count, buf, outResult);
}

bool AhciBlockDevice::flush() { return _port->flushCache(); }

bool AhciBlockDevice::trim(uint64_t, uint32_t) { return true; }

}  // namespace ahci
