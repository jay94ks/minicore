#include "procfs.h"

#include "libkenv/mem.h"
#include "page_frame_allocator.h"
#include "process.h"
#include "syscall.h"
#include "timer.h"

namespace {

bool kEqualsExact(const char* s, kernel::uint32_t sLen, const char* other, kernel::uint32_t otherLen) {
    return sLen == otherLen && memcmp(s, other, otherLen) == 0;
}

// status 텍스트 조립 전용 - Logger(logger.cpp)의 포맷터는 자기 내부
// 고정 버퍼로만 쓸 수 있게 캡슐화돼 있어(공개 API가 아님) 재사용할
// 수 없다. 이 파일만의 아주 좁은 용도(고정 key: value 줄 몇 개)라
// 전체 printf류를 새로 만들지 않고 필요한 두 append만 최소로 둔다.
void kAppendStr(char* buf, kernel::uint32_t bufSize, kernel::uint32_t& pos, const char* s) {
    while (*s && pos + 1 < bufSize) {
        buf[pos++] = *s++;
    }
}

void kAppendI64(char* buf, kernel::uint32_t bufSize, kernel::uint32_t& pos, kernel::int64_t value) {
    const bool neg = value < 0;
    const kernel::uint64_t mag = neg ? (~static_cast<kernel::uint64_t>(value) + 1) : static_cast<kernel::uint64_t>(value);
    char digits[24];
    kernel::uint32_t n = 0;
    kernel::uint64_t m = mag;
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

constexpr char kSelfSelector[] = "self";
constexpr char kStatusSuffix[] = "status";
constexpr kernel::uint32_t kMaxStatusLen = 256;  // §3의 5줄 정도는 넉넉히 담는 v1 상한

// [신규, 2026-09-19, PN-85FA4992] "<selector>/status" 경로를 selector
// 부분만 잘라낸다("self" 또는 10진수 pid) - 첫 '/' 앞부분을 selector로,
// 그 뒤가 정확히 "status"인지 확인한다. 여러 단계 하위 경로(예:
// "self/status/extra")는 지원하지 않는다(v1 스코프, SP-5D965B74 §2).
bool kSplitSelectorStatusPath(const char* relPath, kernel::uint32_t relPathLen, const char** outSelector,
                               kernel::uint32_t* outSelectorLen) {
    kernel::uint32_t slash = 0;
    bool found = false;
    for (kernel::uint32_t i = 0; i < relPathLen; ++i) {
        if (relPath[i] == '/') {
            slash = i;
            found = true;
            break;
        }
    }
    if (!found || slash == 0) {
        return false;
    }
    const char* rest = relPath + slash + 1;
    const kernel::uint32_t restLen = relPathLen - slash - 1;
    if (!kEqualsExact(rest, restLen, kStatusSuffix, sizeof(kStatusSuffix) - 1)) {
        return false;
    }
    *outSelector = relPath;
    *outSelectorLen = slash;
    return true;
}

// [신규, 2026-09-19, PN-85FA4992] selector가 10진수 pid 표기면
// `ProcessId`로 파싱한다("self"는 호출측에서 먼저 걸러냄) - 부호/공백/
// 선행 0 등은 허용하지 않는 가장 단순한 형태(`kFormatStatus()`가
// `kAppendI64()`로 찍는 형식과 대칭). 길이 상한(18자리)은 오버플로
// 방지용 - `ProcessId`(세대<<32|인덱스) 실제 값은 훨씬 작다.
bool kParsePidSelector(const char* s, kernel::uint32_t len, kernel::ProcessId* outPid) {
    if (len == 0 || len > 18) {
        return false;
    }
    kernel::uint64_t value = 0;
    for (kernel::uint32_t i = 0; i < len; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<kernel::uint64_t>(c - '0');
    }
    *outPid = static_cast<kernel::ProcessId>(value);
    return true;
}

// [추가, 2026-09-17, PN-0C282BB7] meminfo/uptime - self/status와 달리
// Process*가 없는 전역 핸들이라 procfs.h의 kProcFsGlobalHandleBit로
// 구분한다. 세부 번호(0/1)는 이 파일 안에서만 의미를 가지는 임의
// 식별자 - 외부에 노출되지 않는다.
constexpr char kMeminfoPath[] = "meminfo";
constexpr char kUptimePath[] = "uptime";
constexpr kernel::uint64_t kProcFsMeminfoHandle = kernel::kProcFsHandleTagBit | kernel::kProcFsGlobalHandleBit | (0ULL << 3);
constexpr kernel::uint64_t kProcFsUptimeHandle = kernel::kProcFsHandleTagBit | kernel::kProcFsGlobalHandleBit | (1ULL << 3);
constexpr kernel::uint32_t kMaxGlobalStatusLen = 320;  // meminfo가 노드 8개까지 나열할 수 있어 status보다 여유를 둠

// [신규, 2026-09-19, PN-770A28FB 항목6] `/sys/live/proc` 디렉터리
// 자신 - 같은 전역 핸들 계열의 세 번째 인덱스(0=meminfo, 1=uptime,
// 2=이 디렉터리)일 뿐, 상태가 없는 고정 sentinel이라 이 계열에
// 자연스럽게 들어맞는다.
constexpr kernel::uint64_t kProcFsRootHandle = kernel::kProcFsHandleTagBit | kernel::kProcFsGlobalHandleBit | (2ULL << 3);

struct ProcFsRootEntry {
    const char* name;
    kernel::uint32_t nameLength;
    bool isDirectory;
};
constexpr ProcFsRootEntry kProcFsRootEntries[] = {
    {"self", 4, true},
    {"meminfo", 7, false},
    {"uptime", 6, false},
};
constexpr kernel::uint32_t kProcFsRootEntryCount =
    static_cast<kernel::uint32_t>(sizeof(kProcFsRootEntries) / sizeof(kProcFsRootEntries[0]));

// [PN-0C282BB7 §1] PageFrameAllocator가 실제로 추적하는 것은 "남은
// 페이지 수"뿐(할당자 자체가 총량을 저장하지 않음, page_frame_allocator.cpp
// 확인 완료) - 그래서 Linux의 MemTotal류를 만들어내지 않고, 실제로
// 존재하는 값(전체/노드별 잔여 페이지)만 KB 단위로 노출한다
// (CLAUDE.md 규칙 4 - 실제로 없는 통계를 발명하지 않음).
kernel::uint32_t kFormatMeminfo(char* buf, kernel::uint32_t bufCap) {
    kernel::uint32_t pos = 0;
    constexpr kernel::uint64_t kPageSizeKb = 4;  // 4KiB 페이지 고정(page_frame_allocator.cpp와 동일 전제)

    kAppendStr(buf, bufCap, pos, "MemFreeKb:\t");
    kAppendI64(buf, bufCap, pos, static_cast<kernel::int64_t>(kernel::PageFrameAllocator::freePageCount() * kPageSizeKb));
    kAppendStr(buf, bufCap, pos, "\n");

    const kernel::uint32_t nodeCount = kernel::PageFrameAllocator::numaNodeCount();
    kAppendStr(buf, bufCap, pos, "NumaNodeCount:\t");
    kAppendI64(buf, bufCap, pos, static_cast<kernel::int64_t>(nodeCount));
    kAppendStr(buf, bufCap, pos, "\n");

    for (kernel::uint32_t node = 0; node < nodeCount; ++node) {
        kAppendStr(buf, bufCap, pos, "MemFreeNode");
        kAppendI64(buf, bufCap, pos, static_cast<kernel::int64_t>(node));
        kAppendStr(buf, bufCap, pos, "Kb:\t");
        kAppendI64(buf, bufCap, pos,
                   static_cast<kernel::int64_t>(kernel::PageFrameAllocator::freePageCountOnNode(node) * kPageSizeKb));
        kAppendStr(buf, bufCap, pos, "\n");
    }

    return pos;
}

// [PN-0C282BB7 §2] Timer::tickCount()는 HPET/LAPIC-PIT 경로 둘 다
// 정확히 100Hz로 증가한다(timer.cpp의 kDefaultTargetHz/kLegacyPitTargetHz
// 확인 완료 - 스케줄러 틱과 같은 시간원을 그대로 재사용, 새 타이머를
// 만들지 않음) - 즉 틱 1개가 정확히 1 centisecond다.
kernel::uint32_t kFormatUptime(char* buf, kernel::uint32_t bufCap) {
    kernel::uint32_t pos = 0;
    const kernel::uint64_t ticks = kernel::Timer::tickCount();

    kAppendStr(buf, bufCap, pos, "UptimeSeconds:\t");
    kAppendI64(buf, bufCap, pos, static_cast<kernel::int64_t>(ticks / 100));
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "UptimeCentiseconds:\t");
    kAppendI64(buf, bufCap, pos, static_cast<kernel::int64_t>(ticks));
    kAppendStr(buf, bufCap, pos, "\n");

    return pos;
}

// [SP-5D965B74 §3] 매 Read마다 그 시점의 스냅샷을 새로 만든다(status는
// 계속 바뀌는 값이라 open()~read() 사이 갱신을 반영해야 하므로 -
// LiveFs의 정적 콘텐츠와 다른 점).
kernel::uint32_t kFormatStatus(kernel::Process* proc, char* buf, kernel::uint32_t bufCap) {
    kernel::uint32_t pos = 0;

    // [수정, 2026-09-19, PN-85FA4992] 원시 포인터 값 대신 재사용 가능한
    // `ProcessId`를 찍는다 - 호출자가 이 `ParentPid` 값을 그대로 다음
    // `<selector>/status` 요청에 넣어 프로세스 트리를 거슬러 올라갈 수
    // 있어야 하므로(원시 포인터는 그 자체로 유효한 selector가 아님).
    kAppendStr(buf, bufCap, pos, "Pid:\t");
    kAppendI64(buf, bufCap, pos, proc->processId);
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "ParentPid:\t");
    kernel::SharedPtr<kernel::Process> parent = proc->parent.lock();
    kAppendI64(buf, bufCap, pos, parent ? parent->processId : kernel::kInvalidProcessId);
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "Role:\t");
    kAppendStr(buf, bufCap, pos, proc->role == kernel::ProcessRole::KernelService ? "KernelService" : "Normal");
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "Zombie:\t");
    kAppendStr(buf, bufCap, pos, proc->isZombie ? "1" : "0");
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "ThreadState:\t");
    // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] 옛 `proc->mainThread`
    // 단일 필드를 대체 - 지금은 프로세스당 스레드가 여전히 하나뿐이라
    // `threads`의 첫 번째(유일한) 스레드 상태를 보고하는 것으로
    // 관찰 가능한 동작은 동일하다. **[알려진 한계]** 스레드가 여럿이
    // 되면(`CreateThread` 착수 후) 이 한 줄짜리 요약은 더 이상 전체를
    // 대표하지 못한다 - `/proc/<pid>/task/<tid>/status`류로 쪼갤지,
    // 여러 줄로 나열할지는 그 착수 세션이 결정할 순수 구현 세부
    // (RM-23F4B687 §4).
    kernel::UserThread* firstThread = nullptr;
    proc->threads.find([&](const kernel::SharedPtr<kernel::UserThread>& t) {
        firstThread = t.get();
        return firstThread != nullptr;
    });
    if (!firstThread) {
        kAppendStr(buf, bufCap, pos, "None");
    } else {
        switch (firstThread->state) {
            case kernel::TaskState::Ready:
                kAppendStr(buf, bufCap, pos, "Ready");
                break;
            case kernel::TaskState::Running:
                kAppendStr(buf, bufCap, pos, "Running");
                break;
            case kernel::TaskState::Blocked:
                kAppendStr(buf, bufCap, pos, "Blocked");
                break;
            case kernel::TaskState::Zombie:
                kAppendStr(buf, bufCap, pos, "Zombie");
                break;
        }
    }
    kAppendStr(buf, bufCap, pos, "\n");

    return pos;
}

bool kResolveCallerProcess(kernel::AsyncTask* task, kernel::SharedPtr<kernel::Process>* outProcess) {
    // PN-5BBD4301이 확립한 유일하게 안전한 패턴 - onExec()/그 위임
    // 함수 안에서 Scheduler::currentTask()를 쓰지 않는다.
    kernel::SharedPtr<kernel::Task> submitter = task->submitterTask.lock();
    if (!submitter) {
        return false;
    }
    auto* callerThread = static_cast<kernel::UserThread*>(submitter.get());
    kernel::SharedPtr<kernel::Process> proc = callerThread->process.lock();
    if (!proc) {
        return false;
    }
    *outProcess = proc;
    return true;
}

// [신규, 2026-09-19, PN-85FA4992] `open()`/`stat()`이 공유하는 selector
// 해석 - "self"면 제출자 자신, 그 외엔 10진수 pid를 `Process::resolveById()`
// 로 재해석한 뒤 `kCanViewProcessStatus()`로 열람 권한을 확인한다(부모/
// 자신/KernelService만 허용, process.h 문서 주석 참고). `NotFound`는
// selector 문법 오류 또는 대상 pid가 이미 죽었을 때, `PermissionDenied`는
// 제출자를 못 찾았거나 권한이 없을 때.
kernel::VfsError kResolveStatusTarget(kernel::AsyncTask* task, const char* selector, kernel::uint32_t selectorLen,
                                       kernel::SharedPtr<kernel::Process>* outProc) {
    kernel::SharedPtr<kernel::Process> caller;
    if (!kResolveCallerProcess(task, &caller)) {
        return kernel::VfsError::PermissionDenied;
    }
    if (kEqualsExact(selector, selectorLen, kSelfSelector, sizeof(kSelfSelector) - 1)) {
        *outProc = caller;
        return kernel::VfsError::None;
    }
    kernel::ProcessId pid;
    if (!kParsePidSelector(selector, selectorLen, &pid)) {
        return kernel::VfsError::NotFound;
    }
    kernel::SharedPtr<kernel::Process> target = kernel::Process::resolveById(pid);
    if (!target) {
        return kernel::VfsError::NotFound;
    }
    if (!kernel::kCanViewProcessStatus(*caller, *target)) {
        return kernel::VfsError::PermissionDenied;
    }
    *outProc = target;
    return kernel::VfsError::None;
}

}  // namespace

namespace kernel {

OpenResult ProcFs::open(AsyncTask* task, const char* relPath, uint32_t relPathLen, uint32_t /*flags*/) {
    // [신규, 2026-09-19, PN-770A28FB 항목6] "proc" 자신(livefs.cpp가
    // "proc/" 접두사와 별개로 정확히 일치하는 경우 여기로 위임) -
    // 루트 디렉터리.
    if (relPathLen == 0) {
        return OpenResult{FileHandle{kProcFsRootHandle}, true, VfsError::None};
    }
    // [PN-0C282BB7] 전역 통계 파일 - 프로세스에 안 매이므로 권한
    // 판정 자체가 없다(procfs.h 참고).
    if (kEqualsExact(relPath, relPathLen, kMeminfoPath, sizeof(kMeminfoPath) - 1)) {
        return OpenResult{FileHandle{kProcFsMeminfoHandle}, false, VfsError::None};
    }
    if (kEqualsExact(relPath, relPathLen, kUptimePath, sizeof(kUptimePath) - 1)) {
        return OpenResult{FileHandle{kProcFsUptimeHandle}, false, VfsError::None};
    }

    const char* selector = nullptr;
    uint32_t selectorLen = 0;
    if (!kSplitSelectorStatusPath(relPath, relPathLen, &selector, &selectorLen)) {
        return OpenResult{FileHandle{}, false, VfsError::NotFound};
    }
    SharedPtr<Process> target;
    const VfsError err = kResolveStatusTarget(task, selector, selectorLen, &target);
    if (err != VfsError::None) {
        return OpenResult{FileHandle{}, false, err};
    }
    if (target->processId != kInvalidProcessId) {
        const uint64_t handleValue =
            (static_cast<uint64_t>(target->processId) << 4) | kProcFsHandleTagBit | kProcFsPidHandleBit;
        return OpenResult{FileHandle{handleValue}, false, VfsError::None};
    }
    // [레거시 v1 경로 유지, 2026-09-19, PN-85FA4992] `processId`가 없는
    // 커널 서비스 프로세스가 self를 여는 경우로만 도달한다 - self가
    // 아닌 pid는 `kResolveStatusTarget()`이 `Process::resolveById()`로
    // 성공 해석한 대상만 여기까지 오므로 항상 유효한 `processId`를
    // 갖는다(테이블에 등록된 것만 해석 성공, SP-9CB55C5B §2). 기존 v1과
    // 동일하게 원시 포인터 핸들로 폴백한다(procfs.h read() 문서 주석의
    // 댕글링 한계도 이 경로에서만 그대로 유지 - 새 회귀 아님).
    return OpenResult{FileHandle{reinterpret_cast<uint64_t>(target.get()) | kProcFsHandleTagBit}, false,
                       VfsError::None};
}

ReadResult ProcFs::read(FileHandle handle, uint64_t offset, void* buf, uint32_t len) {
    if ((handle.value & kProcFsHandleTagBit) == 0) {
        return ReadResult{0, VfsError::InvalidHandle};
    }

    if (handle.value & kProcFsGlobalHandleBit) {
        char global[kMaxGlobalStatusLen];
        uint32_t globalLen;
        if (handle.value == kProcFsMeminfoHandle) {
            globalLen = kFormatMeminfo(global, kMaxGlobalStatusLen);
        } else if (handle.value == kProcFsUptimeHandle) {
            globalLen = kFormatUptime(global, kMaxGlobalStatusLen);
        } else {
            return ReadResult{0, VfsError::InvalidHandle};
        }
        if (offset >= globalLen) {
            return ReadResult{0, VfsError::None};  // EOF
        }
        const uint64_t available = globalLen - offset;
        const uint32_t toCopy = static_cast<uint32_t>(available < len ? available : len);
        memcpy(buf, global + offset, toCopy);
        return ReadResult{toCopy, VfsError::None};
    }

    Process* proc = nullptr;
    SharedPtr<Process> resolved;  // pid 경로에서만 채워짐 - kFormatStatus() 동안 수명 유지
    if (handle.value & kProcFsPidHandleBit) {
        const ProcessId pid = static_cast<ProcessId>(handle.value >> 4);
        resolved = Process::resolveById(pid);
        if (!resolved) {
            // [PN-85FA4992] 대상이 Open~Read 사이에 죽었다(세대 불일치) -
            // 댕글링 역참조 대신 정상적인 "이제 없음" 경로로 처리한다.
            return ReadResult{0, VfsError::NotFound};
        }
        proc = resolved.get();
    } else {
        proc = reinterpret_cast<Process*>(handle.value & ~kProcFsHandleTagBit);
    }

    char status[kMaxStatusLen];
    const uint32_t statusLen = kFormatStatus(proc, status, kMaxStatusLen);

    if (offset >= statusLen) {
        return ReadResult{0, VfsError::None};  // EOF
    }
    const uint64_t available = statusLen - offset;
    const uint32_t toCopy = static_cast<uint32_t>(available < len ? available : len);
    memcpy(buf, status + offset, toCopy);
    return ReadResult{toCopy, VfsError::None};
}

void ProcFs::stat(AsyncTask* task, KernelFsStatArgs* args) {
    if (args->relPathLen == 0) {
        // [신규, 2026-09-19, PN-770A28FB 항목6] "proc" 자신.
        args->size = 0;
        args->type = FileType::Directory;
        args->error = VfsError::None;
        return;
    }
    if (kEqualsExact(args->relPath, args->relPathLen, kMeminfoPath, sizeof(kMeminfoPath) - 1)) {
        char global[kMaxGlobalStatusLen];
        args->size = kFormatMeminfo(global, kMaxGlobalStatusLen);
        args->type = FileType::Regular;
        args->error = VfsError::None;
        return;
    }
    if (kEqualsExact(args->relPath, args->relPathLen, kUptimePath, sizeof(kUptimePath) - 1)) {
        char global[kMaxGlobalStatusLen];
        args->size = kFormatUptime(global, kMaxGlobalStatusLen);
        args->type = FileType::Regular;
        args->error = VfsError::None;
        return;
    }

    const char* selector = nullptr;
    uint32_t selectorLen = 0;
    if (!kSplitSelectorStatusPath(args->relPath, args->relPathLen, &selector, &selectorLen)) {
        args->error = VfsError::NotFound;
        return;
    }
    SharedPtr<Process> target;
    const VfsError err = kResolveStatusTarget(task, selector, selectorLen, &target);
    if (err != VfsError::None) {
        args->error = err;
        return;
    }
    char status[kMaxStatusLen];
    args->size = kFormatStatus(target.get(), status, kMaxStatusLen);
    args->type = FileType::Regular;
    args->error = VfsError::None;
}

void ProcFs::readdir(KernelFsReaddirArgs* args) {
    if (args->dirHandle.value != kProcFsRootHandle) {
        // [v1 축소 범위] `self`(디렉터리로 표시되지만 내부는 `status`
        // 파일 하나뿐 - 그 자체를 다시 나열하는 것은 이번 범위 밖)나
        // 임의 pid 디렉터리(open/read/stat은 PN-85FA4992로 지원하지만,
        // 그 pid 아래를 `readdir()`로 나열하는 것은 여전히 스코프 밖 -
        // `self`도 마찬가지로 그 내부를 나열하지 않으므로 일관된 축소) -
        // 아직 지원하지 않는다.
        args->hasMore = false;
        args->error = VfsError::InvalidHandle;
        return;
    }
    if (args->index >= kProcFsRootEntryCount) {
        args->hasMore = false;
        args->error = VfsError::None;  // 정상 종료(Read의 EOF와 동일한 뜻)
        return;
    }
    const ProcFsRootEntry& entry = kProcFsRootEntries[static_cast<uint32_t>(args->index)];
    memcpy(args->entry.name, entry.name, entry.nameLength);
    args->entry.nameLength = entry.nameLength;
    args->entry.isDirectory = entry.isDirectory;
    args->hasMore = true;
    args->error = VfsError::None;
}

}  // namespace kernel
