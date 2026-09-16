#ifndef MINICORE_KERNEL_TLB_SHOOTDOWN_H
#define MINICORE_KERNEL_TLB_SHOOTDOWN_H

#include "libkenv/types.h"

namespace kernel {

// SP-DE19BB1C - 다른 코어에 남아있는 스테일 TLB 엔트리를 IPI로 강제
// 무효화시킨다. `Paging::unmapPage`는 지금 "이 코어가 보고 있는
// 주소공간" 기준으로만 invlpg하므로(paging.cpp 참고), 매핑을 바꾸는
// 코드는 이것과 별도로 `broadcast()`를 불러야 한다. 실제 호출부는
// `KernelAddressSpaceManager::unmapRegion()`(커널 영역, targetPml4Phys
// 생략 - 항상 전체 온라인 코어 브로드캐스트)와
// `ProcessAddressSpaceManager::unmapRegion()`/`resizeAnonymousRegion()`
// (유저 영역, targetPml4Phys 지정 - PN-D132A1E9) 둘 다.
class TlbShootdown {
public:
    // kTlbShootdownVector에 ISR을 등록한다 - `Idt::init()` 이후 아무
    // 때나 호출 가능(HPET/PIT 핸들러와 같은 관례, `kmain.cpp`가 부팅
    // 중 BSP에서 한 번만 호출 - IDT 내용은 전역 하나뿐이라 AP는 다시
    // 부를 필요 없음). 실제 `broadcast()`가 의미 있으려면
    // `Acpi::init()`/`Lapic::init()`까지 끝나 있어야 한다(온라인 코어
    // 목록 조회/IPI 전송).
    static void init();

    // [virtStart, virtEnd)의 매핑 변경을 다른 코어들에 전파한다 -
    // 호출한 이 코어는(대상에 포함되면) IPI 없이 즉시 invalidate하고,
    // 나머지 대상 코어에는 `Lapic::sendFixedIpi`로 통지한 뒤 전부
    // ACK할 때까지 busy-wait한다 - 반환 시점엔 대상 코어 전부의 TLB가
    // 이미 갱신돼 있음이 보장된다.
    //
    // **targetPml4Phys**(기본값 0 = 커널 영역):
    // - **0(커널 영역)**: 모든 온라인 코어가 대상이다 - 상위 절반
    //   (higher-half)은 모든 프로세스의 PML4가 공유하는 테이블이라
    //   어느 코어가 지금 무엇을 실행 중이든 항상 영향받는다.
    // - **0이 아님(유저 영역, PN-D132A1E9/QU-DE2828A1)**: 시스템 전체
    //   브로드캐스트가 아니라, 이 PML4를 지금 실제로 실행 중인
    //   코어들만(Active CPU Mask, `Scheduler::taskOnCore()`로 스캔)
    //   대상으로 한다 - 호출한 코어 자신도 그 조건을 만족할 때만
    //   직접 invalidate한다(커널 영역과 달리 무조건이 아님).
    //
    // 요청은 **호출한 코어 자신의 전용 슬롯**(코어 인덱스로 나뉜
    // 배열, 락 불필요 - 한 코어는 동기적으로 호출하면 그 스레드
    // 자신이 블로킹되므로 한 번에 하나의 요청만 낼 수 있다)에 쓰고,
    // 대상 코어들의 "수신자별 Target Pending Mask"에 자기 코어
    // 인덱스 비트를 세워 알린다 - 그래서 서로 다른 코어가 동시에
    // (서로 다른 프로세스에 대해) `broadcast()`를 불러도 요청이
    // 뒤섞이지 않는다(§5-1, SP-DE19BB1C).
    //
    // **호출부가 그 매핑 변경 자체를 보호하는 락을 쥔 채로 불러야
    // 한다**(자기 자신의 요청 슬롯은 스스로만 쓰므로 다른 요청과는
    // 안전하지만, 같은 주소공간에 대한 매핑 변경 자체는 여전히
    // 호출부의 락이 직렬화해야 한다).
    static void broadcast(uint64_t virtStart, uint64_t virtEnd, uint64_t targetPml4Phys = 0);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TLB_SHOOTDOWN_H
