#include "epoll.h"

#include "async_task.h"
#include "channel.h"
#include "delayed_exec.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "paging.h"
#include "process.h"
#include "socket.h"
#include "timer.h"

namespace kernel {

namespace {

// [channel.cpp/socket.cpp/vfs_syscall.cpp와 동일한 패턴 재사용 - 각
// 파일이 자기 몫을 따로 갖는 기존 관례(RM-23F4B687 §4)] v1은 epoll
// syscall이 전부 유저 트랩 경로로만 도달한다(Process::fileDescriptors
// 자체가 Process 전용) - KernelThread 제출자는 안전하게 거절한다.
SharedPtr<Process> kProcessFromSubmitter(AsyncTask* task) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter || !submitter->isUserLevel) {
        return SharedPtr<Process>();
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return thread->process.lock();
}

bool kValidateUserBuffer(AsyncTask* task, const void* ptr, uint64_t length) {
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter || !submitter->isUserLevel) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return Paging::isUserRangeValid(reinterpret_cast<uint64_t>(ptr), length, thread->userPml4Phys);
}

// [socket.cpp/vfs_syscall.cpp와 동일한 최소 구현 복제, PN-EA4EE935 원안]
constexpr int32_t kMaxFileDescriptorValue = 1024;
int32_t kAllocateFd(Process* process) {
    for (int32_t candidate = 0; candidate < kMaxFileDescriptorValue; ++candidate) {
        if (!process->fileDescriptors.find(
                [candidate](const Process::FileDescriptor& e) { return e.fd == candidate; })) {
            return candidate;
        }
    }
    return -1;
}

// 이 EpollWait 호출 하나가 동시에 감시할 수 있는 fd 수의 v1 상한 -
// 관찰자 노드를 EpollWaitHandler::onExec()의 스택 위 고정 배열로 두기
// 위한 실용적 제약(PendingConnectRequest가 스택 위에서 yield를
// 넘나드는 것과 동일한 관례, channel.h 참고). 이보다 많은 fd를
// 등록해도 즉시 판정(레벨 스캔) 자체는 전부 훑지만, 블로킹 시 관찰자
// 등록은 앞의 이만큼만 된다(초과분은 다른 watch가 먼저 깨워 재스캔될
// 때만 함께 보고됨 - RM-23F4B687 §4, 실사용으로 더 필요해지면
// 동적 확장 검토).
constexpr uint32_t kMaxEpollWaitWatchedFds = 16;

// [SP-6350DEBB §4] fd 종류별 즉시 판정 결과 - readable/writable/
// errorFlag는 서로 배타적이지 않다(POSIX와 동일 - 에러/EOF 상태는
// readable+writable+error를 동시에 켠다).
struct EpollFdState {
    bool readable = false;
    bool writable = false;
    bool errorFlag = false;
};

// 소켓 fd 하나의 상태를 조회한다 - listening/connected 두 경우만
// 다룬다(그 외, 즉 아직 bind/connect 전인 소켓은 항상 준비 안 됨).
EpollFdState kQuerySocketState(AsyncTask* callerTask, UnixSocket* socket) {
    EpollFdState state;
    if (socket->listening) {
        SharedPtr<Channel> channel = kResolveChannelId(socket->channelId);
        if (!channel) {
            // 채널이 이미 파괴됨 - POSIX EPOLLHUP류, readable+error로 보고.
            state.readable = true;
            state.errorFlag = true;
            return state;
        }
        SpinlockGuard guard(channel->lock);
        state.readable = channel->pendingHead != nullptr;
        return state;
    }
    if (socket->bridge != 0) {
        SharedPtr<BridgePipe> bridge = kResolveOwnedBridge(callerTask, socket->bridge);
        if (!bridge) {
            state.readable = true;
            state.writable = true;
            state.errorFlag = true;
            return state;
        }
        const bool broken = kIsBridgeBroken(bridge.get());
        if (socket->readShutdown || broken) {
            state.readable = true;
        } else {
            SharedPtr<BridgePipe> peer = bridge->peer.lock();
            if (peer) {
                SpinlockGuard guard(peer->outbound.lock);
                state.readable = peer->outbound.used > 0;
            }
        }
        {
            SpinlockGuard guard(bridge->outbound.lock);
            state.writable = broken || (bridge->outbound.capacity - bridge->outbound.used) > 0;
        }
        state.errorFlag = broken;
        return state;
    }
    return state;  // 아직 bind/connect 전 - 절대 준비되지 않음
}

// [SP-6350DEBB §4] 이 fd의 다음 상태 변화를 감시하려면 어디에
// 관찰자를 등록해야 하는지도 소켓 상태 조회와 완전히 같은 분기를
// 타므로, 등록/해제를 이 한 함수로 함께 처리한다 - `add`가 true면
// 등록(pushBack), false면 해제(remove, 이미 없으면 안전한 no-op).
// `readNode`/`writeNode`는 호출부(EpollWaitHandler::onExec)의 스택
// 위에 살아있는 전용 슬롯 - 한 AsyncTask가 여러 fd/방향에 동시에
// 등록되려면 반드시 서로 다른 노드가 필요하다(channel.h의
// EpollObserverQueue 문서 주석 참고 - AsyncTask::next 재사용 큐와
// 달리, 이 큐는 애초에 이 문제 때문에 별도 노드 타입을 쓴다).
void kUpdateSocketObserver(AsyncTask* callerTask, UnixSocket* socket, uint32_t interestMask, bool wantReadable,
                            bool wantWritable, EpollObserverNode* readNode, EpollObserverNode* writeNode, bool add) {
    if (socket->listening) {
        if (!wantReadable) {
            return;
        }
        SharedPtr<Channel> channel = kResolveChannelId(socket->channelId);
        if (!channel) {
            return;
        }
        SpinlockGuard guard(channel->lock);
        if (add) {
            readNode->task = callerTask;
            channel->acceptObservers.pushBack(readNode);
        } else {
            channel->acceptObservers.remove(readNode);
        }
        return;
    }
    if (socket->bridge == 0) {
        return;
    }
    SharedPtr<BridgePipe> bridge = kResolveOwnedBridge(callerTask, socket->bridge);
    if (!bridge) {
        return;
    }
    if (wantReadable) {
        SharedPtr<BridgePipe> peer = bridge->peer.lock();
        if (peer) {
            SpinlockGuard guard(peer->outbound.lock);
            if (add) {
                readNode->task = callerTask;
                peer->outbound.readObservers.pushBack(readNode);
            } else {
                peer->outbound.readObservers.remove(readNode);
            }
        }
    }
    if (wantWritable) {
        SpinlockGuard guard(bridge->outbound.lock);
        if (add) {
            writeNode->task = callerTask;
            bridge->outbound.writeObservers.pushBack(writeNode);
        } else {
            bridge->outbound.writeObservers.remove(writeNode);
        }
    }
    (void)interestMask;
}

EpollFdState kQueryFdState(AsyncTask* callerTask, Process::FileDescriptor* fdEntry) {
    switch (fdEntry->kind) {
        case MountKind::Socket:
            return kQuerySocketState(callerTask, fdEntry->socket.get());
        case MountKind::KernelDriver: {
            // [SP-6350DEBB §4] 디스크 파일은 블로킹 개념이 없다 - POSIX
            // select/poll/epoll 전부 이렇게 취급(항상 준비됨).
            EpollFdState state;
            state.readable = true;
            state.writable = true;
            return state;
        }
        default:
            // Channel(원 IPC, 미구현)/Epoll(중첩, v1 미구현) - 준비 안 됨
            // 취급. EpollCtl(Add)가 애초에 이 kind들을 거절하므로(아래
            // EpollCtlHandler 참고) 정상 경로에서는 도달하지 않는다.
            return EpollFdState{};
    }
}

void kUpdateFdObserver(AsyncTask* callerTask, Process::FileDescriptor* fdEntry, uint32_t interestMask,
                        EpollObserverNode* readNode, EpollObserverNode* writeNode, bool add) {
    if (fdEntry->kind != MountKind::Socket) {
        return;  // KernelDriver는 항상 준비됨이라 관찰 대상이 될 상태 변화 자체가 없음
    }
    const bool wantReadable = (interestMask & static_cast<uint32_t>(EpollEventMask::Readable)) != 0;
    const bool wantWritable = (interestMask & static_cast<uint32_t>(EpollEventMask::Writable)) != 0;
    kUpdateSocketObserver(callerTask, fdEntry->socket.get(), interestMask, wantReadable, wantWritable, readNode,
                          writeNode, add);
}

bool kIsWatchableKind(MountKind kind) { return kind == MountKind::Socket || kind == MountKind::KernelDriver; }

class EpollCreateHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<EpollCreateArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        void* mem = GenericSlabAllocator::alloc(sizeof(EpollInstance));
        if (!mem) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        memset(mem, 0, sizeof(EpollInstance));
        auto* raw = reinterpret_cast<EpollInstance*>(mem);
        raw->init();
        SharedPtr<EpollInstance> instance = kMakeShared<EpollInstance>(raw);
        if (!instance) {
            GenericSlabAllocator::free(mem, sizeof(EpollInstance));
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        instance->watches.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);

        process->fileDescriptors.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        const int32_t newFd = kAllocateFd(process.get());
        if (newFd < 0) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        Process::FileDescriptor fdEntry;
        fdEntry.fd = newFd;
        fdEntry.kind = MountKind::Epoll;
        fdEntry.epollInstance = instance;
        fdEntry.used = true;
        if (!process->fileDescriptors.insert(fdEntry)) {
            args->error = ChannelError::ResourceExhausted;
            co_return;
        }
        args->fd = newFd;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

class EpollCtlHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<EpollCtlArgs*>(argsRaw);
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        auto* epSlot = process->fileDescriptors.find(
            [epfd = args->epfd](const Process::FileDescriptor& e) { return e.fd == epfd; });
        if (!epSlot || epSlot->value.kind != MountKind::Epoll || !epSlot->value.epollInstance) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        SharedPtr<EpollInstance> instance = epSlot->value.epollInstance;

        if (args->op == EpollCtlOp::Add) {
            auto* targetSlot = process->fileDescriptors.find(
                [fd = args->targetFd](const Process::FileDescriptor& e) { return e.fd == fd; });
            if (!targetSlot || !kIsWatchableKind(targetSlot->value.kind)) {
                // [SP-6350DEBB §7] Channel(원 IPC 미구현)/Epoll(중첩,
                // v1 미구현)을 감시하려는 시도는 정직하게 거절한다.
                args->error = ChannelError::NotSupported;
                co_return;
            }
            if (targetSlot->value.fd == args->epfd) {
                args->error = ChannelError::InvalidArgument;  // [SP-6350DEBB §3-A] 자기 자신 감시 금지
                co_return;
            }
            SpinlockGuard guard(instance->lock);
            if (instance->watches.find([fd = args->targetFd](const EpollWatch& w) { return w.targetFd == fd; })) {
                args->error = ChannelError::AlreadyExists;
                co_return;
            }
            EpollWatch watch;
            watch.targetFd = args->targetFd;
            watch.interestMask = args->mask;
            watch.userData = args->userData;
            if (!instance->watches.insert(watch)) {
                args->error = ChannelError::ResourceExhausted;
                co_return;
            }
            args->error = ChannelError::None;
            co_return;
        }

        if (args->op == EpollCtlOp::Mod) {
            SpinlockGuard guard(instance->lock);
            auto* watchSlot = instance->watches.find(
                [fd = args->targetFd](const EpollWatch& w) { return w.targetFd == fd; });
            if (!watchSlot) {
                args->error = ChannelError::NotFound;
                co_return;
            }
            watchSlot->value.interestMask = args->mask;
            watchSlot->value.userData = args->userData;
            args->error = ChannelError::None;
            co_return;
        }

        if (args->op == EpollCtlOp::Del) {
            SpinlockGuard guard(instance->lock);
            auto* watchSlot = instance->watches.find(
                [fd = args->targetFd](const EpollWatch& w) { return w.targetFd == fd; });
            if (!watchSlot) {
                args->error = ChannelError::NotFound;
                co_return;
            }
            instance->watches.erase(watchSlot);
            args->error = ChannelError::None;
            co_return;
        }

        args->error = ChannelError::InvalidArgument;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

// [SP-6350DEBB §5] 100Hz(10ms/틱) 고정 시간원 가정 - timer.h 문서
// 주석("HPET 있으면 그쪽, 없으면 LAPIC/PIT 100Hz 폴백 - 어느 쪽이든
// tickCount() 의미는 동일") 그대로. 런타임에 실제 Hz를 조회하는 API가
// 아직 없어(RM-23F4B687 §4, 필요해지면 그때 추가) 이 프로젝트의 다른
// 곳(HPET/LAPIC 초기화 코드 자체)과 동일하게 100Hz를 그대로 가정한다.
constexpr uint64_t kEpollTimerHz = 100;

uint64_t kMsToTicksCeil(int64_t ms) {
    if (ms <= 0) {
        return 0;
    }
    const uint64_t u = static_cast<uint64_t>(ms);
    return (u * kEpollTimerHz + 999) / 1000;
}

// scheduleTimeout()(cancelSource.trigger()만 함)과 달리, EpollWait의
// for(;;) { ...; yield(); } 대기 루프를 실제로 깨워야 하므로
// AsyncReactor::submitCompletion()을 직접 부른다 - kOnAsyncTaskTimeout
// (async_task.cpp)과 동일한 약한 참조 보호 패턴(대상이 이미 끝나
// weakRef가 무효화됐으면 조용히 아무 일도 안 함).
void kOnEpollWaitTimeout(void* arg) {
    auto* ref = static_cast<AsyncTaskWeakRef*>(arg);
    AsyncTask* task = ref->lock();
    if (task) {
        AsyncReactor::submitCompletion(task);
    }
    ref->release();
}

class EpollWaitHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<EpollWaitArgs*>(argsRaw);
        if (args->maxEvents == 0 || !args->outEvents) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        if (!kValidateUserBuffer(task, args->outEvents, sizeof(EpollReadyEvent) * args->maxEvents)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }
        SharedPtr<Process> process = kProcessFromSubmitter(task);
        if (!process) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        auto* epSlot = process->fileDescriptors.find(
            [epfd = args->epfd](const Process::FileDescriptor& e) { return e.fd == epfd; });
        if (!epSlot || epSlot->value.kind != MountKind::Epoll || !epSlot->value.epollInstance) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        SharedPtr<EpollInstance> instance = epSlot->value.epollInstance;  // yield 넘나들며 계속 소유

        int32_t watchFds[kMaxEpollWaitWatchedFds];
        uint32_t watchMasks[kMaxEpollWaitWatchedFds];
        uint64_t watchUserData[kMaxEpollWaitWatchedFds];
        EpollObserverNode readNodes[kMaxEpollWaitWatchedFds];
        EpollObserverNode writeNodes[kMaxEpollWaitWatchedFds];
        bool registered[kMaxEpollWaitWatchedFds] = {};

        uint64_t deadlineTick = 0;
        bool hasDeadline = false;
        AsyncTaskWeakRef* timeoutRef = nullptr;
        uint64_t timeoutToken = 0;

        for (;;) {
            uint32_t watchCount = 0;
            {
                SpinlockGuard guard(instance->lock);
                instance->watches.forEach([&](EpollWatch& w, void*) {
                    if (watchCount < kMaxEpollWaitWatchedFds) {
                        watchFds[watchCount] = w.targetFd;
                        watchMasks[watchCount] = w.interestMask;
                        watchUserData[watchCount] = w.userData;
                        ++watchCount;
                    }
                });
            }

            uint32_t reportedCount = 0;
            for (uint32_t i = 0; i < watchCount && reportedCount < args->maxEvents; ++i) {
                auto* targetSlot = process->fileDescriptors.find(
                    [fd = watchFds[i]](const Process::FileDescriptor& e) { return e.fd == fd; });
                if (!targetSlot) {
                    continue;  // [SP-6350DEBB §6] 감시 대상이 그 사이 닫힘 - v1은 조용히 건너뜀
                }
                EpollFdState state = kQueryFdState(task, &targetSlot->value);
                uint32_t events = 0;
                if ((watchMasks[i] & static_cast<uint32_t>(EpollEventMask::Readable)) && state.readable) {
                    events |= static_cast<uint32_t>(EpollEventMask::Readable);
                }
                if ((watchMasks[i] & static_cast<uint32_t>(EpollEventMask::Writable)) && state.writable) {
                    events |= static_cast<uint32_t>(EpollEventMask::Writable);
                }
                if (state.errorFlag) {
                    events |= static_cast<uint32_t>(EpollEventMask::Error);
                }
                if (events != 0) {
                    EpollReadyEvent ev;
                    ev.events = events;
                    ev.userData = watchUserData[i];
                    args->outEvents[reportedCount] = ev;
                    ++reportedCount;
                }
            }

            // 이전 루프에서 등록해 둔 관찰자를 전부 해제 - 재스캔 결과와
            // 무관하게(반환하든 다시 등록하든) 매번 깨끗한 상태에서
            // 다시 시작한다.
            for (uint32_t i = 0; i < watchCount; ++i) {
                if (!registered[i]) {
                    continue;
                }
                auto* targetSlot = process->fileDescriptors.find(
                    [fd = watchFds[i]](const Process::FileDescriptor& e) { return e.fd == fd; });
                if (targetSlot) {
                    kUpdateFdObserver(task, &targetSlot->value, watchMasks[i], &readNodes[i], &writeNodes[i], false);
                }
                registered[i] = false;
            }

            if (reportedCount > 0) {
                if (timeoutToken != 0) {
                    DelayedExecutionQueue::cancel(timeoutToken);
                }
                args->count = reportedCount;
                args->error = ChannelError::None;
                co_return;
            }

            if (args->timeoutMs == 0) {
                args->count = 0;
                args->error = ChannelError::None;
                co_return;
            }

            if (hasDeadline && Timer::tickCount() >= deadlineTick) {
                args->count = 0;
                args->error = ChannelError::None;
                co_return;
            }

            if (args->timeoutMs > 0 && !hasDeadline) {
                hasDeadline = true;
                deadlineTick = Timer::tickCount() + kMsToTicksCeil(args->timeoutMs);
                timeoutRef = task->ensureWeakRef();
                if (timeoutRef) {
                    timeoutRef->addRef();
                    timeoutToken = DelayedExecutionQueue::schedule(kMsToTicksCeil(args->timeoutMs),
                                                                    &kOnEpollWaitTimeout, timeoutRef);
                }
            }

            for (uint32_t i = 0; i < watchCount; ++i) {
                auto* targetSlot = process->fileDescriptors.find(
                    [fd = watchFds[i]](const Process::FileDescriptor& e) { return e.fd == fd; });
                if (targetSlot) {
                    kUpdateFdObserver(task, &targetSlot->value, watchMasks[i], &readNodes[i], &writeNodes[i], true);
                    registered[i] = true;
                }
            }

            AsyncTask::yield();
        }
    }
    void onFailure(AsyncTask*) override {}

    // [PN-C4611402류] 취소(강제 종료 등)되면 등록해 둔 관찰자 전부를
    // 정리해야 댕글링 노드가 남지 않는다 - 다만 이 노드들은 이
    // AsyncTask 자신의 스택(이제 곧 반납될 스택) 위에 있으므로, onExec
    // 쪽 루프가 일반적으로 매 반복 해제하는 것과 별개로, 취소된
    // 그 순간에 등록돼 있었을 수 있는 나머지를 안전하게 남겨 두지
    // 않는다. v1은 대상 fd를 다시 조회할 방법이 args만으로는 없어
    // (watch 목록 자체가 onExec 스택 지역 변수) 이 정리를 못 한다 -
    // 대신 각 관찰자 큐 자체가 이미 "노드가 가리키는 AsyncTask가 곧
    // 사라진다"는 사실을 모른 채 남아있는 위험을 아래처럼 완화한다:
    // 이 노드들이 가리키는 큐(RingBuffer/Channel)의 다음 이벤트 발생
    // 시점에 노드가 popFront()로 꺼내지면 submitCompletion(task)이
    // 호출될 텐데, `task`는 이미 취소/반납된 뒤라 AsyncReactor가
    // 이를 안전하게 처리해야 한다(기존 AsyncTaskWaitQueue 소비자들도
    // 동일한 일반 원칙에 기댄다 - AsyncTask 자체의 반납은 항상
    // kReleaseAsyncTask()를 통해서만 일어나고, 그 전까지 구조체 메모리
    // 자체는 유효하게 남는다는 프레임워크 불변조건). 실사용 빈도가
    // 확인되면 Del과 동일한 명시적 해제 경로를 추가하는 후속 검토가
    // 필요하다(PN-7562DA62 항목10 "정리" 범위로 남김).
    void onCancel(AsyncTask*, void*) override {}
};

EpollCreateHandler gEpollCreateHandler;
EpollCtlHandler gEpollCtlHandler;
EpollWaitHandler gEpollWaitHandler;

}  // namespace

void Epoll::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointEpollCreate, &gEpollCreateHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointEpollCtl, &gEpollCtlHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointEpollWait, &gEpollWaitHandler);
}

}  // namespace kernel
