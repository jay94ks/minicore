#ifndef MINICORE_KERNEL_RESOURCE_GROUP_H
#define MINICORE_KERNEL_RESOURCE_GROUP_H

#include "channel.h"
#include "libkenv/chunked_list.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "mount_table.h"
#include "named_object.h"
#include "syscall.h"
#include "task.h"

namespace kernel {

class Process;

// [SP-245D130B] Linux `cgroup`에 대응하는 이 프로젝트의 이름
// (`ProcessRole`/`TaskClass`와 같은 PascalCase 관례). §1(트리)/
// §2(Process 연결)/§4(freeze)는 먼저 구현됐고(PN-4190BBD3), §3(CPU
// 쿼터 스로틀)/§5(VFS 계정 노출)/§8(syscall 6종)은 `SP-6A563A8F`
// (approved)가 구체화해 이 증분이 실제로 구현한다.
constexpr SyscallEndpointId kSyscallEndpointResourceGroupJoin = kMakeSyscallEndpointId(9, 0);
constexpr SyscallEndpointId kSyscallEndpointResourceGroupCreate = kMakeSyscallEndpointId(9, 1);
constexpr SyscallEndpointId kSyscallEndpointResourceGroupDestroy = kMakeSyscallEndpointId(9, 2);
constexpr SyscallEndpointId kSyscallEndpointResourceGroupSetCpuQuota = kMakeSyscallEndpointId(9, 3);
constexpr SyscallEndpointId kSyscallEndpointResourceGroupFreeze = kMakeSyscallEndpointId(9, 4);
constexpr SyscallEndpointId kSyscallEndpointResourceGroupThaw = kMakeSyscallEndpointId(9, 5);

constexpr uint32_t kMaxResourceGroupNameLength = kMaxNamedObjectNameLength;  // 64, named_object.h와 동일 상한

// [SP-245D130B §8] 대상 그룹을 이름으로 지정 - 이름은 트리 전체에서
// 전역 유일(named_object.h의 NamedObjectTable과 동일한 평평한 이름
// 공간 관례를 그대로 재사용 - 새 레지스트리를 또 만들지 않고
// `kFindResourceGroupByName()`이 트리를 직접 DFS로 순회한다, 그룹
// 수가 실사용에서 매우 적을 것으로 예상돼 실측 전 별도 색인은
// 과설계로 판단, RM-23F4B687 §4).
// [정정, 2026-09-19] SP-245D130B §8이 제안한 "호출자 또는 지정
// 프로세스" 중 v1은 호출자 자신만 지원한다 - 임의의 다른 프로세스를
// 대신 가입시키려면 "그 대상 프로세스에 대한 권한"이라는 별도 축이
// 추가로 필요한데, §9-2가 확정한 모델("호출자가 그 그룹의 조상 체인
// 안에 있을 때만")은 그룹에 대한 권한만 다룰 뿐 대상 프로세스에 대한
// 권한은 다루지 않는다 - 이 갭은 CLAUDE.md 규칙 4에 따라 임의로
// 메우지 않고 v1 스코프를 좁혀(호출자 자신만) 회피한다. 필요해지면
// 별도 계획으로 확장.
struct ResourceGroupJoinArgs {
    char name[kMaxResourceGroupNameLength] = {};
    uint32_t nameLength = 0;
    ChannelError error = ChannelError::None;
};

struct ResourceGroupCreateArgs {
    char parentName[kMaxResourceGroupNameLength] = {};
    uint32_t parentNameLength = 0;  // 0이면 루트 아래 생성(SP-245D130B §8 "생략 시 루트 자식")
    char name[kMaxResourceGroupNameLength] = {};
    uint32_t nameLength = 0;
    ChannelError error = ChannelError::None;
};

struct ResourceGroupDestroyArgs {
    char name[kMaxResourceGroupNameLength] = {};
    uint32_t nameLength = 0;
    ChannelError error = ChannelError::None;
};

struct ResourceGroupSetCpuQuotaArgs {
    char name[kMaxResourceGroupNameLength] = {};
    uint32_t nameLength = 0;
    uint32_t periodTicks = 0;  // 0 = 무제한(quotaTicks 무시)
    uint32_t quotaTicks = 0;
    ChannelError error = ChannelError::None;
};

struct ResourceGroupFreezeArgs {
    char name[kMaxResourceGroupNameLength] = {};
    uint32_t nameLength = 0;
    ChannelError error = ChannelError::None;
};

struct ResourceGroupThawArgs {
    char name[kMaxResourceGroupNameLength] = {};
    uint32_t nameLength = 0;
    ChannelError error = ChannelError::None;
};

// [SP-6A563A8F §2, 변경 없음] SP-245D130B §3가 스케치해 둔 그대로 -
// PN-158B6B2F가 이미 `usedTicksInPeriod` 증가 배선을 완료해 뒀다.
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

    // [신규, 2026-09-17, SP-B26CDBDD §6.2, PN-158B6B2F] 이 그룹 소속
    // 프로세스들의 `Process::memoryBytesUsed` 단순 합 - `Process::
    // execImage()`가 가산, `Process::destroy()`가 감산한다(coarse -
    // 정확한 페이지 단위 실시간 추적이 아니라 큰 단위 이벤트에서만).
    uint64_t totalMemoryBytesUsed = 0;
};

// [신규, 2026-09-17, SP-245D130B] 자원 그룹 - 프로세스 트리
// (Process::parent/children)와 완전히 별개의 축이다.
// [갱신, 2026-09-19, SP-6A563A8F §5-A, PN-4190BBD3 항목3/5/6] 동적
// 그룹 생성/삭제가 실제로 착수되면서, `parent`/`children`을 원래
// 각주("동적 그룹 생성이 없어 SharedPtr/EnableSharedFromThis가
// 필요 없다")가 예고한 대로 재검토했다 - `EnableSharedFromThis<
// ResourceGroup>`을 상속해 `parent`는 `WeakPtr<ResourceGroup>`(자식이
// 부모를 소유하지 않음), `children`은 `ChunkedList<SharedPtr<
// ResourceGroup>, 8>`(부모가 자식을 소유 - Process::children과 동일
// 패턴)로 바꾼다. `gRootResourceGroup`은 여전히 정적 전역이라
// `weakFromThis()`가 자연히 채워지지 않으므로, `kResourceGroupInit()`
// 이 UserThread::_selfRef와 동일한 no-op 삭제자 self-ref 트릭으로
// 채워 준다(resource_group.cpp 참고) - 그래야 루트 바로 아래 자식을
// 만들 때도 `child->parent = gRootResourceGroup.weakFromThis()`가
// 빈 WeakPtr이 아닌 진짜 값을 갖는다.
class ResourceGroup : public EnableSharedFromThis<ResourceGroup> {
public:
    Spinlock lock;
    char name[kMaxNamedObjectNameLength] = {};
    uint64_t nameLength = 0;

    WeakPtr<ResourceGroup> parent;
    static constexpr uint32_t kMaxChildrenChunkCapacity = 8;
    ChunkedList<SharedPtr<ResourceGroup>, kMaxChildrenChunkCapacity> children;

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

    // [신규, 2026-09-19, SP-6A563A8F §5-A] `weakFromThis()`가
    // protected라 외부 호출부(`ResourceGroupCreateHandler`)가 직접
    // 못 부른다 - `Process::joinResourceGroup()`/`UserThread::
    // weakAsTask()`와 동일한 이유의 공개 래퍼.
    WeakPtr<ResourceGroup> selfWeak() { return weakFromThis(); }

    // [정정, 2026-09-18, SP-76250478/PN-0EB2FABF 조사 중 발견] 이
    // 주석은 실제 구현과 이미 어긋나 있었다(freeze() 자신은 순회 없이
    // `frozen` 플래그만 세운다 - 실제 정지는 각 Task가 다음
    // `Scheduler::onTick()`을 탈 때 `kCheckAndMarkFrozen()`이 지연
    // 적용한다, resource_group.cpp 참고) - "그룹 소속 전체 프로세스의
    // 스레드가 다음 디스패치 시점에 지연 정지된다"로 정정. §4의 한계
    // 참고. 이미 다른 이유로 Blocked인 Task(디버그 정지 등)는
    // 건드리지 않는다.
    void freeze();
    // freeze()가 실제로 멈춘(Process::frozenByGroup) 것만 다시 깨운다 -
    // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] `Process::threads`
    // (process.h)의 스레드 전부를 재개 대상으로 순회한다(옛 단일
    // mainThread 재개에서 확장, resource_group.cpp 참고).
    // [신규, 2026-09-17, SP-245D130B §9-4] `debugSession.pausedByDebugger`
    // 도 함께 서 있으면 `frozenByGroup`만 내리고 실제로 깨우지는
    // 않는다(debug_session.h 문서 주석 참고) - 사유가 여러 개면 전부
    // 풀려야만 실제로 재개된다는 원칙.
    void thaw();

    // [신규, 2026-09-19, SP-6A563A8F §5-A] 동적 그룹 생성/삭제 전용 -
    // Process::allocate()/release()와 정확히 같은 관례(슬랩 alloc+
    // memset, 실제 생성자 없음 - 모든 필드의 NSDMI가 0/nullptr/빈
    // 컨테이너라 memset(0)과 비트 단위로 동일). `destroy()`는
    // `kMakeShared`의 기본 삭제자(`kDestroyAndFree<T>`)가 참조 카운트
    // 0에서 자동 호출 - 소유한 컨테이너만 정리(그 시점엔
    // `ResourceGroupDestroyHandler`가 이미 "비어있음"을 강제해 둔
    // 뒤라 실제로는 항상 빈 컨테이너의 clear()).
    static ResourceGroup* allocate();
    static void release(ResourceGroup* group);
    void destroy();
};

// 명시적으로 다른 그룹에 가입하지 않은 모든 프로세스가 속하는 루트
// 그룹 - 정적 전역 객체(진짜 C++ 생성자를 거친다, Channel/Process류의
// 슬랩+init() 관례가 아니다). `EnableSharedFromThis` 상속으로 바뀌었지만
// 이 인스턴스 자신은 여전히 슬랩이 아니라 static storage에 산다 -
// `kResourceGroupInit()`이 self-ref 트릭으로 `weakFromThis()`만
// 별도로 살려 둔다(위 클래스 문서 주석 참고).
extern ResourceGroup gRootResourceGroup;

// 부팅 시 한 번 호출 - gRootResourceGroup 이름을 채우고, self-ref
// 트릭으로 그 weakFromThis()를 영구히 유효하게 만든다.
void kResourceGroupInit();

// [SP-245D130B §4] `Scheduler::onTick()`의 재스케줄 결정 지점에서
// 호출한다 - `task`가 유저 프로세스에 속하고 그 프로세스의 그룹이
// frozen이면 `Process::frozenByGroup`를 세우고 true를 반환한다(호출부는
// 이 경우 `enqueue()` 대신 `task->state = TaskState::Blocked`로
// 전환). 커널 전용 Task(`isUserLevel == false`)나 그룹이 없는(아직
// `SpawnProcess`를 안 거친) 경우는 항상 false.
bool kCheckAndMarkFrozen(Task* task);

// [신규, 2026-09-19, SP-6A563A8F §6] `Scheduler::pickNext()`의 CPU
// 쿼터 스로틀 검사가 쓴다 - 커널 전용 Task는 그룹 소속이 아니므로
// nullptr(SP-B26CDBDD §3.2와 동일 전제).
ResourceGroup* kResourceGroupOf(Task* task);

// [신규, 2026-09-19, SP-6A563A8F §3] 그룹의 CPU 쿼터 주기가 롤오버할
// 시점이 됐으면(`periodStartTick` 이후 `periodTicks` 경과) 카운터를
// 리셋하고, 이번 주기 쿼터를 이미 소진했는지(true=스로틀 중)를
// 반환한다. `periodTicks==0`(무제한)이면 항상 false. 두 곳에서
// 호출된다 - `Scheduler::onTick()`의 카운터 증가 지점(누적 방지)과
// `Scheduler::pickNext()`의 스로틀 판정(위 문서 주석 그대로).
bool kCheckAndResetCpuPeriod(ResourceGroup* group);

// [신규, 2026-09-19, SP-6A563A8F §5] `ResourceGroupSetCpuQuota` 핸들러
// 전용 - 계층적 쿼터 강제("자식 합이 부모를 넘을 수 없다",
// `QU-3356AD2B` §9-3 확정). `changingChild`는 지금 값을 바꾸려는
// 자식 자신(형제 합산에서 제외하고 새 값으로 대체) - 새로 생성되는
// 자식이면 아직 `parent.children`에 없으므로 그냥 nullptr을 넘긴다.
// 부모가 무제한(`periodTicks==0`)이거나 `parent==nullptr`(루트 자신을
// 대상으로 하는 경우)이면 항상 true(제약 없음).
bool kValidateChildQuotaAgainstParent(ResourceGroup* parent, ResourceGroup* changingChild, uint32_t newPeriodTicks,
                                       uint32_t newQuotaTicks);

// [신규, 2026-09-19, SP-245D130B §9-2 확정("호출자가 이미 그 그룹의
// 조상 체인 안에 있을 때만 허용")] `ResourceGroupJoin`/`Create`/
// `Destroy`/`SetCpuQuota`/`Freeze`/`Thaw` 6종 syscall이 공유하는 단일
// 권한 검증 - `caller`가 속한 그룹(`caller.group`)이 `target`
// 자신이거나 그 조상 중 하나면 true. `caller.group`이 nullptr이면
// (이론상 도달 불가 - SpawnProcess/fork가 항상 최소 루트에 가입시킴)
// false.
bool kCallerInAncestorChain(Process& caller, ResourceGroup& target);

// [신규, 2026-09-19, SP-245D130B §8] 이름으로 그룹을 찾는다 - 이름은
// 트리 전체에서 전역 유일(위 Args 구조체들의 문서 주석 참고). 루트
// 자신을 포함해 트리 전체를 DFS로 순회 - 없으면 nullptr.
ResourceGroup* kFindResourceGroupByName(const char* name, uint32_t nameLength);

// [신규, 2026-09-19, SP-245D130B §8/SP-6A563A8F §5-A/§7] ResourceGroup
// syscall 6종(Join/Create/Destroy/SetCpuQuota/Freeze/Thaw) 등록.
class ResourceGroupService {
public:
    static void registerSyscallEndpoints();
};

// [신규, 2026-09-19, SP-6A563A8F §5, PN-4190BBD3 항목5] `/sys/live/
// resourcegroup/<name>/cpu.stat` VFS 읽기 노출 핸들 - `ProcFs`
// (procfs.h)의 "포인터 최하위 비트를 태그로 예약" 관례를 그대로
// 재사용한다. `ResourceGroup::allocate()`가 `GenericSlabAllocator`로
// 할당되고(동적 그룹), 그 버킷 크기가 전부 32의 배수라(`kBucketSizes`,
// minicore/libs/libkmm/slab.cpp) 슬랩 슬롯 주소의 최하위 5비트(0~4)는
// 항상 0이다 - 그중 비트3을 태그로 예약해도 실제 포인터 값과 절대
// 충돌하지 않는다.
constexpr uint64_t kResourceGroupHandleTagBit = 1ULL << 3;

// `gRootResourceGroup`은 슬랩이 아니라 정적 전역(진짜 C++ 생성자를
// 거침, 클래스 문서 주석 참고)이라 위 32바이트 정렬 보장이 적용되지
// 않는다 - 포인터에 태그 비트만 OR하는 방식은 그 정적 인스턴스의
// 실제 링크 주소가 우연히 그 비트를 이미 갖고 있으면 복원이 깨지는
// 이론적 위험이 있다(PN-4190BBD3 항목5가 착수 중 발견한 갭). 대신
// 루트는 포인터를 아예 핸들에 담지 않고 완전히 고정된 정수 핸들로
// 특별 취급한다(procfs.cpp의 `kProcFsMeminfoHandle`과 같은 관례) -
// 실제 동적 그룹 포인터는 비트0~4가 항상 0이므로 비트4까지 서 있는
// 이 상수와 절대 같아질 수 없다.
constexpr uint64_t kResourceGroupRootHandleBit = 1ULL << 4;
constexpr uint64_t kResourceGroupRootCpuStatHandle = kResourceGroupHandleTagBit | kResourceGroupRootHandleBit;

// [신규, 2026-09-19, PN-770A28FB 항목7] "resourcegroup" 자신(디렉터리
// 나열 대상) - 위 두 핸들(동적 그룹 포인터|비트3, 루트 cpu.stat)과
// 겹치지 않도록 비트5를 함께 세운다(LiveFs의 `kLiveFsRootDirHandleValue`
// 도 비트5를 쓰지만 그쪽은 `kResourceGroupHandleTagBit`(비트3) 없이
// 단독 값이라 이 상수(비트3+비트5)와 절대 같아질 수 없다). 실제 동적
// 그룹 포인터(| 비트3)와도 안 겹치는 이유는 kResourceGroupRootCpuStatHandle
// 문서 주석과 동일 - 진짜 힙 포인터는 이렇게 작은 값이 될 수 없다.
constexpr uint64_t kResourceGroupDirHandleValue = kResourceGroupHandleTagBit | (1ULL << 5);

// `LiveFs`의 "resourcegroup/" 하위 경로 위임 대상 - `ProcFs`와 동일한
// 구조(별도 계층 상속 없는 순수 헬퍼). `<name>/cpu.stat` 파일 읽기 +
// "resourcegroup" 자신의 디렉터리 나열(그룹 트리 전체를 DFS pre-order로
// 평탄화, PN-770A28FB 항목7) 둘 다 지원 - 그 외 경로는 NotFound.
// `SP-6A563A8F` §5가 `meminfo`/`uptime`(procfs.h) 선례를 명시적으로
// 재사용 대상으로 지목했으므로, 그 선례와 동일하게 호출자 권한 제약
// 없이 항상 읽을 수 있다(특정 프로세스에 종속되지 않는 그룹 통계).
// 동적 그룹 핸들은 `Process*` 핸들과 동일한 v1 한계(Open~Read 사이에
// 그룹이 Destroy되면 댕글링 가능, procfs.h의 `read()` 문서 주석과
// 동일한 처지)를 그대로 물려 받는다.
class ResourceGroupFs {
public:
    static OpenResult open(const char* relPath, uint32_t relPathLen);
    static ReadResult read(FileHandle handle, uint64_t offset, void* buf, uint32_t len);
    static void stat(const char* relPath, uint32_t relPathLen, KernelFsStatArgs* args);

    // [신규, 2026-09-19, PN-770A28FB 항목7] "resourcegroup" 나열 -
    // `index`는 트리를 루트부터 DFS pre-order로 순회했을 때 몇 번째
    // 노드인지(루트 자신도 0번째로 포함). 각 그룹은 전부 디렉터리로
    // 보고한다(`<name>/cpu.stat` 파일을 담고 있으므로).
    static void readdir(KernelFsReaddirArgs* args);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_RESOURCE_GROUP_H
