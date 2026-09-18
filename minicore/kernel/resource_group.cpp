#include "resource_group.h"

#include "libkenv/mem.h"
#include "process.h"
#include "scheduler.h"
#include "syscall.h"
#include "timer.h"

namespace kernel {

// 정적 전역 - 실제 C++ 정적 초기화를 거친다(NSDMI 그대로 유효,
// resource_group.h의 클래스 주석 참고).
ResourceGroup gRootResourceGroup;

namespace {
constexpr char kRootResourceGroupName[] = "root";

// [신규, 2026-09-19, SP-6A563A8F §5-A] gRootResourceGroup은 정적
// 전역이라 절대 GenericSlabAllocator::free()로 반납돼선 안 된다 -
// UserThread::_selfRef(syscall.h)와 동일한 no-op 삭제자 관례.
void kNoOpReleaseResourceGroup(ResourceGroup*) {}

// gRootResourceGroup.weakFromThis()가 영구히 유효하도록 붙잡아 두는
// 강한 참조 - 이 SharedPtr 자체가 사라지면 컨트롤 블록의 강한 카운트가
// 0이 돼 weakFromThis()가 죽은 참조를 돌려주므로, 파일 스코프 정적으로
// 커널 수명 내내 살려 둔다.
SharedPtr<ResourceGroup> gRootResourceGroupSelfRef;

bool kNamesEqual(const char* a, uint32_t aLen, const char* b, uint32_t bLen) {
    if (aLen != bLen) {
        return false;
    }
    for (uint32_t i = 0; i < aLen; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

void kCopyName(char* dst, uint64_t dstCapacity, const char* src, uint32_t srcLen) {
    const uint32_t copyLen = srcLen < dstCapacity ? srcLen : static_cast<uint32_t>(dstCapacity);
    memcpy(dst, src, copyLen);
}

// [신규, 2026-09-19, SP-245D130B §8] 이름으로 그룹을 찾는 DFS - 루트
// 자신부터 확인.
ResourceGroup* kFindResourceGroupByNameFrom(ResourceGroup* node, const char* name, uint32_t nameLength) {
    if (!node) {
        return nullptr;
    }
    if (kNamesEqual(node->name, static_cast<uint32_t>(node->nameLength), name, nameLength)) {
        return node;
    }
    ResourceGroup* found = nullptr;
    node->children.forEach([&](SharedPtr<ResourceGroup>& child, auto*) {
        if (found || !child) {
            return;
        }
        found = kFindResourceGroupByNameFrom(child.get(), name, nameLength);
    });
    return found;
}

}  // namespace

void kResourceGroupInit() {
    memcpy(gRootResourceGroup.name, kRootResourceGroupName, sizeof(kRootResourceGroupName) - 1);
    gRootResourceGroup.nameLength = sizeof(kRootResourceGroupName) - 1;
    // [신규, 2026-09-19, SP-6A563A8F §5-A] self-ref 트릭 - resource_group.h
    // 클래스 문서 주석 참고. Resurrect(재부팅 없는 재사용)가 이 정적
    // 전역에는 적용되지 않으므로(커널 자체가 한 번만 부팅) 매번 다시
    // 만들 필요 없이 여기서 한 번만 채우면 커널 수명 내내 유효하다.
    gRootResourceGroupSelfRef = kMakeShared<ResourceGroup>(&gRootResourceGroup, &kNoOpReleaseResourceGroup);
}

void ResourceGroup::addMember(const WeakPtr<Process>& proc) {
    SpinlockGuard guard(lock);
    memberProcesses.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    memberProcesses.insert(proc);
}

void ResourceGroup::removeMember(Process* proc) {
    if (!proc) {
        return;
    }
    SpinlockGuard guard(lock);
    auto* slot = memberProcesses.find([proc](const WeakPtr<Process>& weak) {
        // lock() 없이 원시 포인터로 비교 - 이 시점에 proc은 호출자가
        // 이미 SharedPtr/raw로 살아있음을 보장한 상태에서만 불린다
        // (Process::joinResourceGroup의 "옛 그룹에서 빼기" 경로).
        SharedPtr<Process> locked = weak.lock();
        return locked.get() == proc;
    });
    if (slot) {
        memberProcesses.erase(slot);
    }
}

void ResourceGroup::freeze() {
    SpinlockGuard guard(lock);
    if (frozen) {
        return;
    }
    frozen = true;
    // 이미 Ready 상태로 대기 중이던 Task는 여기서 당장 멈추지 못한다
    // (클래스 주석의 한계 참고) - 다음에 그 Task가 실제로 디스패치돼
    // Scheduler::onTick()을 거칠 때 kCheckAndMarkFrozen()이 잡는다.
    // 이 함수 자신은 순수하게 플래그만 세운다(추가 순회 불필요 -
    // frozen 하나만 보면 되므로 memberProcesses를 지금 당장 건드릴
    // 실익이 없다).
}

void ResourceGroup::thaw() {
    ChunkedList<SharedPtr<Process>, kMaxMemberChunkCapacity> toWake;
    {
        SpinlockGuard guard(lock);
        if (!frozen) {
            return;
        }
        frozen = false;
        toWake.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        memberProcesses.forEach([&](WeakPtr<Process>& weak, auto*) {
            SharedPtr<Process> proc = weak.lock();
            if (proc && proc->frozenByGroup) {
                toWake.insert(proc);
            }
        });
    }
    // 락 밖에서 실제로 깨운다(Scheduler::enqueue 자체가 별도 cli
    // 임계구역을 쓰므로 lock을 쥔 채 부를 이유가 없다 - 락 중첩 최소화).
    //
    // [신규, 2026-09-17, SP-245D130B §9-4 답변] `frozenByGroup`은 항상
    // 내려놓지만(그룹 자신은 실제로 풀렸으므로), `debugSession.
    // pausedByDebugger`가 함께 서 있으면 실제로 깨우지는 않는다 -
    // 디버거가 이 프로세스를 세워 둔 이유는 그룹과 무관하게 독립적으로
    // 남아 있어야 한다(debug_session.h의 `pausedByDebugger` 문서 주석
    // 참고 - 반대 방향은 미래의 `DebugContinue`가 책임진다).
    toWake.forEach([](SharedPtr<Process>& proc, auto*) {
        proc->frozenByGroup = false;
        // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] 옛 `proc->mainThread`
        // 단일 재개를 `proc->threads` 전체 순회로 대체 - 지금은 프로세스당
        // 스레드가 여전히 하나뿐이라 관찰 가능한 동작은 동일하다.
        if (!proc->debugSession.pausedByDebugger) {
            proc->threads.forEach([](SharedPtr<UserThread>& threadRef, auto*) {
                if (UserThread* t = threadRef.get()) {
                    Scheduler::enqueue(Scheduler::currentCoreIndex(), t);
                }
            });
        }
    });
}

ResourceGroup* ResourceGroup::allocate() {
    void* raw = GenericSlabAllocator::alloc(sizeof(ResourceGroup));
    if (!raw) {
        return nullptr;
    }
    memset(raw, 0, sizeof(ResourceGroup));
    return reinterpret_cast<ResourceGroup*>(raw);
}

void ResourceGroup::release(ResourceGroup* group) {
    GenericSlabAllocator::free(group, sizeof(ResourceGroup));
}

void ResourceGroup::destroy() {
    // ResourceGroupDestroyHandler가 호출 전 이미 "비어있음"을 강제해
    // 두므로 실제로는 항상 빈 컨테이너의 clear()다 - 방어적으로만.
    children.clear();
    memberProcesses.clear();
}

bool kCheckAndMarkFrozen(Task* task) {
    if (!task->isUserLevel) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(task);
    SharedPtr<Process> proc = thread->process.lock();
    if (!proc || !proc->group) {
        return false;
    }
    if (!proc->group->frozen) {
        return false;
    }
    proc->frozenByGroup = true;
    return true;
}

ResourceGroup* kResourceGroupOf(Task* task) {
    if (!task->isUserLevel) {
        return nullptr;  // 커널 자체 Task는 그룹 소속 아님(SP-B26CDBDD §3.2와 동일 전제)
    }
    auto* thread = static_cast<UserThread*>(task);
    SharedPtr<Process> proc = thread->process.lock();
    return proc ? proc->group : nullptr;
}

bool kCheckAndResetCpuPeriod(ResourceGroup* group) {
    if (!group || group->cpu.periodTicks == 0) {
        return false;  // 무제한 그룹 - 스로틀 대상 아님
    }
    const uint64_t now = Timer::tickCount();
    if (now - group->cpu.periodStartTick >= group->cpu.periodTicks) {
        group->cpu.periodStartTick = now;
        group->cpu.usedTicksInPeriod = 0;
    }
    return group->cpu.usedTicksInPeriod >= group->cpu.quotaTicks;
}

bool kValidateChildQuotaAgainstParent(ResourceGroup* parent, ResourceGroup* changingChild, uint32_t newPeriodTicks,
                                       uint32_t newQuotaTicks) {
    if (!parent || parent->cpu.periodTicks == 0) {
        return true;  // 부모 무제한(또는 대상이 루트 자신) - 항상 허용
    }
    // 쿼터를 "비율"(quota/period)로 정규화해 비교한다 - 자식마다 다른
    // periodTicks를 가질 수 있으므로 절대값(quotaTicks) 합산은 의미가
    // 없다. 고정소수점 스케일은 SP-B26CDBDD §2.1의 kVruntimeScale과
    // 같은 이유로 정수 나눗셈 0-버림을 피한다.
    constexpr uint64_t kRatioScale = 1000000;  // ppm 단위
    auto ratioOf = [](uint32_t quota, uint32_t period) -> uint64_t {
        return period == 0 ? 0 : (static_cast<uint64_t>(quota) * kRatioScale) / period;
    };
    uint64_t siblingSumRatio = 0;
    parent->children.forEach([&](SharedPtr<ResourceGroup>& child, auto*) {
        ResourceGroup* c = child.get();
        if (!c || c == changingChild) {
            return;  // 변경 대상 자신은 새 값으로 아래에서 따로 더함
        }
        siblingSumRatio += ratioOf(c->cpu.quotaTicks, c->cpu.periodTicks);
    });
    siblingSumRatio += ratioOf(newQuotaTicks, newPeriodTicks);
    const uint64_t parentRatio = ratioOf(parent->cpu.quotaTicks, parent->cpu.periodTicks);
    return siblingSumRatio <= parentRatio;
}

bool kCallerInAncestorChain(Process& caller, ResourceGroup& target) {
    ResourceGroup* callerGroup = caller.group;
    if (!callerGroup) {
        return false;  // 이론상 도달 불가 - SpawnProcess/fork가 항상 최소 루트에 가입시킴
    }
    ResourceGroup* cur = &target;
    while (cur) {
        if (cur == callerGroup) {
            return true;
        }
        SharedPtr<ResourceGroup> parent = cur->parent.lock();
        cur = parent.get();
    }
    return false;
}

ResourceGroup* kFindResourceGroupByName(const char* name, uint32_t nameLength) {
    return kFindResourceGroupByNameFrom(&gRootResourceGroup, name, nameLength);
}

namespace {

// [신규, 2026-09-19, SP-6A563A8F §5] cpu.stat 텍스트 조립 전용 -
// procfs.cpp의 kAppendStr/kAppendI64와 완전히 동일한 최소 헬퍼(그
// 파일의 주석 그대로 - Logger 포맷터는 재사용 불가능한 캡슐화라 이
// 파일만의 아주 좁은 용도로 다시 최소 구현).
void kAppendStr(char* buf, uint32_t bufSize, uint32_t& pos, const char* s) {
    while (*s && pos + 1 < bufSize) {
        buf[pos++] = *s++;
    }
}

void kAppendI64(char* buf, uint32_t bufSize, uint32_t& pos, int64_t value) {
    const bool neg = value < 0;
    const uint64_t mag = neg ? (~static_cast<uint64_t>(value) + 1) : static_cast<uint64_t>(value);
    char digits[24];
    uint32_t n = 0;
    uint64_t m = mag;
    if (m == 0) {
        digits[n++] = '0';
    }
    while (m) {
        digits[n++] = static_cast<char>('0' + (m % 10));
        m /= 10;
    }
    if (neg) {
        kAppendStr(buf, bufSize, pos, "-");
    }
    while (n) {
        if (pos + 1 < bufSize) {
            buf[pos++] = digits[--n];
        } else {
            n = 0;
        }
    }
}

constexpr char kCpuStatFileName[] = "cpu.stat";
constexpr uint32_t kMaxCpuStatLen = 160;  // 5줄 정도(PeriodTicks/QuotaTicks/UsedTicksInPeriod/TotalCpuTicks/Frozen)

// relPath("<name>/cpu.stat")에서 이름 길이만 분리한다 - 이름 자신은
// '/'를 포함할 수 없다는 전제(named_object.h류 평평한 이름 공간과
// 동일 관례)이므로 첫 '/'를 구분자로 삼는다.
bool kSplitResourceGroupPath(const char* relPath, uint32_t relPathLen, uint32_t* outNameLen) {
    for (uint32_t i = 0; i < relPathLen; ++i) {
        if (relPath[i] == '/') {
            *outNameLen = i;
            return true;
        }
    }
    return false;
}

// [SP-6A563A8F §5] 매 Read/Stat마다 그 시점의 스냅샷을 새로 조립한다
// (procfs.cpp의 status/meminfo와 동일한 원칙) - 락 없이 읽는다(다른
// 코어가 동시에 cpu 필드를 갱신 중이면 값이 살짝 튈 수 있으나, 통계
// 텍스트 파일 하나의 필드 몇 개라 procfs.cpp의 기존 status/meminfo
// 판독도 동일하게 락을 안 쓰는 것과 같은 수준의 정밀도로 충분하다고
// 판단).
uint32_t kFormatCpuStat(ResourceGroup* group, char* buf, uint32_t bufCap) {
    uint32_t pos = 0;

    kAppendStr(buf, bufCap, pos, "PeriodTicks:\t");
    kAppendI64(buf, bufCap, pos, static_cast<int64_t>(group->cpu.periodTicks));
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "QuotaTicks:\t");
    kAppendI64(buf, bufCap, pos, static_cast<int64_t>(group->cpu.quotaTicks));
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "UsedTicksInPeriod:\t");
    kAppendI64(buf, bufCap, pos, static_cast<int64_t>(group->cpu.usedTicksInPeriod));
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "TotalCpuTicks:\t");
    kAppendI64(buf, bufCap, pos, static_cast<int64_t>(group->accounting.totalCpuTicks));
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "Frozen:\t");
    kAppendStr(buf, bufCap, pos, group->frozen ? "1" : "0");
    kAppendStr(buf, bufCap, pos, "\n");

    return pos;
}

}  // namespace

OpenResult ResourceGroupFs::open(const char* relPath, uint32_t relPathLen) {
    uint32_t nameLen = 0;
    if (!kSplitResourceGroupPath(relPath, relPathLen, &nameLen)) {
        return OpenResult{FileHandle{}, false, VfsError::NotFound};
    }
    const char* file = relPath + nameLen + 1;
    const uint32_t fileLen = relPathLen - nameLen - 1;
    if (!kNamesEqual(file, fileLen, kCpuStatFileName, sizeof(kCpuStatFileName) - 1)) {
        return OpenResult{FileHandle{}, false, VfsError::NotFound};
    }
    ResourceGroup* group = kFindResourceGroupByName(relPath, nameLen);
    if (!group) {
        return OpenResult{FileHandle{}, false, VfsError::NotFound};
    }
    if (group == &gRootResourceGroup) {
        return OpenResult{FileHandle{kResourceGroupRootCpuStatHandle}, false, VfsError::None};
    }
    return OpenResult{FileHandle{reinterpret_cast<uint64_t>(group) | kResourceGroupHandleTagBit}, false,
                       VfsError::None};
}

ReadResult ResourceGroupFs::read(FileHandle handle, uint64_t offset, void* buf, uint32_t len) {
    if ((handle.value & kResourceGroupHandleTagBit) == 0) {
        return ReadResult{0, VfsError::InvalidHandle};
    }
    ResourceGroup* group = (handle.value & kResourceGroupRootHandleBit)
                               ? &gRootResourceGroup
                               : reinterpret_cast<ResourceGroup*>(handle.value & ~kResourceGroupHandleTagBit);

    char statText[kMaxCpuStatLen];
    const uint32_t statLen = kFormatCpuStat(group, statText, kMaxCpuStatLen);

    if (offset >= statLen) {
        return ReadResult{0, VfsError::None};  // EOF
    }
    const uint64_t available = statLen - offset;
    const uint32_t toCopy = static_cast<uint32_t>(available < len ? available : len);
    memcpy(buf, statText + offset, toCopy);
    return ReadResult{toCopy, VfsError::None};
}

void ResourceGroupFs::stat(const char* relPath, uint32_t relPathLen, KernelFsStatArgs* args) {
    uint32_t nameLen = 0;
    if (!kSplitResourceGroupPath(relPath, relPathLen, &nameLen)) {
        args->error = VfsError::NotFound;
        return;
    }
    const char* file = relPath + nameLen + 1;
    const uint32_t fileLen = relPathLen - nameLen - 1;
    if (!kNamesEqual(file, fileLen, kCpuStatFileName, sizeof(kCpuStatFileName) - 1)) {
        args->error = VfsError::NotFound;
        return;
    }
    ResourceGroup* group = kFindResourceGroupByName(relPath, nameLen);
    if (!group) {
        args->error = VfsError::NotFound;
        return;
    }
    char statText[kMaxCpuStatLen];
    args->size = kFormatCpuStat(group, statText, kMaxCpuStatLen);
    args->isDirectory = false;
    args->error = VfsError::None;
}

namespace {

class ResourceGroupJoinHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ResourceGroupJoinArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        ResourceGroup* target = kFindResourceGroupByName(args->name, args->nameLength);
        if (!target) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (!kCallerInAncestorChain(*caller, *target)) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }
        caller->joinResourceGroup(target);
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ResourceGroupJoinHandler gResourceGroupJoinHandler;

class ResourceGroupCreateHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ResourceGroupCreateArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        ResourceGroup* parent = args->parentNameLength == 0
                                     ? &gRootResourceGroup
                                     : kFindResourceGroupByName(args->parentName, args->parentNameLength);
        if (!parent) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (!kCallerInAncestorChain(*caller, *parent)) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }
        // 이름 유일성 - SP-245D130B §8의 원 제안(형제간 유일)보다 넓게,
        // 트리 전체에서 유일하도록 강제한다 - `kFindResourceGroupByName()`
        // (Join/SetCpuQuota/Freeze/Thaw/Destroy 전부가 재사용하는 조회
        // 함수)이 이름 하나로 그룹 하나를 찾는 평평한 조회를 전제하므로,
        // 형제간에만 유일하면 트리 다른 곳의 동명 그룹과 뒤섞인다(착수
        // 세션이 실측 코드 조사로 발견해 넓힌 것 - RM-23F4B687 §4).
        if (kFindResourceGroupByName(args->name, args->nameLength)) {
            args->error = ChannelError::AlreadyExists;
            co_return;
        }

        ResourceGroup* raw = ResourceGroup::allocate();
        if (!raw) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        SharedPtr<ResourceGroup> child = kMakeShared<ResourceGroup>(raw);
        if (!child) {
            ResourceGroup::release(raw);
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        kCopyName(child->name, sizeof(child->name), args->name, args->nameLength);
        child->nameLength = args->nameLength;
        child->parent = parent->selfWeak();
        // CPU 컨트롤(§2)은 기본값 그대로 무제한(periodTicks=0) - 생성
        // 시점에 자동으로 쿼터가 걸리지 않는다(명시적으로
        // ResourceGroupSetCpuQuota를 따로 호출해야 함).
        parent->children.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        parent->children.insert(child);  // 부모가 진짜 소유자(SharedPtr)
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ResourceGroupCreateHandler gResourceGroupCreateHandler;

class ResourceGroupDestroyHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ResourceGroupDestroyArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        ResourceGroup* target = kFindResourceGroupByName(args->name, args->nameLength);
        if (!target) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        SharedPtr<ResourceGroup> parent = target->parent.lock();
        if (!parent) {
            args->error = ChannelError::PermissionDenied;  // 루트 그룹은 삭제 불가
            co_return;
        }
        if (!kCallerInAncestorChain(*caller, *target)) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }
        // ChunkedList에 empty()가 없어(libkenv/chunked_list.h) forEach로
        // 직접 확인한다 - 청크가 비어 있으면 콜백이 한 번도 안 불린다.
        bool childrenEmpty = true;
        target->children.forEach([&](SharedPtr<ResourceGroup>&, auto*) { childrenEmpty = false; });
        bool membersEmpty = true;
        target->memberProcesses.forEach([&](WeakPtr<Process>&, auto*) { membersEmpty = false; });
        if (!childrenEmpty || !membersEmpty) {
            args->error = ChannelError::NotEmpty;  // Linux cgroup과 동일한 "비어있음 강제"
            co_return;
        }
        auto* slot = parent->children.find(
            [target](const SharedPtr<ResourceGroup>& c) { return c.get() == target; });
        if (slot) {
            parent->children.erase(slot);  // SharedPtr 해제 - 참조 카운트 0에서 자기 소멸(destroy())
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ResourceGroupDestroyHandler gResourceGroupDestroyHandler;

class ResourceGroupSetCpuQuotaHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ResourceGroupSetCpuQuotaArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        ResourceGroup* target = kFindResourceGroupByName(args->name, args->nameLength);
        if (!target) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (!kCallerInAncestorChain(*caller, *target)) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }
        if (args->periodTicks != 0 && args->quotaTicks > args->periodTicks) {
            args->error = ChannelError::InvalidArgument;  // 주기보다 많은 실행 시간은 무의미
            co_return;
        }
        SharedPtr<ResourceGroup> parent = target->parent.lock();
        if (!kValidateChildQuotaAgainstParent(parent.get(), target, args->periodTicks, args->quotaTicks)) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }

        target->cpu.periodTicks = args->periodTicks;
        target->cpu.quotaTicks = args->quotaTicks;
        // [확정, 2026-09-18, QU-8ED9EBD2 답변 - "설정 시점부터 새 주기가
        // 깨끗하게 시작"] 재설정 이전 주기의 사용량이 새 주기로 넘어와
        // 즉시 스로틀 상태가 되지 않도록 여기서 함께 리셋한다 -
        // kCheckAndResetCpuPeriod() 자체는 자연 롤오버 전용(이 즉시
        // 리셋은 이 핸들러만의 책임).
        target->cpu.usedTicksInPeriod = 0;
        target->cpu.periodStartTick = Timer::tickCount();
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ResourceGroupSetCpuQuotaHandler gResourceGroupSetCpuQuotaHandler;

class ResourceGroupFreezeHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ResourceGroupFreezeArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        ResourceGroup* target = kFindResourceGroupByName(args->name, args->nameLength);
        if (!target) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (!kCallerInAncestorChain(*caller, *target)) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }
        target->freeze();
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ResourceGroupFreezeHandler gResourceGroupFreezeHandler;

class ResourceGroupThawHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<ResourceGroupThawArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
        if (!caller) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        ResourceGroup* target = kFindResourceGroupByName(args->name, args->nameLength);
        if (!target) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (!kCallerInAncestorChain(*caller, *target)) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }
        target->thaw();
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

ResourceGroupThawHandler gResourceGroupThawHandler;

}  // namespace

void ResourceGroupService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointResourceGroupJoin, &gResourceGroupJoinHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointResourceGroupCreate, &gResourceGroupCreateHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointResourceGroupDestroy, &gResourceGroupDestroyHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointResourceGroupSetCpuQuota, &gResourceGroupSetCpuQuotaHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointResourceGroupFreeze, &gResourceGroupFreezeHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointResourceGroupThaw, &gResourceGroupThawHandler);
}

}  // namespace kernel
