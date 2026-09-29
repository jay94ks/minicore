#ifndef MINICORE_KERNEL_DEVICE_REGISTRY_H
#define MINICORE_KERNEL_DEVICE_REGISTRY_H

#include "channel.h"  // ChannelError, AsyncTaskWaitQueue, EpollObserverQueue(timerfd.h/signalfd.h와 동일 관례)
#include "libkenv/permission.h"  // Uid/Gid
#include "libkenv/types.h"
#include "mount_table.h"  // KernelFsDriver/FileHandle/OpenResult/ReadResult/VfsError 재사용
#include "syscall.h"

// SP-23880DC6(디바이스 이벤트/노드 계층, udev 유사 시스템) - devmgr/fs가
// 인식한 장치(v1은 AHCI 블록 장치만)를 통일된 이름공간으로 노출한다:
// 커널 자신(devmgr/fs) 소유 장치는 평평하게 `/sys/dev/<name>`, 향후
// ring3 usermode 드라이버(§5-C, 아직 실사용처 없음) 소유 장치는
// `/sys/dev/<uid>/<name>`로 중첩된다(§3.2, 평평/중첩 여부는 ownerUid
// 값 자체가 결정). `DeviceOwnerTable`(pnp.h, BAR/IRQ 자원 소유권)과는
// 완전히 다른 관심사 - 같은 실제 장치가 두 테이블 모두에 독립적으로
// 등록될 수 있다(SP-23880DC6 §0).
namespace kernel {

enum class DeviceClass : uint8_t {
    Unknown = 0,
    Block = 1,
    // Char/Network/Input 등은 실사용처(문자 장치, NIC, PnP 입력 장치)가
    // 실제로 생기면 추가한다(§5-D, RM-23F4B687 §4 - 미리 만들지 않음).
};

constexpr uint32_t kMaxDeviceRegistryEntries = 64;  // v1 상한, 실측 후 조정
constexpr uint32_t kMaxDeviceNameLength = 32;

struct DeviceRegistryEntry {
    bool used = false;
    char name[kMaxDeviceNameLength] = {};
    uint32_t nameLength = 0;
    DeviceClass deviceClass = DeviceClass::Unknown;
    // 등록한 서브시스템이 이 값의 의미를 정한다(§4) - v1 Block은
    // fs::BlockDevice*를 그대로 담는다(같은 커널 주소공간이라 안전).
    uint64_t backingHandle = 0;
    Uid ownerUid = kRootUid;
    Gid ownerGid = kRootGid;
    uint16_t mode = 0400;  // §3.6 v1 기본값(구조적 통제가 실제 접근 판정 - §3.7 참고)
    // [§4 결정1, 답변 2026-09-29] 배타적 Open 상태 - 이미 열려 있으면
    // 재차 Open을 거부한다(착수 세션이 확정하기로 한 "단순 bool").
    bool opened = false;
};

class DeviceRegistry {
public:
    static void init();

    // 성공하면 실제 배정된 이름/길이를 채운다(클래스 접두어+순번
    // 자동 생성 - §3.4, 이름 충돌은 원리적으로 불가능). 슬롯 고갈만
    // 실패 사유. 등록 성공 시 열려 있는 모든 DeviceEventsOpen fd에
    // Added 이벤트를 브로드캐스트한다(§3.3).
    static bool announce(DeviceClass cls, uint64_t backingHandle, Uid ownerUid, Gid ownerGid, uint16_t mode,
                          char* outName, uint32_t* outNameLength);
    // Removed 이벤트 브로드캐스트까지 포함(§3.3) - v1은 실제 핫언플러그
    // 경로가 없어 이론적 API(SP-9DD4F3EA §3.4 미구현).
    static void withdraw(const char* name, uint32_t nameLength);

    // 이름으로 조회 + 내부 고정 슬롯의 실제 위치(rawIndex)도 함께
    // 돌려준다 - `DeviceRegistryFs::Open()`이 이 rawIndex를 FileHandle에
    // 그대로 인코딩해 Read/Write/Close/재조회가 나중에 같은 슬롯을
    // 바로 찾게 한다(named_object.cpp의 objectId=포인터 관례와 같은
    // 취지, 다만 여기는 고정 배열이라 인덱스로 충분하다).
    static bool findByName(const char* name, uint32_t nameLength, uint32_t* outRawIndex,
                            DeviceRegistryEntry* outEntry);
    // rawIndex로 직접 조회(핸들 기반 Read/Write/Close 전용, O(1)).
    static bool resolveRaw(uint32_t rawIndex, DeviceRegistryEntry* outEntry);

    // [§3.2 Readdir/합성 네임스페이스] uid가 소유한 장치만 등록 순서
    // 그대로 index번째 - uid==kRootUid면 평평한 루트 장치들
    // (`/sys/dev/<name>` 나열), 그 외 uid는 그 uid의 서브디렉터리
    // (`/sys/dev/<uid>` 나열)에 쓴다.
    static bool getByOwnerAndIndex(Uid uid, uint32_t index, DeviceRegistryEntry* outEntry);
    // uid!=root인 항목들의 고유 uid 집합을 등장 순서대로 index번째 -
    // "/sys/dev" 루트 나열이 그 uid들을 합성 서브디렉터리로 보여주는
    // 데 쓴다(§3.2).
    static bool getNestedUidByIndex(uint32_t index, Uid* outUid);

    // [§4 결정2, 답변 2026-09-29] 배타적 Open - 이미 열려 있으면 false.
    static bool tryMarkOpenedRaw(uint32_t rawIndex);
    static void markClosedRaw(uint32_t rawIndex);
};

// fs.cpp가 구현(§4 결정2 "마운트된 장치는 Open 불가") - backingHandle
// (fs::BlockDevice*)이 현재 어떤 FileSystemDriver의 마운트 백킹으로
// 쓰이고 있는지 조회한다. device_registry.cpp가 이 프로젝트 관례(선언은
// 여기, 정의는 fs.cpp - fs.cpp만 gExt4Driver 등 구체 드라이버 인스턴스를
// 알아야 하므로 순환 include를 피한다)로 호출한다.
bool kIsBackingHandleMounted(uint64_t backingHandle);

// `/sys/dev`에 마운트되는 커널 자체 구현 드라이버(§3.2) - livefs.h의
// LiveFs와 완전히 같은 패턴(named/kernel 대신 flat + uid-nested 두
// 계층을 하나의 onExec 디스패치로 처리).
class DeviceRegistryFs : public KernelFsDriver {
public:
    static DeviceRegistryFs& instance();
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override;
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

// --- 핫플러그 이벤트(§3.3) - DeviceEventsOpen ---

// [신규, SP-23880DC6 §3.3, RM-48E1E610 그룹2(Device) call4 - 다음
// 미사용 번호로 예약] EnumerateDevices(0)/RequestIoPermission(1)/
// AllocDmaBuffer(2)/FreeDmaBuffer(3) 다음.
constexpr SyscallEndpointId kSyscallEndpointDeviceEventsOpen = kMakeSyscallEndpointId(2, 4);

struct DeviceEventsOpenArgs {
    // out
    ChannelError error = ChannelError::None;
    int64_t fd = -1;
};

enum class DeviceEventKind : uint8_t { Added = 1, Removed = 2 };

struct DeviceEvent {
    DeviceEventKind kind = DeviceEventKind::Added;
    DeviceClass deviceClass = DeviceClass::Unknown;
    char name[kMaxDeviceNameLength] = {};
    uint32_t nameLength = 0;
};

// signalfd.h의 RtSignalQueue와 완전히 동일한 패턴(고정 용량 링버퍼,
// 가득 차면 가장 오래된 것부터 버림) - "새 정책을 또 만들지 않는다"는
// SP-A7479F83 §6-B의 판단을 그대로 재사용.
class DeviceEventQueue {
public:
    static constexpr uint32_t kCapacity = 32;

    void push(const DeviceEvent& event) {
        if (_count < kCapacity) {
            const uint32_t tail = (_head + _count) % kCapacity;
            _entries[tail] = event;
            ++_count;
        } else {
            _entries[_head] = event;
            _head = (_head + 1) % kCapacity;
        }
    }

    bool pop(DeviceEvent* outEvent) {
        if (_count == 0) {
            return false;
        }
        *outEvent = _entries[_head];
        _head = (_head + 1) % kCapacity;
        --_count;
        return true;
    }

private:
    DeviceEvent _entries[kCapacity]{};
    uint32_t _head = 0;
    uint32_t _count = 0;
};

struct DeviceEventFdState {
    Spinlock lock;
    DeviceEventQueue queue;
    AsyncTaskWaitQueue pendingReaders;
    EpollObserverQueue epollReadObservers;
    void destroy() {}
};

class DeviceEventsService {
public:
    static void registerSyscallEndpoints();
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_DEVICE_REGISTRY_H
