#ifndef MINICORE_KERNEL_RESOURCE_GROUP_H
#define MINICORE_KERNEL_RESOURCE_GROUP_H

#include "libkenv/chunked_list.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "named_object.h"
#include "task.h"

namespace kernel {

class Process;

// [SP-245D130B] Linux `cgroup`에 대응하는 이 프로젝트의 이름
// (`ProcessRole`/`TaskClass`와 같은 PascalCase 관례). 이번 증분은
// §1(트리)/§2(Process 연결)/§4(freeze)만 다룬다 - §3(CPU 쿼터
// 스로틀)/§5(계정)/§6(메모리)/§7(I/O)는 아래 필드만 자리를 잡아 두고
// 실제 로직은 후속 계획으로 분리했다(PN-4190BBD3 체크리스트 참고,
// RM-23F4B687 §4 - 한 번에 전부 만들지 않는다).

// [자리만 확보, 후속] SP-245D130B §3 - CPU 쿼터 스로틀. 아직
// Scheduler::onTick()에 연결되지 않았다(계정 카운터 갱신 로직 자체가
// 스케줄러 핫패스 변경이라 별도로 신중히 착수 - PN-4190BBD3 참고).
struct ResourceGroupCpuControl {
    uint32_t periodTicks = 0;  // 0 = 무제한(기본값)
    uint32_t quotaTicks = 0;
    uint32_t usedTicksInPeriod = 0;
    uint64_t periodStartTick = 0;
};

// [자리만 확보, 후속] SP-245D130B §5 - CPU 시간 계정. §3과 마찬가지로
// 아직 실제로 증가시키는 코드가 없다.
struct ResourceGroupAccounting {
    uint64_t totalCpuTicks = 0;
};

// [신규, 2026-09-17, SP-245D130B] 자원 그룹 - 프로세스 트리
// (Process::parent/children)와 완전히 별개의 축이다. 이번 증분은
// **`gRootResourceGroup` 하나만** 실존한다(동적 그룹 생성
// `ResourceGroupCreate`는 아직 없음 - §1의 `parent`/`children` 필드는
// 그 후속을 위해 자리만 잡아 둔 것으로, 지금은 항상 비어 있다).
class ResourceGroup {
public:
    Spinlock lock;
    char name[kMaxNamedObjectNameLength] = {};
    uint64_t nameLength = 0;

    // [후속 전용, 지금은 항상 비어 있음] 그룹 트리 - §1 참고.
    ResourceGroup* parent = nullptr;
    static constexpr uint32_t kMaxChildrenChunkCapacity = 8;
    ChunkedList<ResourceGroup*, kMaxChildrenChunkCapacity> children;

    // 이 그룹에 속한 프로세스들 - `WeakPtr<Process>`(관찰만, 그룹이
    // 프로세스를 소유하지 않는다 - SP-245D130B §1). `Process`는
    // `memset(0)`으로 할당되므로(process.cpp `Process::allocate()`)
    // `WeakPtr`을 안전하게 담을 수 있다(`Channel::ownerProcess`가
    // 겪은 placement-new 문제와 달리 - PN-18FDBFF3/SP-CA3C3E57 §6.1
    // 참고, Channel은 memset을 안 거쳐 이 방식을 못 썼다).
    static constexpr uint32_t kMaxMemberChunkCapacity = 8;
    ChunkedList<WeakPtr<Process>, kMaxMemberChunkCapacity> memberProcesses;

    ResourceGroupCpuControl cpu;
    ResourceGroupAccounting accounting;

    // [SP-245D130B §4] `TaskState`에 새 상태를 추가하지 않고 기존
    // `Blocked`를 재사용한다(SP-9A6D579F §3.5와 동일한 원칙). 실제
    // 적용은 `Scheduler::onTick()`의 재스케줄 시점(`kCheckAndMarkFrozen`,
    // 아래)에서 일어난다 - 이미 Ready 큐에 들어가 있던(아직 한 번도
    // 안 뽑힌) Task까지 즉시 멈추지는 못한다는 한계가 있다(큐 임의
    // 제거 API가 없음 - 다음에 그 Task가 실제로 실행돼 이 코드를
    // 다시 거칠 때 비로소 멈춘다, 정직하게 문서화).
    bool frozen = false;

    // `proc`는 호출자(`Process::joinResourceGroup`, 유일한 호출부)가
    // 이미 `weakFromThis()`로 만들어 둔 값을 그대로 넘긴다 -
    // `weakFromThis()`가 protected라 `ResourceGroup` 쪽에서 직접 만들
    // 수 없기 때문(RM-32D06563의 self-ref류 관례와 같은 이유).
    void addMember(const WeakPtr<Process>& proc);
    void removeMember(Process* proc);

    // 그룹 소속 전체 프로세스의 mainThread를 순회하며 Ready/Running인
    // 것만 Blocked로 전환 대상 표시(§4의 한계 참고). 이미 다른 이유로
    // Blocked인 Task(디버그 정지 등)는 건드리지 않는다.
    void freeze();
    // freeze()가 실제로 멈춘(Process::frozenByGroup) 것만 다시 깨운다.
    void thaw();
};

// 명시적으로 다른 그룹에 가입하지 않은 모든 프로세스가 속하는 루트
// 그룹 - 정적 전역 객체(진짜 C++ 생성자를 거친다, Channel/Process류의
// 슬랩+init() 관례가 아니다) - 동적 그룹이 아직 없어 SharedPtr/
// EnableSharedFromThis가 필요 없다(있어도 self-ref 패턴을 또 새로
// 만들어야 해서 오히려 과설계, RM-23F4B687 §4).
extern ResourceGroup gRootResourceGroup;

// 부팅 시 한 번 호출 - gRootResourceGroup 이름만 채운다.
void kResourceGroupInit();

// [SP-245D130B §4] `Scheduler::onTick()`의 재스케줄 결정 지점에서
// 호출한다 - `task`가 유저 프로세스에 속하고 그 프로세스의 그룹이
// frozen이면 `Process::frozenByGroup`를 세우고 true를 반환한다(호출부는
// 이 경우 `enqueue()` 대신 `task->state = TaskState::Blocked`로
// 전환). 커널 전용 Task(`isUserLevel == false`)나 그룹이 없는(아직
// `SpawnProcess`를 안 거친) 경우는 항상 false.
bool kCheckAndMarkFrozen(Task* task);

}  // namespace kernel

#endif  // MINICORE_KERNEL_RESOURCE_GROUP_H
