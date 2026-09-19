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

class DmaBufferService {
public:
    // 부팅 시 한 번 호출 - AllocDmaBuffer/FreeDmaBuffer endpoint를 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_DMA_BUFFER_H
