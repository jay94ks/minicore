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

constexpr char kSelfStatusPath[] = "self/status";
constexpr kernel::uint32_t kMaxStatusLen = 256;  // §3의 5줄 정도는 넉넉히 담는 v1 상한

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

    kAppendStr(buf, bufCap, pos, "Pid:\t");
    kAppendI64(buf, bufCap, pos, reinterpret_cast<kernel::int64_t>(proc));
    kAppendStr(buf, bufCap, pos, "\n");

    kAppendStr(buf, bufCap, pos, "ParentPid:\t");
    kernel::SharedPtr<kernel::Process> parent = proc->parent.lock();
    kAppendI64(buf, bufCap, pos, parent ? reinterpret_cast<kernel::int64_t>(parent.get()) : -1);
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

    if (!kEqualsExact(relPath, relPathLen, kSelfStatusPath, sizeof(kSelfStatusPath) - 1)) {
        return OpenResult{FileHandle{}, false, VfsError::NotFound};
    }
    SharedPtr<Process> proc;
    if (!kResolveCallerProcess(task, &proc)) {
        return OpenResult{FileHandle{}, false, VfsError::PermissionDenied};
    }
    return OpenResult{FileHandle{reinterpret_cast<uint64_t>(proc.get()) | kProcFsHandleTagBit}, false, VfsError::None};
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

    auto* proc = reinterpret_cast<Process*>(handle.value & ~kProcFsHandleTagBit);

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
        args->isDirectory = true;
        args->error = VfsError::None;
        return;
    }
    if (kEqualsExact(args->relPath, args->relPathLen, kMeminfoPath, sizeof(kMeminfoPath) - 1)) {
        char global[kMaxGlobalStatusLen];
        args->size = kFormatMeminfo(global, kMaxGlobalStatusLen);
        args->isDirectory = false;
        args->error = VfsError::None;
        return;
    }
    if (kEqualsExact(args->relPath, args->relPathLen, kUptimePath, sizeof(kUptimePath) - 1)) {
        char global[kMaxGlobalStatusLen];
        args->size = kFormatUptime(global, kMaxGlobalStatusLen);
        args->isDirectory = false;
        args->error = VfsError::None;
        return;
    }

    if (!kEqualsExact(args->relPath, args->relPathLen, kSelfStatusPath, sizeof(kSelfStatusPath) - 1)) {
        args->error = VfsError::NotFound;
        return;
    }
    SharedPtr<Process> proc;
    if (!kResolveCallerProcess(task, &proc)) {
        args->error = VfsError::PermissionDenied;
        return;
    }
    char status[kMaxStatusLen];
    args->size = kFormatStatus(proc.get(), status, kMaxStatusLen);
    args->isDirectory = false;
    args->error = VfsError::None;
}

void ProcFs::readdir(KernelFsReaddirArgs* args) {
    if (args->dirHandle.value != kProcFsRootHandle) {
        // [v1 축소 범위] `self`(디렉터리로 표시되지만 내부는 `status`
        // 파일 하나뿐 - 그 자체를 다시 나열하는 것은 이번 범위 밖)나
        // 임의 pid 디렉터리(위 클래스 문서 주석 - QU-764C5624 답변
        // 대기) - 아직 지원하지 않는다.
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
