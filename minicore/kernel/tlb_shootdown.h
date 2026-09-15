#ifndef MINICORE_KERNEL_TLB_SHOOTDOWN_H
#define MINICORE_KERNEL_TLB_SHOOTDOWN_H

#include "libkenv/types.h"

namespace kernel {

// SP-DE19BB1C - 커널 영역(higher-half, 모든 PML4가 공유하는 PDPT/PD/
// PT) 매핑을 바꿀 때, 다른 코어에 남아있는 스테일 TLB 엔트리를 IPI로
// 강제 무효화시킨다. `Paging::unmapPage`는 지금 "이 코어가 보고 있는
// 주소공간" 기준으로만 invlpg하므로(paging.cpp 참고), 커널 영역
// 변경은 이것과 별도로 `broadcast()`를 불러야 한다 - **아직 이
// 프로젝트엔 그 실제 호출부(SP-2AAD7C8D의 `KernelAddressSpaceManager`)
// 자체가 없다**(mmap 서브시스템 미착수) - 이 클래스는 그 소비자가
// 생길 때까지 독립적으로 완결된 인프라로 먼저 갖춰 둔다.
class TlbShootdown {
public:
    // kTlbShootdownVector에 ISR을 등록한다 - `Idt::init()` 이후 아무
    // 때나 호출 가능(HPET/PIT 핸들러와 같은 관례, `kmain.cpp`가 부팅
    // 중 BSP에서 한 번만 호출 - IDT 내용은 전역 하나뿐이라 AP는 다시
    // 부를 필요 없음). 실제 `broadcast()`가 의미 있으려면
    // `Acpi::init()`/`Lapic::init()`까지 끝나 있어야 한다(온라인 코어
    // 목록 조회/IPI 전송).
    static void init();

    // [virtStart, virtEnd)의 커널 영역 매핑 변경을 모든 온라인 코어에
    // 전파한다 - 호출한 이 코어는 IPI 없이 즉시 invalidate하고, 나머지
    // 온라인 코어에는 `Lapic::sendFixedIpi`로 통지한 뒤 전부 ACK할
    // 때까지(pendingAckCount==0) busy-wait한다 - 반환 시점엔 모든
    // 온라인 코어의 TLB가 이미 갱신돼 있음이 보장된다.
    //
    // **호출부가 그 매핑 변경 자체를 보호하는 락(향후
    // `KernelAddressSpaceManager`의 전역 Spinlock)을 쥔 채로 불러야
    // 한다** - 이 함수 자신은 요청 슬롯을 하나만 쓰므로, 동시에 두
    // 개의 커널 영역 매핑 변경이 겹치면 요청이 뒤섞인다(§3 참고,
    // SP-DE19BB1C).
    static void broadcast(uint64_t virtStart, uint64_t virtEnd);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_TLB_SHOOTDOWN_H
