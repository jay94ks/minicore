#ifndef MINICORE_FS_AHCI_H
#define MINICORE_FS_AHCI_H

#include "block_device.h"
#include "libmc/types.h"

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

namespace ahci {

// AHCI 1.3.1 사양 §3 - 포트당 최대 32개 커맨드 슬롯(§3.1 "슬롯
// free-list 관리 - 컨트롤러가 광고하는 슬롯 수만큼", §3.5의 NCQ 동시
// 발급에도 그대로 쓰인다는 서술과 일치하는 상한).
constexpr mc::uint32_t kMaxCommandSlots = 32;

// 이 포트가 실제로 초기화에 성공하고 장치가 붙어 있는지까지 확인했을
// 때만 채워지는, IDENTIFY DEVICE 검증 결과.
struct PortProbeResult {
    bool devicePresent = false;    // PxSSTS.DET==3(장치 있음+통신 확립)
    bool identifySucceeded = false;
    mc::uint16_t identifyData[256] = {};  // ATA IDENTIFY DEVICE 응답(워드 단위, 리틀엔디안 그대로)
};

// [SP-C2670F69 §3.1] 포트 하나 - 커맨드 리스트(1페이지)/FIS 수신
// 버퍼(1페이지)를 자신의 DMA 버퍼로 소유한다. v1은 슬롯 0 하나만
// 실제로 쓴다(§3.5 NCQ 확정 설계는 있으나, 이번 증분은 비-NCQ
// 커맨드까지가 스코프 - free-list 필드 자체는 §3.1이 요구한 대로
// slotCount만큼 둔다).
class AhciPort {
public:
    // hbaVirtAddr: 이 포트가 속한 HBA MMIO 영역의 가상주소 시작점.
    // portIndex: PI 비트마스크에서의 포트 번호(레지스터 오프셋
    // 0x100 + portIndex*0x80 계산에 씀). slotCount: AhciController가
    // CAP.NCS+1로 읽어 전달(§3.5). use32BitDma: CAP.S64A==0(컨트롤러가
    // 64비트 물리주소를 못 받음)이면 true - AllocDmaBuffer 호출마다
    // physAddrLimit=32로 강제한다(SP-39F18E30 §5-A, 레거시 대비용이지만
    // AHCI 컨트롤러도 이론상 S64A=0일 수 있어 그대로 존중).
    bool init(mc::uint64_t hbaVirtAddr, mc::uint32_t portIndex, mc::uint32_t slotCount, bool use32BitDma);

    // §3.1 "최소한의 실제 I/O" 검증 지점 - 비-NCQ IDENTIFY DEVICE(0xEC)
    // 를 슬롯 0 하나만 써서 발급하고 완료까지 폴링한다(인터럽트 미배선,
    // §3.3은 후속). 장치가 없거나(DET!=3) 발급/완료에 실패하면 false.
    bool probeWithIdentify(PortProbeResult* outResult);

    // LBA48 READ DMA EXT(0x25)/WRITE DMA EXT(0x35) - probeWithIdentify()
    // 와 동일한 슬롯0/폴링 골격을 공유한다(내부 issueAtaCommand()).
    // count는 섹터 수(512바이트 단위), outBuf/buf는 이 프로세스 자신의
    // 힙/스택 등 아무 버퍼나 가능(내부에서 DMA 가능 버퍼로 왕복 복사) -
    // 최대 전송량은 PRDT 엔트리 1개의 상한(버디 할당자 kMaxOrder=10과
    // 일치하는 4MiB-1)을 넘지 않아야 한다. 장치가 없거나 발급/완료에
    // 실패하면 false.
    bool readSectors(mc::uint64_t lba, mc::uint32_t count, void* outBuf);
    bool writeSectors(mc::uint64_t lba, mc::uint32_t count, const void* buf);

    // FLUSH CACHE EXT(0xEA) - 데이터 전송이 없는 커맨드(PRDT 없음).
    // BlockDevice::flush()가 그대로 위임한다.
    bool flushCache();

private:
    // probeWithIdentify/readSectors/writeSectors/flushCache가 공유하는
    // 실제 발급+폴링 로직 - command/lba/sectorCount로 Register H2D
    // FIS를 채우고(IDENTIFY처럼 lba/count가 무의미한 커맨드는 0으로
    // 넘기면 됨), dataBytes>0이면 dataPhysAddr를 가리키는 PRDT 엔트리
    // 1개를 구성해 슬롯 0으로 발급한다 - dataBytes==0이면 데이터
    // 전송이 없는 커맨드(FLUSH 등)로 간주해 PRDT 자체를 생략한다.
    // isWrite는 커맨드 헤더 W 비트(전송 방향)에만 반영 - 실제 데이터를
    // 그 방향으로 복사하는 책임은 호출부에 있다.
    bool issueAtaCommand(mc::uint8_t command, mc::uint64_t lba, mc::uint32_t sectorCount, bool isWrite,
                          mc::uint64_t dataPhysAddr, mc::uint32_t dataBytes);

    mc::uint64_t _portRegBase = 0;   // 이 포트의 레지스터 블록 시작 가상주소
    mc::uint64_t _clbVirtAddr = 0;   // 커맨드 리스트 가상주소(1페이지)
    mc::uint64_t _fbVirtAddr = 0;    // FIS 수신 버퍼 가상주소(1페이지)
    mc::uint32_t _clbDmaHandle = 0;
    mc::uint32_t _fbDmaHandle = 0;
    mc::uint32_t _slotCount = 0;
    bool _use32BitDma = false;  // CAP.S64A==0이면 커맨드 구조체도 하위 4GiB 이내로 강제(§5-A)
};

// [SP-C2670F69 §3.1] HBA 초기화 - GHC.AE 설정, CAP으로 포트/슬롯 수
// 확인, PI 비트마스크로 실제 존재하는 포트만 AhciPort로 구성한다.
class AhciController {
public:
    bool init(mc::uint64_t mmioVirtAddr);

    // 초기화된 포트 중 실제로 장치가 붙어 있는(DET==3) 첫 번째 포트를
    // 찾아 IDENTIFY DEVICE까지 실행한다. 성공하면 outPort에 그 포트를
    // 가리키는 포인터를 채운다(수명은 이 AhciController 자신과 같음) -
    // AhciBlockDevice::init()에 그대로 넘기면 된다. 못 찾으면 false.
    bool probeFirstDevice(PortProbeResult* outResult, AhciPort** outPort);

private:
    mc::uint64_t _mmioVirtAddr = 0;
    mc::uint32_t _slotCount = 0;
    mc::uint32_t _portsImplemented = 0;  // PI 비트마스크 그대로
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
    void init(AhciPort* port, const mc::uint16_t* identifyData);

    mc::uint32_t blockSize() const override { return _blockSize; }
    mc::uint64_t blockCount() const override { return _blockCount; }
    bool readBlocks(mc::uint64_t lba, mc::uint32_t count, void* buf) override;
    bool writeBlocks(mc::uint64_t lba, mc::uint32_t count, const void* buf) override;
    bool flush() override;
    // [v1] TRIM(DATA SET MANAGEMENT) 미구현 - BlockDevice 문서 주석이
    // 명시한 대로 미지원 장치는 그냥 true(성공)를 반환해도 데이터
    // 정확성에 영향이 없다(최적화 힌트일 뿐).
    bool trim(mc::uint64_t lba, mc::uint32_t count) override;

private:
    AhciPort* _port = nullptr;
    mc::uint32_t _blockSize = 512;
    mc::uint64_t _blockCount = 0;
};

}  // namespace ahci

#endif  // MINICORE_FS_AHCI_H
