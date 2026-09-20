#ifndef MINICORE_KERNEL_PNP_H
#define MINICORE_KERNEL_PNP_H

#include "channel.h"
#include "libkenv/types.h"
#include "syscall.h"

namespace kernel {

// 장치 자동 인식/핫플러그 프레임워크(SP-9DD4F3EA, "PnP")의 커널 쪽
// syscall 표면 - devmgr(유저랜드, PN-BD9AAE2F)이 하드웨어 토폴로지를
// 조회하는 데 쓴다. §3.2-§3.4(드라이버 매칭/IO 권한 부여/핫플러그)는
// devmgr 자신의 코드와 후속 syscall(RequestIoPermission, RM-48E1E610
// 9번, 아직 미구현)이 다룬다 - 이 파일은 §3.1(장치 열거)만 구현한다.

// [SP-9DD4F3EA §3.1] PCI 장치 하나의 열거 결과 - devmgr이 이 배열을
// 로컬에 캐시해 드라이버 매칭(§3.2)에 쓴다.
struct DeviceDescriptor {
    uint32_t bus = 0, device = 0, function = 0;
    uint32_t vendorId = 0, deviceId = 0;
    uint32_t classCode = 0, subclass = 0, progIf = 0;
    // BAR0~5 중 메모리 매핑인 것만(포트 I/O BAR는 0) - 64비트 BAR의
    // 상위 32비트를 담는 다음 슬롯은 독립된 BAR가 아니므로 0으로
    // 남는다. 아직 어떤 프로세스 주소공간에도 매핑되지 않은 물리주소
    // 그대로다(실제 가상주소 매핑/소유권 배정은 RequestIoPermission,
    // §3.3의 몫 - 이 syscall은 순수 조회만 한다).
    uint64_t mmioBases[6] = {};
    // 0이면 아직 미배정(MSI/MSI-X 협상은 RequestIoPermission 시점에
    // 이뤄진다 - 여기서는 열거만 한다).
    uint32_t irqVector = 0;
};

// [SP-9DD4F3EA §3.1] devmgr이 부팅 후 이 syscall을 반복 호출해 전체
// PCI 장치 목록을 얻는다(페이지네이션 - 결과가 커널 스택/버퍼 크기를
// 넘을 수 있음).
struct EnumerateDevicesArgs {
    // in: 몇 번째 장치부터 반환할지.
    uint32_t startIndex = 0;
    // in/out: 호출부가 준비한 배열 크기(in) / 실제로 채운 개수(out).
    uint32_t capacity = 0;
    DeviceDescriptor* outDevices = nullptr;
    // out: 전체 장치 수(다음 호출의 startIndex 계산용).
    uint32_t totalCount = 0;
    // [이 struct의 첫 실제 구현에서 추가] outDevices가 호출자 자신의
    // 유저 주소공간에 속하지 않으면 ChannelError::InvalidPointer -
    // 새 오류 종류를 만들지 않고 기존 enum을 재사용한다(SP-9DD4F3EA
    // §3.3의 RequestIoPermissionArgs도 이미 ChannelError를 그대로
    // 재사용하는 선례가 있다, PN-B552E75F가 도입한 값 그대로 재사용).
    ChannelError error = ChannelError::None;
};

// [갱신, 2026-09-17, SP-E9B44929] Device 그룹(2).
constexpr SyscallEndpointId kSyscallEndpointEnumerateDevices = kMakeSyscallEndpointId(2, 0);

// [SP-9DD4F3EA §3.3] devmgr(또는 devmgr이 스폰한 드라이버 자식,
// SP-EAB162FC §4/QU-3AAAB5E9 - 이 syscall은 role 검증을 하지 않고
// `DeviceOwnerTable` 소유권 확인만으로 충분하다고 확정됨)이 특정
// 장치의 MMIO BAR에 대한 접근 권한을 요청한다. **v1 축소 범위(다음
// 두 항목은 의도적으로 미구현 - PN-* 후속 계획으로 별도 추적)**:
// (1) MSI/MSI-X 인터럽트 벡터 배정 - `assignedIrqVector`는 항상 0을
//     반환한다(§3.1의 "0=아직 미배정"과 같은 의미), (2) BAR 실제
//     크기 조회(표준 PCI BAR sizing 절차 - "전부 1 써보고 읽어서
//     크기 역산") - 이 syscall이 요청받은 물리주소 하나당 고정
//     4KiB만 매핑한다(대다수 소형 MMIO 레지스터 블록엔 충분, 그
//     이상이 필요한 장치가 실제로 나오면 그때 확장).
struct RequestIoPermissionArgs {
    uint32_t bus = 0, device = 0, function = 0;  // 이 PCI 장치를 특정
    // 요청하는 BAR - EnumerateDevices가 돌려준 그 장치의
    // DeviceDescriptor::mmioBases[] 값 중 정확히 하나와 일치해야
    // 한다(그 외 값은 임의 물리주소 접근 시도로 간주해 거부).
    uint64_t mmioBase = 0;
    // out
    // NotFound(장치가 없거나 mmioBase가 그 장치의 실제 BAR가 아님) /
    // InvalidHandle(제출자를 못 찾음 또는 이미 다른 프로세스가 이
    // BAR를 점유) / ResourceExhausted(주소공간 매핑 실패 또는
    // DeviceOwnerTable 포화) - 전부 기존 ChannelError 재사용(SP
    // 원문이 RequestIoPermissionArgs에 이미 ChannelError를 쓰기로
    // 확정해 둔 선례 그대로).
    ChannelError error = ChannelError::None;
    uint64_t mappedVirtualAddr = 0;  // 성공 시 호출자 프로세스 주소공간의 가상주소
    uint32_t assignedIrqVector = 0;  // v1: 항상 0(위 "v1 축소 범위" 참고)
};

constexpr SyscallEndpointId kSyscallEndpointRequestIoPermission = kMakeSyscallEndpointId(2, 1);

// [신규, 2026-09-20, SP-43331889 §3] `EnumerateDevicesHandler::onExec()`
// 본문(pnp.cpp) - 유저 포인터 검증(트랩 경계를 넘는 syscall에서만
// 의미 있음)은 그 핸들러가 이미 끝내고 여기로 넘어온다는 전제. devmgr
// 이 커널 모드로 흡수된 뒤(§7) 직접 호출하는 진입점이자, 기존 syscall
// 트랩 어댑터도 이 함수 하나로 통일해 쓴다.
void kEnumerateDevicesSync(uint32_t startIndex, uint32_t* capacity, DeviceDescriptor* outDevices,
                           uint32_t* outTotalCount);

// [신규, 2026-09-20, SP-43331889 §7(fs 전환)] `RequestIoPermissionHandler::
// onExec()` 본문(pnp.cpp) - `kEnumerateDevicesSync`와 같은 이유로
// 익명 네임스페이스 밖으로 뺐다. `caller`는 트랩 경로에선
// `task->submitterTask.lock()`, 커널 모드 직접 호출(fs 등)에서는
// 호출자 자신의 `weakAsTask().lock()`을 그대로 넘긴다 - `kClaimBar()`가
// `const SharedPtr<Task>&`를 요구해 `Task*`가 아니라 `SharedPtr<Task>`를
// 받는다(`kMapMmioForCaller()`/`kUnmapMmioForCaller()`엔 `caller.get()`로
// 넘김). `kMapMmioForCaller()`가 이미 `caller->isUserLevel`로
// 분기하므로 이 함수는 그 분기를 그대로 물려받는다(KernelThread
// 분기는 아직 실제 호출자가 없어 미검증 - pnp.cpp의 `kMapMmioForCaller`
// 문서 주석 참고, fs 전환이 첫 실제 호출자가 된다).
void kRequestIoPermissionSync(const SharedPtr<Task>& caller, uint32_t bus, uint32_t device, uint32_t function,
                               uint64_t mmioBase, uint64_t* outMappedVirtualAddr, uint32_t* outAssignedIrqVector,
                               ChannelError* outError);

class PnpService {
public:
    // 부팅 시 한 번 호출 - EnumerateDevices/RequestIoPermission
    // endpoint를 등록한다.
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PNP_H
