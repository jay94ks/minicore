#ifndef MINICORE_KERNEL_PROCESS_H
#define MINICORE_KERNEL_PROCESS_H

#include "address_space.h"
#include "libkenv/types.h"

namespace elf {
class Image;  // 전방 선언(minicore/libs/libelf/elf.h) - Process::execImage 시그니처용
}

namespace kernel {

class UserThread;

// 유저 프로세스의 커널 쪽 표현(SP-8B6B8D25 §2-B/§5, 유저랜드 준비
// 마일스톤 - 계획 PN-16CA347D) - 이름 자체는 제안일 뿐 확정이 아니다
// (UserThread/Task와 동일 각주, SP-04EE2A18). "커널 Task와 (유저)
// 쓰레드는 다른 개념"(설계자 지시, 2026-09-14)이라는 구분을 그대로
// 이어받아, Process는 **주소공간(전용 PML4)의 소유자**이고
// `UserThread`(Task 상속)는 그 안에서 실제로 스케줄링되는 실행
// 흐름이다 - 지금은 프로세스당 스레드 하나만 지원(v1, 멀티스레드
// 프로세스는 후속 과제).
//
// **범위 안내**: 이 구조체는 "주소공간 소유 + 유저 모드 폴트 정보
// 보관" 부분만 다룬다(§2-B가 명시적으로 요구하는 부분, 지금 바로
// 구현/검증 가능) - ELF 로더/프로세스 생성-exec/ring3 진입은 유저
// 주소공간 레이아웃(128TiB, 코드 베이스 0x400000, 스택은 끝점부터,
// 코드/스택 양쪽 가드 페이지 - SP-8B6B8D25 §5/§5-A, QU-72108298/
// QU-7043EA6D 답변으로 확정 완료) 자체는 준비됐지만, **QU-FF3F0CAA
// ("첫 프로세스"의 정체/유저랜드 빌드 체계) 답변이 아직 없어** 실제로
// 실측 검증할 방법이 정해지기 전까지는 별도로 이어간다.
class Process {
public:
    // 유저 모드 페이지 폴트 정보(§2-B "그 폴트 정보는 그 프로세스
    // 자신의 자료구조(PCB)에 매달아 둔다") - 폴트가 나면 커널을 멈추지
    // 않고 이 프로세스만 멈춘 뒤 여기 기록해 둔다.
    //
    // **깨우기 정책 확정(QU-0C4D25EF 답변, 2026-09-14)**: 이 폴트
    // 정보를 읽어 "프로세스를 죽일지, swapfs에서 읽어와 페이지를
    // 갈아 끼울지"를 판단하는 비동기 작업(AsyncTaskHandler)이 나중에
    // 처리한다 - `Task::state`는 새 상태 없이 **기존 `Blocked`를 그냥
    // 재사용**한다(그 처리가 끝나기 전까지 이 프로세스가 실행되지만
    // 않으면 되고, "왜 Blocked인지"는 이 `pending` 플래그 자체가
    // 이미 말해 준다). 그 비동기 작업/swapfs 자체는 아직 이
    // 프로젝트에 없다 - 그 인프라가 생길 때 실제로 연결한다.
    struct FaultInfo {
        bool pending = false;
        uint64_t faultAddr = 0;    // CR2
        uint64_t errorCode = 0;    // 하드웨어가 스택에 남긴 에러 코드
        uint64_t rip = 0;          // 폴트를 일으킨 명령어
    };

    uint64_t pml4Phys = 0;        // 이 프로세스 전용 주소공간의 PML4 물리 프레임(Paging::createAddressSpace)
    UserThread* mainThread = nullptr;  // v1: 프로세스당 스레드 하나(위 클래스 주석 참고)
    FaultInfo lastFault;

    // 이 프로세스가 소유한 VMA(코드/데이터/스택 - execImage()가 채움)의
    // 장부(PN-71C3D483 항목 3, SP-2AAD7C8D §2/§4) - destroy()가 이걸로
    // 실제 페이지를 찾아 반납한다. init()이 pml4Phys 확보 직후 초기화.
    ProcessAddressSpaceManager addressSpace;

    // pml4Phys를 새로 확보하고 커널 상위 절반(higher-half)을 공유하는
    // 상태로 초기화한다(Paging::createAddressSpace 참고 - 하위 절반은
    // 전부 비어 있는 채로 시작, ELF 로더가 채울 자리). 실패(Slab/페이지
    // 고갈) 시 false - 블로킹하지 않는다.
    bool init();

    // pml4Phys를 반납한다 - **이 함수 자신이 먼저 `addressSpace.
    // unmapAll()`로 이 프로세스가 실제로 매핑한 하위 절반의 모든
    // VMA(코드/데이터/스택)를 Paging::unmapPage로 해제한 뒤에 PML4
    // 프레임을 반납한다**(PN-71C3D483 항목 3 완료 - 예전엔 이 순서를
    // 호출부가 직접 챙겨야 했으나, VMA 추적 자료구조가 생겨 이제
    // 이 함수 하나로 완결된다). **알려진 한계**: `Paging::
    // destroyAddressSpace`는 PML4 프레임 자체만 반납하고, 그 하위에
    // 매달린 PDPT/PD/PT 중간 테이블 프레임은 건드리지 않는다(leaf
    // 데이터 페이지만 이 함수가 회수 - 중간 테이블 프레임 회수는 이
    // 항목의 스코프 밖, 별도로 추적한다).
    void destroy();

    // ELF 이미지를 이 프로세스 주소공간에 로드하고, thread(호출부가
    // 마련해 둔, 아직 init() 전인 UserThread)를 그 진입점으로 ring3
    // 진입하도록 준비시킨다(PN-16CA347D 6번/PN-124C105B) - 성공하면
    // thread 자신을 반환한다(호출부가 Scheduler::enqueue해야 실제로
    // 실행 시작), 실패(ELF 로드/유저 스택 확보 실패) 시 nullptr.
    //
    // **v1 한계**:
    // - 유저 스택은 고정 크기(64KiB)/고정 주소, 가드 페이지 없음
    //   (커널 스택 가드 페이지와 같은 패턴으로 후속 추가 예정, 새 DC
    //   불필요 수준).
    // - syscall MSR 경로(STAR/LSTAR/SFMASK)가 아직 없어 유저 코드는
    //   반드시 `int 0x80`으로만 트랩해야 한다(PN-124C105B 남은 항목).
    // - 프로세스당 스레드 하나 전제(위 클래스 주석과 동일) - thread는
    //   호출부가 소유(동적 할당/해제는 이번 범위 밖, PN-40E976F2).
    UserThread* execImage(const elf::Image& image, UserThread* thread);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PROCESS_H
