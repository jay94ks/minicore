#ifndef MINICORE_KERNEL_TASK_H
#define MINICORE_KERNEL_TASK_H

#include "libkenv/spinlock.h"

namespace kernel {

// 스케줄러의 최소 스케줄링 단위(PL-2D3184BC, 설계자 지시, QU-BA001D73,
// 2026-09-14 - "커널 작업은 태스크(Task)라 명명하고, 이걸 사용한다.
// 일반적인 프로세스는 다수의 쓰레드를 가질 수 있으며 각 쓰레드는
// 어떤 코어에서만/코어에서는 실행(불)가능을 결정할 수 있어야한다").
// 프로세스(주소공간+자원 소유)와 Task(실제 스케줄링/실행 단위)는
// 개념적으로 분리되지만, 유저 프로세스가 아직 없어 이 구조체는
// 지금은 커널 전용 실행 흐름만 표현한다 - 유저랜드가 생기면 CR3/
// 유저 스택 등 필드가 추가될 것이다.

enum class TaskState {
    Ready,
    Running,
    Blocked,
    Zombie,
};

enum class TaskClass {
    Normal,
    RealTime,  // QU-77A52430 - 항상 Normal보다 먼저 스케줄링(구현 예정)
};

// affinityMask 비트 i가 1이면 코어 i에서 실행 가능 - "이 코어에서만"
// (허용 목록)과 "이 코어에서는 불가"(제외 목록) 둘 다 전체 마스크에서
// 시작해 비트를 세우거나/지우는 것으로 자연스럽게 표현된다.
constexpr unsigned int kTaskAffinityAllCores = 0xFFFFFFFFU;
constexpr unsigned long kTaskDefaultKernelStackSize = 8UL * 1024UL;  // 8KiB(QU-BA001D73)

using TaskEntry = void (*)(void* arg);

// context_switch.S의 kContextSwitch/kTaskStartTrampoline이 이 구조체의
// savedRsp 오프셋(항상 첫 필드, 오프셋 0)을 그대로 참조한다 - 필드
// 순서를 바꾸려면 그쪽 어셈블리도 같이 확인해야 한다.
struct Task {
    // 이 Task가 스위칭 아웃될 때의 RSP(커널 스택 안, kContextSwitch가
    // push한 레지스터들의 맨 위를 가리킨다). init() 직후엔 아직 한
    // 번도 실행되지 않은 상태로 kTaskStartTrampoline에 진입하도록
    // 미리 꾸며 둔 스택을 가리킨다.
    unsigned long savedRsp = 0;

    unsigned long kernelStackPhys = 0;  // PageFrameAllocator가 준 물리주소(해제 시 필요)
    unsigned long kernelStackSize = 0;

    TaskState state = TaskState::Ready;
    TaskClass taskClass = TaskClass::Normal;
    unsigned int affinityMask = kTaskAffinityAllCores;

    // 코어별 큐(폴백/lock-free 공용, PL-2D3184BC 4단계)가 쓰는 침습적
    // (intrusive) 다음-포인터 - 이 Task가 큐에 들어있을 때만 유효.
    AtomicPtr<Task> next;

    // 커널 스택을 새로 할당하고, entry(arg)를 처음 실행할 준비가 된
    // 상태로 초기화한다(트램폴린 스택 프레임 구성) - 스케줄러 큐에
    // 넣는 것은 호출부 책임(아직 스케줄러 자체가 없어 별도 API 없음).
    // Paging::init() 이후에만 호출 가능(direct map 필요).
    void init(TaskEntry entry, void* arg, unsigned long stackSize = kTaskDefaultKernelStackSize);
};

// 현재 실행 흐름의 레지스터 상태를 저장하고 newRsp로 전환한다 -
// *oldRspSlot에 전환 전 RSP를 기록한다. 소프트웨어 방식 컨텍스트
// 스위칭(DC-8EA1E7F6/QU-BA001D73 - "하드웨어 TSS 대신 소프트웨어
// 스위칭") - x86_64 System V 콜리세이브 레지스터(rbx/rbp/r12-r15)와
// RFLAGS만 저장/복원한다(caller-saved 레지스터는 C++ 호출 규약상
// 이미 호출부가 필요하면 자기 스택에 저장해 뒀을 것이므로 안 건드림).
extern "C" void kContextSwitch(unsigned long* oldRspSlot, unsigned long newRsp);

}  // namespace kernel

#endif  // MINICORE_KERNEL_TASK_H
