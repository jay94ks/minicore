#include "user_sync.h"

#include "libkmm/slab.h"
#include "process.h"

namespace {

using namespace kernel;

// [PN-E82744B1] Channel의 gChannelTable(channel.cpp)/Process의
// gProcessTable(process.cpp)과 완전히 같은 세대 태그 슬롯 테이블
// 관례 - Mutex/Semaphore는 서로 다른 자원 종류라 각자 독립된 테이블을
// 둔다(기존 관례상 자원 종류별로 테이블을 따로 두는 게 정상 - 하나로
// 합치면 서로 다른 타입의 핸들이 우연히 같은 인덱스/세대를 가질 때
// 구분할 방법이 없어진다). 상한 4096은 Channel의 64K/Process의
// 65535처럼 설계자가 못박은 값이 아니라 순수 구현 세부(RM-23F4B687
// §4) - 이 커널 규모에서 동시 생존 Mutex/Semaphore가 이 수를 넘을
// 실사용 시나리오가 아직 없어 넉넉히 시작한다.
constexpr uint32_t kMaxUserMutexTableSlots = 4096;
constexpr uint32_t kMaxUserSemaphoreTableSlots = 4096;

struct UserMutexTableSlot {
    UserMutex* ptr = nullptr;
    uint32_t generation = 0;
};
UserMutexTableSlot gUserMutexTable[kMaxUserMutexTableSlots];
Spinlock gUserMutexTableLock;

MutexHandle kAllocateMutexHandle(UserMutex* mutex) {
    SpinlockGuard guard(gUserMutexTableLock);
    for (uint32_t i = 0; i < kMaxUserMutexTableSlots; ++i) {
        if (gUserMutexTable[i].ptr == nullptr) {
            gUserMutexTable[i].generation++;
            gUserMutexTable[i].ptr = mutex;
            return (static_cast<uint64_t>(gUserMutexTable[i].generation) << 32) | i;
        }
    }
    return 0;  // 슬롯 고갈 - 호출부가 ResourceExhausted로 매핑
}

// 안전 해석 - 유저가 넘긴 MutexHandle을 이 함수를 거치지 않고는
// 어디서도 UserMutex*로 캐스팅하지 않는다(Channel의
// kResolveChannelId()와 동일한 원칙 - PN-CE6A04AB가 실제로 겪은
// 취약점 계열을 이 새 syscall군이 처음부터 피해 간다).
UserMutex* kResolveMutexHandle(MutexHandle handle) {
    if (handle == 0) {
        return nullptr;
    }
    const uint32_t index = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    const uint32_t generation = static_cast<uint32_t>(handle >> 32);
    if (index >= kMaxUserMutexTableSlots) {
        return nullptr;
    }
    UserMutexTableSlot& slot = gUserMutexTable[index];
    if (slot.generation != generation || slot.ptr == nullptr) {
        return nullptr;
    }
    return slot.ptr;
}

void kFreeMutexHandle(MutexHandle handle) {
    if (handle == 0) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    if (index >= kMaxUserMutexTableSlots) {
        return;
    }
    SpinlockGuard guard(gUserMutexTableLock);
    gUserMutexTable[index].ptr = nullptr;  // generation은 그대로 - 다음 재사용 때 +1
}

struct UserSemaphoreTableSlot {
    AsyncSemaphore* ptr = nullptr;
    uint32_t generation = 0;
};
UserSemaphoreTableSlot gUserSemaphoreTable[kMaxUserSemaphoreTableSlots];
Spinlock gUserSemaphoreTableLock;

SemaphoreHandle kAllocateSemaphoreHandle(AsyncSemaphore* sem) {
    SpinlockGuard guard(gUserSemaphoreTableLock);
    for (uint32_t i = 0; i < kMaxUserSemaphoreTableSlots; ++i) {
        if (gUserSemaphoreTable[i].ptr == nullptr) {
            gUserSemaphoreTable[i].generation++;
            gUserSemaphoreTable[i].ptr = sem;
            return (static_cast<uint64_t>(gUserSemaphoreTable[i].generation) << 32) | i;
        }
    }
    return 0;
}

AsyncSemaphore* kResolveSemaphoreHandle(SemaphoreHandle handle) {
    if (handle == 0) {
        return nullptr;
    }
    const uint32_t index = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    const uint32_t generation = static_cast<uint32_t>(handle >> 32);
    if (index >= kMaxUserSemaphoreTableSlots) {
        return nullptr;
    }
    UserSemaphoreTableSlot& slot = gUserSemaphoreTable[index];
    if (slot.generation != generation || slot.ptr == nullptr) {
        return nullptr;
    }
    return slot.ptr;
}

void kFreeSemaphoreHandle(SemaphoreHandle handle) {
    if (handle == 0) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    if (index >= kMaxUserSemaphoreTableSlots) {
        return;
    }
    SpinlockGuard guard(gUserSemaphoreTableLock);
    gUserSemaphoreTable[index].ptr = nullptr;
}

// WaitHandler/KillHandler 등과 동일한 관례(PN-5BBD4301 - task->
// submitterTask.lock()으로, Scheduler::currentTask()가 아니다) -
// yield를 거칠 수 있는 핸들러(Lock/Wait)는 반드시 이 값을 **core.lock()/
// acquire() 호출 전에** 미리 확보해 둬야 한다(yield 이후엔 다른
// 코어에서 재개될 수 있어 이 시점의 판단이 더는 유효하지 않다).
bool kResolveCallerProcessId(AsyncTask* task, ProcessId* outId) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    auto* callerThread = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
    SharedPtr<Process> caller = callerThread ? callerThread->process.lock() : SharedPtr<Process>();
    if (!caller) {
        return false;
    }
    *outId = caller->processId;
    return true;
}

class MutexCreateHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<MutexCreateArgs*>(argsRaw);
        void* mem = GenericSlabAllocator::alloc(sizeof(UserMutex));
        if (!mem) {
            args->handle = 0;
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        // [PN-E82744B1] 이 프로젝트 전역 관례(placement new 없이 raw
        // slab + 명시적 init())의 의도적 예외 - `UserMutex::core`
        // (AsyncMutex)는 mutex_core.h 자신이 "kMakeSharedNew()류
        // placement new로 만들어야 한다"고 명시한 진짜 C++ 생성자를
        // 가진 타입이라(내부 MutexCore::_locked를 생성자가 false로
        // 초기화), placement new 없이 raw 메모리를 그대로 UserMutex*로
        // 캐스팅하면 `_locked`가 쓰레기 값으로 남아 첫 lock()부터
        // 오동작할 수 있다.
        auto* mutex = new (mem) UserMutex();
        args->handle = kAllocateMutexHandle(mutex);
        if (args->handle == 0) {
            mutex->~UserMutex();
            GenericSlabAllocator::free(mem, sizeof(UserMutex));
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
MutexCreateHandler gMutexCreateHandler;

class MutexDestroyHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<MutexDestroyArgs*>(argsRaw);
        UserMutex* mutex = kResolveMutexHandle(args->handle);
        if (!mutex) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        kFreeMutexHandle(args->handle);
        mutex->~UserMutex();
        GenericSlabAllocator::free(mutex, sizeof(UserMutex));
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
MutexDestroyHandler gMutexDestroyHandler;

class MutexLockHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<MutexLockArgs*>(argsRaw);
        UserMutex* mutex = kResolveMutexHandle(args->handle);
        if (!mutex) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        ProcessId callerId;
        if (!kResolveCallerProcessId(task, &callerId)) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        // §17.1 그대로 - 경합 시 AsyncMutex(YieldingPolicy)가
        // AsyncTask::yield()로 리액터에 양보했다가 재개된다. yield
        // 이후에도 이 스택 프레임(mutex/callerId 지역 변수)은 그대로
        // 유효하다 - 위에서 이미 안전하게 확보해 둔 값만 쓴다.
        mutex->core.lock();
        mutex->owner = callerId;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
MutexLockHandler gMutexLockHandler;

class MutexUnlockHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<MutexUnlockArgs*>(argsRaw);
        UserMutex* mutex = kResolveMutexHandle(args->handle);
        if (!mutex) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        ProcessId callerId;
        if (!kResolveCallerProcessId(task, &callerId)) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        if (mutex->owner != callerId) {
            args->error = ChannelError::NotOwner;
            co_return;
        }
        // [순서 정정, user_sync.h 문서 주석 참고] owner를 먼저 리셋한
        // 뒤에 core.unlock()을 호출한다 - 반대 순서면 unlock() 성공과
        // 이 리셋 사이에 다른 대기자가 lock()에 성공해 owner를 채운
        // 뒤 이 스레드가 뒤늦게 그 기록을 지워버리는 경쟁이 생긴다.
        mutex->owner = kInvalidProcessId;
        mutex->core.unlock();
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
MutexUnlockHandler gMutexUnlockHandler;

class SemaphoreCreateHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SemaphoreCreateArgs*>(argsRaw);
        void* mem = GenericSlabAllocator::alloc(sizeof(AsyncSemaphore));
        if (!mem) {
            args->handle = 0;
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        // MutexCreateHandler와 동일한 이유(placement new 필요) - `explicit
        // BasicSemaphore(uint32_t)`가 진짜 생성자라 initialCount를 실제로
        // 인자로 넘겨 생성자를 거쳐야 SemaphoreCore::_count가 정확히
        // 세팅된다.
        auto* sem = new (mem) AsyncSemaphore(args->initialCount);
        args->handle = kAllocateSemaphoreHandle(sem);
        if (args->handle == 0) {
            sem->~AsyncSemaphore();
            GenericSlabAllocator::free(mem, sizeof(AsyncSemaphore));
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
SemaphoreCreateHandler gSemaphoreCreateHandler;

class SemaphoreDestroyHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SemaphoreDestroyArgs*>(argsRaw);
        AsyncSemaphore* sem = kResolveSemaphoreHandle(args->handle);
        if (!sem) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        kFreeSemaphoreHandle(args->handle);
        sem->~AsyncSemaphore();
        GenericSlabAllocator::free(sem, sizeof(AsyncSemaphore));
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
SemaphoreDestroyHandler gSemaphoreDestroyHandler;

class SemaphoreWaitHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SemaphoreWaitArgs*>(argsRaw);
        AsyncSemaphore* sem = kResolveSemaphoreHandle(args->handle);
        if (!sem) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        sem->acquire();
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
SemaphoreWaitHandler gSemaphoreWaitHandler;

class SemaphorePostHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SemaphorePostArgs*>(argsRaw);
        AsyncSemaphore* sem = kResolveSemaphoreHandle(args->handle);
        if (!sem) {
            args->error = ChannelError::NotFound;
            co_return;
        }
        sem->release();
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};
SemaphorePostHandler gSemaphorePostHandler;

}  // namespace

namespace kernel {

void UserSyncService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointMutexCreate, &gMutexCreateHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointMutexDestroy, &gMutexDestroyHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointMutexLock, &gMutexLockHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointMutexUnlock, &gMutexUnlockHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSemaphoreCreate, &gSemaphoreCreateHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSemaphoreDestroy, &gSemaphoreDestroyHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSemaphoreWait, &gSemaphoreWaitHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSemaphorePost, &gSemaphorePostHandler);
}

}  // namespace kernel
