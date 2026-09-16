#include "procfs.h"

#include "libkenv/mem.h"
#include "process.h"
#include "syscall.h"

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
    if (!proc->mainThread) {
        kAppendStr(buf, bufCap, pos, "None");
    } else {
        switch (proc->mainThread->state) {
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

}  // namespace kernel
