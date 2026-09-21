#ifndef MINICORE_KERNEL_DMA_BUFFER_H
#define MINICORE_KERNEL_DMA_BUFFER_H

#include "channel.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

// [SP-39F18E30 §2] devmgr(또는 devmgr이 fork()로 스폰한 드라이버 자식,
// AHCI(SP-C2670F69 §3.2)/USB(SP-E35FD36C §3.2)가 공통으로 쓴다)가
// 컨트롤러의 DMA 구조체(Command List/FIS/PRDT 등)를 담을, 물리적으로
// 연속인 메모리를 확보하는 syscall - devmgr 프로세스는 자기 페이지의
// 물리 주소를 직접 조회할 수 없으므로(SP-8B6B8D25 §2-A 원칙), 커널이
// 대신 확보해 물리 주소와 함께 내준다.
struct AllocDmaBufferArgs {
    uint64_t sizeBytes = 0;      // in: 4KiB 페이지 단위로 올림
    uint32_t physAddrLimit = 0;  // in: 0=제한 없음(64비트 전역), 32=하위
                                  //     4GiB 이내로 강제(레거시 UHCI/OHCI/
                                  //     EHCI용, §5)
    // out
    // ResourceExhausted(페이지 고갈 또는 주소공간 매핑 실패) /
    // InvalidArgument(sizeBytes==0) / InvalidHandle(제출자를 못 찾음) -
    // 전부 기존 ChannelError 재사용(RequestIoPermissionArgs와 동일한
    // 선례, pnp.h 문서 주석 참고).
    ChannelError error = ChannelError::None;
    uint64_t virtualAddr = 0;   // devmgr(또는 그 드라이버 자식) 프로세스 주소공간에 매핑된 가상주소(페이지 정렬)
    uint64_t physicalAddr = 0;  // 컨트롤러 DMA 구조체에 그대로 채워 넣을 물리주소(시작점, 물리적으로 연속)
    uint32_t handle = 0;        // FreeDmaBuffer/프로세스 종료 시 정리용 식별자
};

struct FreeDmaBufferArgs {
    uint32_t handle = 0;
    // out
    ChannelError error = ChannelError::None;  // InvalidHandle(이미 반납됐거나 존재하지 않음, 또는 호출자 소유가 아님)
};

// [갱신, 2026-09-17, SP-E9B44929] Device 그룹(2) - RequestIoPermission(1)
// 다음 call 번호(RM-48E1E610 "그룹 2 - Device" 참고, 번호는 이미 예약돼
// 있었다).
constexpr SyscallEndpointId kSyscallEndpointAllocDmaBuffer = kMakeSyscallEndpointId(2, 2);
constexpr SyscallEndpointId kSyscallEndpointFreeDmaBuffer = kMakeSyscallEndpointId(2, 3);

// [신규, 2026-09-20, SP-43331889 §7(fs 전환), 설계자 지시(QU-5FC58B06
// 답변 - "전부 옮긴 후에 맵핑을 후순위로 미뤄")] `AllocDmaBufferHandler`/
// `FreeDmaBufferHandler` 본문(dma_buffer.cpp) - `kEnumerateDevicesSync`
// 와 같은 이유로 익명 네임스페이스 밖으로 뺐다.
//
// [구현, 2026-09-22, PN-A8BE8BED 항목3, QU-0C2CB097 답변] KernelThread
// 호출자(devmgr/fs)를 위한 커널 모드 DMA 버퍼 매핑을 실제로 구현했다 -
// `physAddrLimit`(4GiB 미만 강제)까지 감안하면 devmgr의
// RequestIoPermission처럼 "고정 슬롯 하나"로는 부족하고(AHCI가 명령
// 마다 커맨드 테이블+데이터 버퍼를 새로 할당) physmap 직접 재사용도
// 캐시 일관성 문제가 있어(둘 다 dma_buffer.cpp 파일 서두 주석 참고),
// 유저모드 경로와 구조적으로 대응하는 전용 가상주소 스크래치 할당자
// (`kKernelDmaScratchVirtBase`, dma_buffer.cpp)로 구현했다.
void kAllocDmaBufferSync(const SharedPtr<Task>& caller, uint64_t sizeBytes, uint32_t physAddrLimit,
                          uint64_t* outVirtualAddr, uint64_t* outPhysicalAddr, uint32_t* outHandle,
                          ChannelError* outError);
void kFreeDmaBufferSync(const SharedPtr<Task>& caller, uint32_t handle, ChannelError* outError);

class DmaBufferService {
public:
    // 부팅 시 한 번 호출 - AllocDmaBuffer/FreeDmaBuffer endpoint를 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_DMA_BUFFER_H
