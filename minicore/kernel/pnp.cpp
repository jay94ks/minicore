#include "pnp.h"

#include "address_space.h"
#include "libkenv/spinlock.h"
#include "paging.h"
#include "pci.h"
#include "process.h"
#include "task.h"

namespace kernel {

namespace {

// [신규] 부팅 시 스캔한 PCI 토폴로지를 캐시한다 - devmgr이
// EnumerateDevices를 반복 호출(페이지네이션)할 때마다 Pci::enumerate()
// 를 다시 도는 대신 최초 호출 시 한 번만 채운다(SP-9DD4F3EA §4a-3
// 확정대로 devmgr 인스턴스는 항상 1개뿐이라 동시 접근 경합은 사실상
// 없지만, 최초 채움 구간만 Spinlock으로 보호해 둔다). 핫플러그(§3.4,
// PN-BD9AAE2F 체크리스트 6번 - 아직 미착수)가 이 캐시를 실제로
// 무효화/재스캔하는 경로는 이 증분의 범위 밖이다.
constexpr uint32_t kMaxCachedPciDevices = 256;
DeviceDescriptor gDeviceCache[kMaxCachedPciDevices];
uint32_t gDeviceCacheCount = 0;
bool gDeviceCacheReady = false;
Spinlock gDeviceCacheLock;

// PCI-PCI 브리지(headerType != 0x00)는 표준 6-BAR 레이아웃이 아니다
// (0x10/0x14만 BAR, 나머지 오프셋은 버스 번호 등 다른 의미) - v1
// 범위 밖이라 mmioBases를 전부 0으로 남긴다.
void kFillMmioBases(const Pci::Device& dev, uint64_t (&mmioBases)[6]) {
    for (auto& v : mmioBases) {
        v = 0;
    }
    if (dev.headerType != 0x00) {
        return;
    }
    uint32_t i = 0;
    while (i < 6) {
        uint32_t raw = Pci::readConfig32(dev.bus, dev.device, dev.function, static_cast<uint8_t>(0x10 + i * 4));
        if (raw == 0 || (raw & 0x1)) {
            // 0=미구현 BAR, bit0=1이면 포트 I/O BAR(메모리 매핑 아님) - 둘 다 스킵.
            ++i;
            continue;
        }
        uint64_t base = raw & 0xFFFFFFF0u;
        uint32_t type = (raw >> 1) & 0x3;
        if (type == 0x2 && i + 1 < 6) {
            // 64비트 BAR - 다음 슬롯이 상위 32비트(그 슬롯 자체는
            // 독립된 BAR가 아니므로 건너뛴다).
            uint32_t high =
                Pci::readConfig32(dev.bus, dev.device, dev.function, static_cast<uint8_t>(0x10 + (i + 1) * 4));
            base |= (static_cast<uint64_t>(high) << 32);
            mmioBases[i] = base;
            i += 2;
            continue;
        }
        mmioBases[i] = base;
        ++i;
    }
}

void kCollectPciDevice(const Pci::Device& dev) {
    if (gDeviceCacheCount >= kMaxCachedPciDevices) {
        return;  // 실측 후 상한 조정 여지(RM-23F4B687 §4) - 지금은 조용히 버림
    }
    DeviceDescriptor& d = gDeviceCache[gDeviceCacheCount++];
    d = DeviceDescriptor{};
    d.bus = dev.bus;
    d.device = dev.device;
    d.function = dev.function;
    d.vendorId = dev.vendorId;
    d.deviceId = dev.deviceId;
    d.classCode = dev.classCode;
    d.subclass = dev.subclass;
    d.progIf = dev.progIf;
    kFillMmioBases(dev, d.mmioBases);
    d.irqVector = 0;
}

void kEnsureDeviceCache() {
    SpinlockGuard guard(gDeviceCacheLock);
    if (gDeviceCacheReady) {
        return;
    }
    gDeviceCacheCount = 0;
    Pci::enumerate(&kCollectPciDevice);
    gDeviceCacheReady = true;
}

// kEnsureDeviceCache() 이후에만 의미 있다 - 못 찾으면 nullptr.
const DeviceDescriptor* kFindCachedDevice(uint32_t bus, uint32_t device, uint32_t function) {
    for (uint32_t i = 0; i < gDeviceCacheCount; ++i) {
        const DeviceDescriptor& d = gDeviceCache[i];
        if (d.bus == bus && d.device == device && d.function == function) {
            return &d;
        }
    }
    return nullptr;
}

// [제거, 2026-09-20, SP-43331889 §3-1] 여기 있던
// `kProcessFromSubmitterForPnp(AsyncTask*)`(PN-9CC66142/DC-21647E46,
// channel.cpp의 옛 kProcessFromSubmitter와 동일한 패턴)는 `submitter`
// 를 무조건 `static_cast<UserThread*>`해 Process 없는 KernelThread
// 제출자(devmgr/fs, §1 확정)엔 잠재적 UB였다 - 아래 `kMapMmioForCaller`/
// `kClaimBar`가 `kOwnerProcessOf(Task*)`(process.h §4)와 `Task*` 직접
// 전달로 완전히 대체했다.

// [신규, 2026-09-20, SP-43331889 §3] RequestIoPermission의 MMIO 매핑 -
// UserThread 호출자는 기존과 동일하게 자신의 `Process::addressSpace`
// 에 매핑(ring3 접근 가능, PAGE_USER)하지만, KernelThread 호출자는
// 별도 주소공간이 없으므로 `IoApic::init()`(ioapic.cpp:65-77)이 이미
// 증명해 둔 패턴 그대로 예약된 고정 커널 가상주소 슬롯에
// `Paging::mapPage()`로 직접 매핑한다(PAGE_USER 없음, ring0 전용).
// v1은 커널 모드 드라이버 MMIO가 동시에 하나뿐이라는 전제(devmgr/fs
// 모두 단일 스레드) - 여러 개 동시 지원이 필요해지면 슬롯을 배열로
// 늘린다.
//
// **[미검증, 2026-09-20]** isKernelMode 분기는 아직 실제로 타는
// 호출 경로가 없다(devmgr/fs가 여전히 UserThread라 이 분기를 안 씀) -
// §3의 devmgr/fs 직접 호출 재작성이 끝나 KernelThread가 이 함수를
// 실제로 부르는 순간이 이 분기의 첫 QEMU 실측이다(SP-43331889 §3-1
// "실측 없이 진행하지 않는다" 원칙과 일관 - 다만 로직 자체는
// `IoApic::init()`의 이미 검증된 패턴을 그대로 복제했을 뿐 새로
// 발명한 게 없다).
constexpr uint64_t kKernelDriverMmioScratchVirtBase = 0xFFFF901000003000UL;  // LAPIC(lapic.cpp)+0x3000, HPET 바로 다음 페이지

bool kMapMmioForCaller(Task* caller, uint64_t physAddr, uint64_t length, uint64_t* outVirtAddr) {
    if (caller->isUserLevel) {
        SharedPtr<Process> process = kOwnerProcessOf(caller);
        if (!process) {
            return false;
        }
        return process->addressSpace.mapRegion(length, PAGE_WRITABLE | PAGE_USER | PAGE_CACHE_DISABLE,
                                                VmaBacking::FixedPhysical, physAddr, outVirtAddr);
    }
    if (caller->isKernelMode) {
        Paging::mapPage(kKernelDriverMmioScratchVirtBase, physAddr, PAGE_WRITABLE | PAGE_CACHE_DISABLE);
        *outVirtAddr = kKernelDriverMmioScratchVirtBase;
        return true;
    }
    return false;
}

// BAR 소유권 획득이 실패한 되돌리기 전용 - v1의 커널 모드 고정 슬롯은
// 재사용 예정이라 명시적으로 되돌릴 게 없다(그 슬롯을 다시 매핑하는
// 다음 호출이 자연히 덮어쓴다).
void kUnmapMmioForCaller(Task* caller, uint64_t virtAddr, uint64_t length) {
    if (caller->isUserLevel) {
        if (SharedPtr<Process> process = kOwnerProcessOf(caller)) {
            process->addressSpace.unmapRegion(virtAddr, length);
        }
    }
}

// [SP-9DD4F3EA §3.3a] BAR별 소유 프로세스 기록 - "이 BAR를 이미
// 누가 점유했는가"만 답한다. **v1 축소 - Process Teardown Hook과
// 아직 연동되지 않았다**(§3.3a가 명시한 공식 요구사항이지만
// PN-71C3D483의 teardown 경로에 이 테이블을 끼워 넣는 배선은 후속
// 계획으로 분리 - 아래 kIsBarOwned가 매 조회마다 `owner.lock()`으로
// 소유자가 이미 죽었는지 확인해 죽었으면 그 자리에서 슬롯을 회수하는
// **지연(lazy) GC**로 임시 대체한다 - 프로세스가 죽어도 그 BAR를
// 다시 요청하는 다음 호출이 있을 때까지는 슬롯이 남아있을 수 있다는
// 뜻, 명시적 즉시 반납은 아니다).
constexpr uint32_t kMaxDeviceOwnerEntries = 64;

struct DeviceOwnerEntry {
    bool used = false;
    uint32_t bus = 0, device = 0, function = 0;
    uint64_t mmioBase = 0;
    // [갱신, 2026-09-20, SP-43331889 §3-1] 원래 WeakPtr<Process> - "이
    // 슬롯의 소유자가 아직 살아있는가"만 확인하는 순수 liveness 체크라
    // (owner.lock()의 반환값 자체는 한 번도 역참조되지 않는다) Process
    // 전용일 이유가 없다 - Process 없는 KernelThread(devmgr/fs, §1)도
    // 그대로 소유자가 될 수 있게 WeakPtr<Task>로 일반화했다.
    WeakPtr<Task> owner;
};

DeviceOwnerEntry gDeviceOwners[kMaxDeviceOwnerEntries];
Spinlock gDeviceOwnerLock;

bool kMatchesOwnerEntry(const DeviceOwnerEntry& entry, uint32_t bus, uint32_t device, uint32_t function,
                         uint64_t mmioBase) {
    return entry.bus == bus && entry.device == device && entry.function == function && entry.mmioBase == mmioBase;
}

// 이미 다른(살아있는) 프로세스가 점유했으면 true - 지나가는 김에
// 죽은 소유자의 슬롯은 회수한다(위 "지연 GC" 참고).
bool kIsBarOwned(uint32_t bus, uint32_t device, uint32_t function, uint64_t mmioBase) {
    SpinlockGuard guard(gDeviceOwnerLock);
    for (auto& entry : gDeviceOwners) {
        if (!entry.used || !kMatchesOwnerEntry(entry, bus, device, function, mmioBase)) {
            continue;
        }
        if (entry.owner.lock()) {
            return true;
        }
        entry.used = false;  // 소유자가 이미 죽음 - 회수
    }
    return false;
}

// 호출 전 kIsBarOwned()로 비어있음을 이미 확인했다는 전제(그 사이
// 다른 코어가 끼어들 가능성은 이 스핀락으로 직렬화된다 - 같은 락
// 아래서 재확인 없이 바로 빈 슬롯에 꽂는 건 안전하다, 호출부가
// 항상 이 순서로만 부르는 devmgr 단일 인스턴스 전제와도 일치,
// SP-9DD4F3EA §4a-3). 빈 슬롯이 없으면 false(테이블 포화).
// [갱신, 2026-09-20, SP-43331889 §3-1] owner 파라미터를 Process 전용
// SharedPtr<Process>에서 SharedPtr<Task>로 일반화(위 DeviceOwnerEntry
// 문서 주석과 같은 이유) - 호출부는 이제 Process를 거치지 않고
// 제출자 Task 자신(task->submitterTask.resolve())을 바로 넘긴다.
bool kClaimBar(uint32_t bus, uint32_t device, uint32_t function, uint64_t mmioBase, const SharedPtr<Task>& owner) {
    SpinlockGuard guard(gDeviceOwnerLock);
    for (auto& entry : gDeviceOwners) {
        if (entry.used && kMatchesOwnerEntry(entry, bus, device, function, mmioBase) && entry.owner.lock()) {
            return false;  // 그 사이 다른 요청이 먼저 점유함
        }
    }
    for (auto& entry : gDeviceOwners) {
        if (entry.used && !entry.owner.lock()) {
            entry.used = false;  // 지나가는 김에 죽은 슬롯 회수
        }
    }
    for (auto& entry : gDeviceOwners) {
        if (!entry.used) {
            entry.used = true;
            entry.bus = bus;
            entry.device = device;
            entry.function = function;
            entry.mmioBase = mmioBase;
            entry.owner = WeakPtr<Task>(owner);
            return true;
        }
    }
    return false;
}

// [channel.cpp의 kValidateUserBuffer(PN-B552E75F)와 동일한 패턴 재사용
// - 관계도에 기록해 둠] outDevices가 호출자 자신의 유저 주소공간에
// 속하는지 제출자(submitterTask)의 실제 userPml4Phys로 검증한다 -
// onExec()이 AsyncReactor 컨텍스트에서 나중에 실행되므로 "현재 CR3"
// 기본값을 믿을 수 없다는 동일한 이유(async_task.h의 submitterTask
// 주석 참고).
bool kValidateEnumerateBuffer(AsyncTask* task, const void* ptr, uint64_t length) {
    if (length == 0) {
        return true;  // capacity=0(totalCount만 조회)이면 쓰기 자체가 없다.
    }
    SharedPtr<Task> submitter = task->submitterTask.lock();
    if (!submitter) {
        return false;
    }
    auto* thread = static_cast<UserThread*>(submitter.get());
    return Paging::isUserRangeValid(reinterpret_cast<uint64_t>(ptr), length, thread->userPml4Phys);
}

class EnumerateDevicesHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<EnumerateDevicesArgs*>(argsRaw);

        uint64_t bufferBytes = static_cast<uint64_t>(args->capacity) * sizeof(DeviceDescriptor);
        if (!kValidateEnumerateBuffer(task, args->outDevices, bufferBytes)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }

        kEnumerateDevicesSync(args->startIndex, &args->capacity, args->outDevices, &args->totalCount);
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

EnumerateDevicesHandler gEnumerateDevicesHandler;

// [SP-9DD4F3EA §3.3] devmgr(또는 devmgr이 스폰한 드라이버 자식)이
// probe() 성공 후 그 장치의 MMIO BAR 접근 권한을 요청한다. role
// 검증 없음(SP-EAB162FC §4/QU-3AAAB5E9 확정) - DeviceOwnerTable
// 소유권 확인만으로 충분.
class RequestIoPermissionHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<RequestIoPermissionArgs*>(argsRaw);
        SharedPtr<Task> caller = task->submitterTask.lock();
        if (!caller) {
            args->error = ChannelError::InvalidHandle;
            co_return;
        }
        kRequestIoPermissionSync(caller, args->bus, args->device, args->function, args->mmioBase,
                                  &args->mappedVirtualAddr, &args->assignedIrqVector, &args->error);
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

RequestIoPermissionHandler gRequestIoPermissionHandler;

}  // namespace

// [신규, 2026-09-20, SP-43331889 §3] pnp.h 선언 참고 - 외부(devmgr의
// 커널 모드 직접 호출부, §7 착수 시)에서 부를 수 있도록 익명
// 네임스페이스 밖으로 뺐다. 캐시(`gDeviceCache` 등)는 여전히 이 파일
// 안(익명 네임스페이스)에만 있고, `kEnsureDeviceCache()` 같은 내부
// 헬퍼도 같은 번역 단위 안에서는 그대로 이름 조회가 되므로 문제없다.
void kEnumerateDevicesSync(uint32_t startIndex, uint32_t* capacity, DeviceDescriptor* outDevices,
                           uint32_t* outTotalCount) {
    kEnsureDeviceCache();
    uint32_t total = gDeviceCacheCount;
    uint32_t start = startIndex;
    uint32_t filled = 0;
    if (start < total) {
        uint32_t available = total - start;
        filled = available < *capacity ? available : *capacity;
        for (uint32_t i = 0; i < filled; ++i) {
            outDevices[i] = gDeviceCache[start + i];
        }
    }
    *outTotalCount = total;
    *capacity = filled;
}

// [신규, 2026-09-20, SP-43331889 §7(fs 전환)] `RequestIoPermissionHandler::
// onExec()` 본문 - pnp.h 선언 참고. `caller`가 트랩 경로에선 이미
// `task->submitterTask.lock()`로 해석된 뒤 넘어오지만(null 가드는
// 호출부 책임), 커널 모드 직접 호출자는 자기 자신의 `Task*`를 그대로
// 넘긴다. `caller->isUserLevel && !kOwnerProcessOf(...)` 가드는
// "UserThread인데 소속 Process가 없다"는 비정상 상태만 거절할 뿐,
// KernelThread(isUserLevel==false) 호출자는 이 조건 자체가 항상
// false라 그대로 통과한다.
void kRequestIoPermissionSync(const SharedPtr<Task>& caller, uint32_t bus, uint32_t device, uint32_t function,
                               uint64_t mmioBase, uint64_t* outMappedVirtualAddr, uint32_t* outAssignedIrqVector,
                               ChannelError* outError) {
    if (caller->isUserLevel && !kOwnerProcessOf(caller.get())) {
        *outError = ChannelError::InvalidHandle;
        return;
    }

    kEnsureDeviceCache();
    const DeviceDescriptor* dev = kFindCachedDevice(bus, device, function);
    if (!dev) {
        *outError = ChannelError::NotFound;
        return;
    }

    // mmioBase가 이 장치가 실제로 광고한 BAR 중 하나인지 확인 - 그 외
    // 값은 임의 물리주소 접근 시도(보안 검증, RequestIoPermissionArgs
    // 문서 주석 참고).
    bool validBar = false;
    for (uint64_t base : dev->mmioBases) {
        if (base != 0 && base == mmioBase) {
            validBar = true;
            break;
        }
    }
    if (!validBar) {
        *outError = ChannelError::NotFound;
        return;
    }

    if (kIsBarOwned(bus, device, function, mmioBase)) {
        *outError = ChannelError::InvalidHandle;
        return;
    }

    // v1 고정 4KiB - RequestIoPermissionArgs 문서 주석의 "v1 축소
    // 범위" 참고(실제 BAR 크기 조회 절차 미구현).
    constexpr uint64_t kMappingSize = 4096;
    uint64_t mappedAddr = 0;
    if (!kMapMmioForCaller(caller.get(), mmioBase, kMappingSize, &mappedAddr)) {
        *outError = ChannelError::ResourceExhausted;
        return;
    }

    if (!kClaimBar(bus, device, function, mmioBase, caller)) {
        kUnmapMmioForCaller(caller.get(), mappedAddr, kMappingSize);
        *outError = ChannelError::ResourceExhausted;
        return;
    }

    *outMappedVirtualAddr = mappedAddr;
    *outAssignedIrqVector = 0;  // v1: MSI/MSI-X 배정 미구현(문서 주석 참고)
    *outError = ChannelError::None;
}

void PnpService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointEnumerateDevices, &gEnumerateDevicesHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointRequestIoPermission, &gRequestIoPermissionHandler);
}

}  // namespace kernel
