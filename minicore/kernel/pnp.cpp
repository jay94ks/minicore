#include "pnp.h"

#include "libkenv/spinlock.h"
#include "paging.h"
#include "pci.h"
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
        kEnsureDeviceCache();

        uint64_t bufferBytes = static_cast<uint64_t>(args->capacity) * sizeof(DeviceDescriptor);
        if (!kValidateEnumerateBuffer(task, args->outDevices, bufferBytes)) {
            args->error = ChannelError::InvalidPointer;
            co_return;
        }

        uint32_t total = gDeviceCacheCount;
        uint32_t start = args->startIndex;
        uint32_t filled = 0;
        if (start < total) {
            uint32_t available = total - start;
            filled = available < args->capacity ? available : args->capacity;
            for (uint32_t i = 0; i < filled; ++i) {
                args->outDevices[i] = gDeviceCache[start + i];
            }
        }
        args->totalCount = total;
        args->capacity = filled;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

EnumerateDevicesHandler gEnumerateDevicesHandler;

}  // namespace

void PnpService::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointEnumerateDevices, &gEnumerateDevicesHandler);
}

}  // namespace kernel
