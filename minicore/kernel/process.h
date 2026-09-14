#ifndef MINICORE_KERNEL_PROCESS_H
#define MINICORE_KERNEL_PROCESS_H

#include "libkenv/types.h"

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
// 구현/검증 가능) - ELF 로더/프로세스 생성-exec/ring3 진입은 주소
// 공간 레이아웃(SP-8B6B8D25 §5의 0~256TiB 수치와 4레벨 페이징의
// 실제 상한 128TiB 사이의 불일치 등, QU-* 질의로 확인 중)이 확정된
// 뒤 별도로 이어간다.
class Process {
public:
    // 유저 모드 페이지 폴트 정보(§2-B "그 폴트 정보는 그 프로세스
    // 자신의 자료구조(PCB)에 매달아 둔다") - 폴트가 나면 커널을 멈추지
    // 않고 이 프로세스만 멈춘 뒤 여기 기록해 둔다. 이 정보를 보고
    // "진짜 잘못된 접근인지, SWAP된 페이지라 다시 읽어야 하는지"를
    // 판단하는 실제 정책(누가/언제 다시 깨우는지 포함)은 아직 미정 -
    // 필드만 먼저 마련해 둔다.
    struct FaultInfo {
        bool pending = false;
        uint64_t faultAddr = 0;    // CR2
        uint64_t errorCode = 0;    // 하드웨어가 스택에 남긴 에러 코드
        uint64_t rip = 0;          // 폴트를 일으킨 명령어
    };

    uint64_t pml4Phys = 0;        // 이 프로세스 전용 주소공간의 PML4 물리 프레임(Paging::createAddressSpace)
    UserThread* mainThread = nullptr;  // v1: 프로세스당 스레드 하나(위 클래스 주석 참고)
    FaultInfo lastFault;

    // pml4Phys를 새로 확보하고 커널 상위 절반(higher-half)을 공유하는
    // 상태로 초기화한다(Paging::createAddressSpace 참고 - 하위 절반은
    // 전부 비어 있는 채로 시작, ELF 로더가 채울 자리). 실패(Slab/페이지
    // 고갈) 시 false - 블로킹하지 않는다.
    bool init();

    // pml4Phys를 반납한다 - **호출 전에 이 프로세스가 실제로 매핑한
    // 하위 절반의 모든 페이지를 먼저 Paging::unmapPage로 해제해 둬야
    // 한다**(이 함수 자신은 PML4 프레임 자체만 반납, 그 안의 하위
    // 절반 엔트리가 가리키는 하위 테이블/데이터 페이지는 건드리지
    // 않는다 - 호출부 책임). 프로세스 종료 시퀀스 자체는 아직 이
    // 프로젝트에 없다(PN-40E976F2 참고 - onCancel 호출 경로와 같은
    // 선행 조건 대기 상태).
    void destroy();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PROCESS_H
