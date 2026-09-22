#ifndef MINICORE_KERNEL_AHCI_H
#define MINICORE_KERNEL_AHCI_H

#include "block_device.h"
#include "libkenv/types.h"

namespace kernel {
struct AsyncTask;
}  // namespace kernel

// SP-C2670F69 §3.1-§3.5 - AHCI(SATA 스토리지 컨트롤러) 드라이버.
// **[뒤집힘, 2026-09-20, QU-1FB6A7A4 답변 - "블록 디바이스는 그냥
// 아예 fs한테 던져버려. 인식/인식 해제까지 전부."]** 원래 devmgr이
// fork()로 스폰한 드라이버 자식 안에서 실행하도록 설계됐었으나,
// 그 자식이 개설한 이름 없는 Channel을 fs가 발견할 방법이 없다는
// 설계 공백(QU-1FB6A7A4)이 실제로 발견돼 - devmgr을 거치지 않고
// **fs 프로세스 자신**이 PCI 열거/매칭부터 HBA 초기화까지 전부
// 직접 수행하는 것으로 재확정됐다. `minicore/devmgr`에서 여기로
// 옮겨 왔다(PN-4E6EA13D/PN-F60E405A A가 만든 것과 로직은 동일 -
// 파일 위치와 소비자만 바뀌었다).
// **[전환, 2026-09-20, SP-43331889/QU-5FC58B06] fs가 Process 없는
// 커널 KernelThread로 흡수되며 `mc::` -> `kernel::` 타입 전환** -
// AllocDmaBuffer의 커널 모드 매핑 설계는 아직 미정이라(dma_buffer.h
// 문서 주석 참고) 이 파일이 쓰는 DMA 버퍼 alloc/free는 당분간
// 전부 `ChannelError::NotSupported`로 실패한다 - 즉 이 드라이버의
// 구조는 완전하지만 실제 장치 인식(IDENTIFY 등 DMA가 필요한 모든
// 명령)은 그 설계가 결정되기 전까지 실패로 우아하게 되돌아간다.

namespace ahci {

// AHCI 1.3.1 사양 §3 - 포트당 최대 32개 커맨드 슬롯(§3.1 "슬롯
// free-list 관리 - 컨트롤러가 광고하는 슬롯 수만큼", §3.5의 NCQ 동시
// 발급에도 그대로 쓰인다는 서술과 일치하는 상한).
constexpr kernel::uint32_t kMaxCommandSlots = 32;

// 이 포트가 실제로 초기화에 성공하고 장치가 붙어 있는지까지 확인했을
// 때만 채워지는, IDENTIFY DEVICE 검증 결과.
struct PortProbeResult {
    bool devicePresent = false;    // PxSSTS.DET==3(장치 있음+통신 확립)
    bool identifySucceeded = false;
    kernel::uint16_t identifyData[256] = {};  // ATA IDENTIFY DEVICE 응답(워드 단위, 리틀엔디안 그대로)
};

// [SP-C2670F69 §3.1] 포트 하나 - 커맨드 리스트(1페이지)/FIS 수신
// 버퍼(1페이지)를 자신의 DMA 버퍼로 소유한다.
// **[갱신, 2026-09-22, PN-A401DDF9] §3.5 NCQ 실제 구현** - 슬롯
// free-list(`_slotUsed`)로 여러 커맨드를 동시에 in-flight로 관리한다.
// 장치가 NCQ를 지원하면(`configureNcq()`가 IDENTIFY 워드76/75로 판별)
// READ/WRITE FPDMA QUEUED(0x60/0x61)를 여러 슬롯에 동시 발급하고,
// 미지원이면 기존과 동일하게 슬롯 1개(0번)로 자연히 좁혀진다(ATA
// 프로토콜 자체가 비-NCQ 커맨드의 동시 실행을 허용하지 않으므로 -
// 별도 분기가 아니라 `_usableSlotCount=1`로 표현).
class AhciPort {
public:
    // hbaVirtAddr: 이 포트가 속한 HBA MMIO 영역의 가상주소 시작점.
    // portIndex: PI 비트마스크에서의 포트 번호(레지스터 오프셋
    // 0x100 + portIndex*0x80 계산에 씀). slotCount: AhciController가
    // CAP.NCS+1로 읽어 전달(§3.5). use32BitDma: CAP.S64A==0(컨트롤러가
    // 64비트 물리주소를 못 받음)이면 true - AllocDmaBuffer 호출마다
    // physAddrLimit=32로 강제한다(SP-39F18E30 §5-A, 레거시 대비용이지만
    // AHCI 컨트롤러도 이론상 S64A=0일 수 있어 그대로 존중).
    // [갱신, 2026-09-22, PN-FFFE892E] irqVector - fs.cpp의
    // `kRequestIoPermissionSync()`가 배정한 MSI 벡터(0이면 미배정 -
    // MSI capability가 없거나 실패, 폴링만 가능). AhciCommandHandler가
    // 완료 대기에 이 벡터로 WaitInterrupt를 건다.
    bool init(kernel::uint64_t hbaVirtAddr, kernel::uint32_t portIndex, kernel::uint32_t slotCount, bool use32BitDma,
              kernel::uint32_t irqVector);

    // §3.1 "최소한의 실제 I/O" 검증 지점 - 비-NCQ IDENTIFY DEVICE(0xEC)
    // 를 슬롯 free-list에서 하나 빌려 발급하고 완료까지 폴링한다(인터럽트
    // 미배선, §3.3은 후속). 장치가 없거나(DET!=3) 발급/완료에 실패하면
    // false. 이 시점엔 아직 `configureNcq()`가 안 불려 슬롯은 항상
    // 1개(0번)뿐이다.
    bool probeWithIdentify(PortProbeResult* outResult);

    // [PN-A401DDF9, SP-C2670F69 §3.5] IDENTIFY DEVICE 응답(워드76
    // bit8=NCQ 지원, 워드75 bits4:0+1=장치 큐 깊이)으로 NCQ 지원 여부와
    // 실제 사용 가능 슬롯 수(`min(CAP.NCS+1, 장치 큐 깊이)`)를 확정한다 -
    // `AhciBlockDevice::init()`이 identifyData를 받은 직후 한 번 호출.
    void configureNcq(const kernel::uint16_t* identifyData);

    // [PN-A401DDF9] LBA48 READ/WRITE(장치가 NCQ를 지원하면 FPDMA
    // QUEUED(0x60/0x61), 아니면 기존과 동일한 DMA EXT(0x25/0x35))를
    // 슬롯 free-list에서 하나 빌려 제출하고 즉시 kernel::AsyncTask*를
    // 반환한다(autoFree=false - fs::BlockDevice 문서 주석과 동일한
    // "완료까지 outResult 유효, 반납은 호출부 책임" 계약). outBuf/buf는
    // 이 프로세스 자신의 힙/스택 등 아무 버퍼나 가능(완료 시 내부
    // DMA 버퍼와 왕복 복사) - 최대 전송량은 PRDT 엔트리 1개의 상한
    // (버디 할당자 kMaxOrder=10과 일치하는 4MiB-1)을 넘지 않아야 한다.
    // 장치가 없거나, 사용 가능한 슬롯이 모두 이미 in-flight거나, DMA
    // 버퍼 확보에 실패하면 nullptr.
    kernel::AsyncTask* submitReadSectors(kernel::uint64_t lba, kernel::uint32_t count, void* outBuf,
                                          fs::BlockIoResult* outResult);
    kernel::AsyncTask* submitWriteSectors(kernel::uint64_t lba, kernel::uint32_t count, const void* buf,
                                           fs::BlockIoResult* outResult);

    // FLUSH CACHE EXT(0xEA) - 데이터 전송이 없는 커맨드(PRDT 없음).
    // BlockDevice::flush()가 그대로 위임한다(드물게 불리는 경로라
    // 당분간 동기 유지 - block_device.h QU-47203076 참고).
    bool flushCache();

    // [PN-A401DDF9] 슬롯 free-list 반납 - 익명 네임스페이스의
    // `AhciCommandHandler`(ahci.cpp, submitReadSectors/submitWriteSectors
    // 가 제출한 커맨드의 완료/취소를 처리)가 유일한 실제 호출부다 -
    // AhciPort 멤버 함수가 아니라 그 처리기의 `onExec`/`onCancel`에서
    // 불러야 해서 공개해야 한다.
    void releaseSlot(kernel::uint32_t slotIndex);

private:
    // probeWithIdentify/issueAtaCommand(동기, 슬롯 free-list에서 하나
    // 빌려 완료까지 폴링)와 submitAtaCommand(비동기, 슬롯만 빌려 즉시
    // 반환)가 공유하는 실제 "FIS+PRDT+커맨드헤더 구성 및 발급" 로직 -
    // command/lba/sectorCount로 Register H2D FIS를 채우고(IDENTIFY처럼
    // lba/count가 무의미한 커맨드는 0으로 넘기면 됨), dataBytes>0이면
    // dataPhysAddr를 가리키는 PRDT 엔트리 1개를 구성해 발급한다 -
    // dataBytes==0이면 데이터 전송이 없는 커맨드(FLUSH 등)로 간주해
    // PRDT 자체를 생략한다. isWrite는 커맨드 헤더 W 비트(전송 방향)에만
    // 반영 - 실제 데이터를 그 방향으로 복사하는 책임은 호출부에 있다.
    bool issueAtaCommand(kernel::uint8_t command, kernel::uint64_t lba, kernel::uint32_t sectorCount, bool isWrite,
                          kernel::uint64_t dataPhysAddr, kernel::uint32_t dataBytes);

    // [PN-A401DDF9] submitReadSectors/submitWriteSectors가 공유하는
    // 실제 비동기 발급 로직 - 슬롯을 빌리고 DMA 버퍼(커맨드 테이블 +
    // 데이터, 후자는 dataBytes>0일 때만)를 확보해 레지스터까지 세팅한
    // 뒤(동기, 빠름) 완료 폴링은 AsyncTaskHandler(ahci.cpp의
    // AhciCommandHandler)에게 넘기고 즉시 반환한다. isRead=true면
    // 완료 시 데이터 버퍼를 callerBuf로 복사(READ), false면 이미
    // 발급 전에 callerBuf(실제로는 const 소스)를 데이터 버퍼로
    // 복사해 둔다(WRITE) - 두 방향을 하나의 시그니처로 표현하기 위해
    // callerBuf는 항상 `void*`로 받고 WRITE 방향 복사는 이 함수
    // 자신이 그 자리에서 처리한다(호출부가 const 포인터를 넘겨도
    // 안전 - WRITE 시엔 이 함수가 읽기만 한다).
    kernel::AsyncTask* submitAtaCommand(kernel::uint8_t command, kernel::uint64_t lba, kernel::uint32_t sectorCount,
                                         bool isWrite, bool isRead, void* callerBuf, kernel::uint32_t dataBytes,
                                         fs::BlockIoResult* outResult);

    // [PN-A401DDF9, SP-C2670F69 §3.5] 슬롯 free-list - `_usableSlotCount`
    // (NCQ 미지원/미확인 시 1, 지원 시 `min(CAP.NCS+1, 장치 큐 깊이)`)
    // 범위 안에서만 빌려준다. 사용 가능한 슬롯이 없으면
    // `kAhciInvalidSlot`(ahci.cpp).
    kernel::uint32_t acquireSlot();

    kernel::uint64_t _portRegBase = 0;   // 이 포트의 레지스터 블록 시작 가상주소
    kernel::uint64_t _clbVirtAddr = 0;   // 커맨드 리스트 가상주소(1페이지)
    kernel::uint64_t _fbVirtAddr = 0;    // FIS 수신 버퍼 가상주소(1페이지)
    kernel::uint32_t _clbDmaHandle = 0;
    kernel::uint32_t _fbDmaHandle = 0;
    kernel::uint32_t _slotCount = 0;
    bool _use32BitDma = false;  // CAP.S64A==0이면 커맨드 구조체도 하위 4GiB 이내로 강제(§5-A)

    // [PN-A401DDF9] configureNcq() 전까지는 항상 1(기존 "슬롯 0 전용"과
    // 동일 동작) - IDENTIFY 응답으로 NCQ 지원이 확인되면 그때 늘어난다.
    kernel::uint32_t _usableSlotCount = 1;
    bool _ncqSupported = false;
    bool _slotUsed[kMaxCommandSlots] = {};
    // [신규, 2026-09-22, PN-FFFE892E] 0=미배정(폴링), 그 외=이 포트가
    // 속한 컨트롤러가 배정받은 MSI 벡터 - submitAtaCommand()가 발급하는
    // AhciCommandArgs에 그대로 실어 보낸다.
    kernel::uint32_t _irqVector = 0;
};

// [SP-C2670F69 §3.1] HBA 초기화 - GHC.AE 설정, CAP으로 포트/슬롯 수
// 확인, PI 비트마스크로 실제 존재하는 포트만 AhciPort로 구성한다.
class AhciController {
public:
    // [갱신, 2026-09-22, PN-FFFE892E] irqVector - AhciPort::init()으로
    // 그대로 전달한다(문서 주석 참고).
    bool init(kernel::uint64_t mmioVirtAddr, kernel::uint32_t irqVector);

    // 초기화된 포트 중 실제로 장치가 붙어 있는(DET==3) 첫 번째 포트를
    // 찾아 IDENTIFY DEVICE까지 실행한다. 성공하면 outPort에 그 포트를
    // 가리키는 포인터를 채운다(수명은 이 AhciController 자신과 같음) -
    // AhciBlockDevice::init()에 그대로 넘기면 된다. 못 찾으면 false.
    bool probeFirstDevice(PortProbeResult* outResult, AhciPort** outPort);

private:
    kernel::uint64_t _mmioVirtAddr = 0;
    kernel::uint32_t _slotCount = 0;
    kernel::uint32_t _portsImplemented = 0;  // PI 비트마스크 그대로
    AhciPort _ports[32];
    bool _portInitialized[32] = {};
};

// [SP-2BCE5D60 §3.0] AhciPort 하나를 감싸 fs 서비스의 파일시스템
// 드라이버(§3.1)가 요구하는 BlockDevice 인터페이스로 노출한다 -
// blockSize/blockCount는 probeFirstDevice()가 이미 받아 둔 IDENTIFY
// 응답에서 계산(재조회 없음).
class AhciBlockDevice : public fs::BlockDevice {
public:
    // port: AhciController::probeFirstDevice()가 채워 준 포트(이미
    // IDENTIFY 완료 상태). identifyData: 같은 호출의 PortProbeResult::
    // identifyData - 그대로 복사해 둔다.
    void init(AhciPort* port, const kernel::uint16_t* identifyData);

    kernel::uint32_t blockSize() const override { return _blockSize; }
    kernel::uint64_t blockCount() const override { return _blockCount; }
    kernel::AsyncTask* submitReadBlocks(kernel::uint64_t lba, void* buf, kernel::uint32_t count,
                                         fs::BlockIoResult* outResult) override;
    kernel::AsyncTask* submitWriteBlocks(kernel::uint64_t lba, const void* buf, kernel::uint32_t count,
                                          fs::BlockIoResult* outResult) override;
    bool flush() override;
    // [v1] TRIM(DATA SET MANAGEMENT) 미구현 - BlockDevice 문서 주석이
    // 명시한 대로 미지원 장치는 그냥 true(성공)를 반환해도 데이터
    // 정확성에 영향이 없다(최적화 힌트일 뿐).
    bool trim(kernel::uint64_t lba, kernel::uint32_t count) override;

private:
    AhciPort* _port = nullptr;
    kernel::uint32_t _blockSize = 512;
    kernel::uint64_t _blockCount = 0;
};

}  // namespace ahci

#endif  // MINICORE_KERNEL_AHCI_H
