#ifndef MINICORE_KERNEL_ADDRESS_SPACE_H
#define MINICORE_KERNEL_ADDRESS_SPACE_H

#include "libkenv/maple_tree.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"

// SP-2AAD7C8D §2/§4 - mmap 서브시스템의 두 관리자(프로세스별/커널
// 전용)와 그 값 타입(Vma). §1이 설명하는 대로 두 관리자는 자료구조는
// 같아도(Maple Tree, libkenv/maple_tree.h) 인스턴스 개수와 동시성
// 보호 범위가 근본적으로 다르다 - ProcessAddressSpaceManager는
// 프로세스마다 하나(그 프로세스만의 Spinlock), KernelAddressSpaceManager
// 는 시스템 전체 싱글턴(모든 CPU가 공유하는 전역 Spinlock + 매핑
// 변경 시 TlbShootdown::broadcast).
//
// PN-012E8C1A(이 문서의 구현 계획) 1차 증분 - Vma/두 관리자 자체와
// mapRegion/unmapRegion/unmapAll만 갖춘다. **아직 없는 것**: Mmap/
// Munmap/Brk syscall 핸들러(§5, RM-48E1E610 17-19번 - "번호만 예약"
// 그대로 남아 있음)와 Process::execImage()/self-terminate 경로와의
// 실제 연동(PN-71C3D483 항목 3) - 둘 다 이 관리자가 먼저 존재해야
// 착수할 수 있어 후속 증분으로 분리했다(SP-DE19BB1C의 TlbShootdown이
// "소비자가 생길 때까지 독립 완결 인프라로 먼저 갖춰 둔다"고 했던 것과
// 같은 패턴 - 이제 이 관리자가 그 소비자가 됐다).

namespace kernel {

enum class VmaBacking : uint32_t {
    Anonymous,      // v1 유일하게 실제로 동작 - mapRegion()이 그 자리에서 PageFrameAllocator로 즉시 채운다
    FixedPhysical,  // 물리주소가 이미 정해짐(MMIO/DMA 버퍼) - 프레임 소유권은 호출부, 이 관리자는 반납 안 함
    FileBacked,     // §9.5 - fs 서비스 등장 이후. v1은 store만 가능(폴트 연동 없음, 사실상 미사용)
};

// SP-2AAD7C8D §4 - Maple Tree의 값(void*)으로 저장되는 VMA 서술자.
// 문서 원안의 `used` 필드(ChunkedList류의 슬롯 재사용 관례)는 두지
// 않았다 - 이 관리자는 Vma를 슬롯 재사용이 아니라 GenericSlabAllocator
// 로 그때그때 개별 확보/반납하므로 그 플래그가 의미가 없다(구현 세부,
// RM-23F4B687 §4 - 공개 계약을 바꾸지 않는 수준).
struct Vma {
    uint64_t start = 0;
    uint64_t end = 0;  // inclusive(MapleTree::store와 동일한 관례)
    uint64_t prot = 0;  // v1은 Paging::PAGE_* 플래그 조합을 그대로 받는다(실제 Mmap syscall ABI 확정 시 재검토)
    VmaBacking backing = VmaBacking::Anonymous;
    uint64_t fixedPhysAddr = 0;  // backing==FixedPhysical일 때만 사용
    int32_t backingFd = -1;      // backing==FileBacked일 때만 사용(§9.2의 fd)
    uint64_t fileOffset = 0;     // backing==FileBacked일 때만 사용
};

// SP-2AAD7C8D §2 - Process마다 하나(인스턴스 N개). v1은 프로세스당
// 스레드 하나뿐이라 실경합이 거의 없지만, 문서가 명시한 대로 락은
// 갖춘다(미래 대비 최소 방어).
//
// **실측 발견(v1 MapleTree 용량 제약, PN-012E8C1A 검증 중, 2026-09-15)**:
// MapleTree는 항상 [0, UINT64_MAX] 전체를 추적한다(libkenv/maple_tree.h
// 문서 그대로) - regionFloor/regionCeil이 그 전체의 진부분집합이면
// (실제로는 거의 항상 그렇다 - 유저 프로세스든 커널 동적 영역이든
// 0이나 UINT64_MAX에 딱 맞닿는 경우는 없다), 그 경계 밖 두 구간
// ([0,regionFloor-1], [regionCeil+1,UINT64_MAX])이 findGap/store가
// 만드는 gap으로서 항상 슬롯 2개를 영구히 점유한다 - 그래서
// kMapleArangeSlotCount(10)개 슬롯 중 실제 VMA 값에 쓸 수 있는 건
// **8개뿐**이다(regionFloor==0 && regionCeil==UINT64_MAX일 때만
// 정확히 10개 전부). 멀티레벨 분할(PN-38D17292)이 끝나기 전까지는
// 프로세스당 최대 8개의 서로 떨어진 VMA만 가질 수 있다는 뜻 - 이
// 한계를 넘는 실제 워크로드가 나타나면 PN-38D17292 우선순위를
// 재검토해야 한다.
class ProcessAddressSpaceManager {
public:
    // pml4Phys: Paging::mapPage/unmapPage/translatePage에 넘길 이
    // 프로세스의 주소공간. regionFloor/regionCeil: mmap 가능 영역
    // (§6-3 "코드 공간 위쪽 ~ 스택 하단 사이 전부" - 정확한 경계는
    // 호출부가 정한다, 아직 execImage()와 연동되지 않아 지금은 호출부
    // 자유. 위 클래스 문서의 8-슬롯 제약을 고려해 정한다).
    void init(uint64_t pml4Phys, uint64_t regionFloor, uint64_t regionCeil);

    // length를 4KiB로 올림해 findGap으로 빈 자리를 찾고 등록한다.
    // Anonymous면 그 자리에서 물리 페이지를 확보해 즉시 매핑까지
    // 마친다(§2-B의 지연 매핑/요구 페이징 대신 v1은 즉시 매핑 - 페이지
    // 폴트 핸들러(Paging::handlePageFault)가 아직 이 관리자를 몰라서
    // 지금은 이게 유일하게 실제로 동작하는 선택지다, 후속 과제).
    // FixedPhysical이면 fixedPhysAddr부터 연속한 이미 있는 물리
    // 프레임을 그대로 매핑한다(프레임 소유권은 호출부, 이 함수는 절대
    // 반납하지 않는다). 실패(gap 없음/트리 포화/페이지 고갈) 시 false -
    // 이미 매핑한 몫이 있으면 이 호출 안에서 롤백한다.
    bool mapRegion(uint64_t length, uint64_t prot, VmaBacking backing, uint64_t fixedPhysAddr,
                   uint64_t* outAddr);

    // mapRegion()이 반환한 [addr, addr+length) 그대로를 넘겨야 한다 -
    // v1은 정확히 일치하는 범위만 지원(MapleTree::erase 자체는 부분
    // 겹침도 지원하지만, 그 조각을 새 Vma 두 개로 나누는 책임은 이
    // 관리자가 아직 구현하지 않았다, §3.3의 erase() 문서 주석 참고).
    // 실제 페이지도 Paging::unmapPage로 해제한다(Anonymous면
    // PageFrameAllocator에도 반납, FixedPhysical은 프레임 자체는 그대로
    // 둠). 등록된 적 없거나 범위가 안 맞으면 false.
    bool unmapRegion(uint64_t addr, uint64_t length);

    // 이 프로세스가 소유한 모든 VMA의 실제 페이지를 전부 해제한다
    // (PN-71C3D483 항목 3용) - **Process::destroy() 호출 전에 반드시
    // 먼저 불러야 한다**(Paging::destroyAddressSpace는 PML4 프레임
    // 자체만 반납하고 그 하위 절반이 가리키는 페이지는 안 건드림,
    // process.h의 Process::destroy() 문서 주석과 동일한 전제). v1
    // MapleTree가 최대 kMapleArangeSlotCount(10)개 엔트리로 제한되므로
    // (§6-5, 위 클래스 문서의 8-슬롯 실측 제약 참고) 이 순회도 최대
    // 10회(실제로는 최대 8개 VMA).
    void unmapAll();

private:
    uint64_t _pml4Phys = 0;
    uint64_t _regionFloor = 0;
    uint64_t _regionCeil = 0;
    Spinlock _lock;
    MapleTree _tree;
};

// SP-2AAD7C8D §2 - 시스템 전체 싱글턴. paging.h의 kLazyZoneBase/
// kLazyZoneSize("나중에 진짜 힙/프로세스 주소공간이 생기면 이 구역이
// 그 정책의 첫 사용처가 될 수 있다"던 자리표시자)를 이 관리자가
// 실제로 관리하는 커널 동적 할당 범위로 쓴다 - direct map/커널 이미지/
// 스택 풀 등 부팅 시 고정 배치된 영역은 건드리지 않는다. v1은
// Anonymous만 지원(커널 영역엔 FixedPhysical/FileBacked 개념이 아직
// 없음 - MMIO는 이 관리자를 거치지 않는 별도 정적 매핑 경로).
// **ProcessAddressSpaceManager와 같은 8-슬롯 실측 제약이 그대로 적용된다**
// (kLazyZoneBase/kLazyZoneSize도 [0,UINT64_MAX]의 진부분집합이므로 -
// 위 ProcessAddressSpaceManager 클래스 문서 참고) - 동시에 관리 가능한
// 커널 동적 영역은 최대 8개.
class KernelAddressSpaceManager {
public:
    // TlbShootdown::init() 이후 아무 때나 호출 가능(kmain.cpp 부팅
    // 순서 참고) - unmapRegion()이 실제로 안전하려면 TlbShootdown::init()
    // 이 먼저 끝나 있어야 한다(호출부 책임).
    static void init();

    // length를 4KiB로 올림해 findGap으로 빈 자리를 찾고, 그 자리에
    // 물리 페이지를 확보해 매핑한다. flags는 Paging::mapPage에 그대로
    // 넘기는 추가 플래그(PAGE_PRESENT/PAGE_USER 없이 커널 전용 - 유저
    // 접근 가능한 커널 매핑은 이 관리자의 대상이 아니다).
    static bool mapRegion(uint64_t length, uint64_t flags, uint64_t* outAddr);

    // mapRegion()이 반환한 [addr, addr+length) 그대로를 넘겨야 한다
    // (ProcessAddressSpaceManager::unmapRegion과 동일한 v1 제약). 실제
    // 페이지를 해제하고 PageFrameAllocator에 반납한 뒤, **이 락을 쥔
    // 채로** TlbShootdown::broadcast()를 불러 모든 온라인 코어의 TLB를
    // 갱신한다(§1 - 커널 영역은 전 코어 공유, tlb_shootdown.h의 호출
    // 요구사항 그대로).
    static bool unmapRegion(uint64_t addr, uint64_t length);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_ADDRESS_SPACE_H
