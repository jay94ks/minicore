// fs 커널 서비스: VFS 마운트 지점의 실질 처리를 담당하는 커널 서비스
// (SP-8B6B8D25 §4-A, PN-452FF696 항목3) - devmgr/pubreg/init과 같은
// 이유("libmc를 통해서만 커널에 요청한다"는 모양부터 갖춘다)로 신설.
//
// **[완료, 이 파일] SP-8B6B8D25 §4 마운트 지점 목록 사전 등록**: 부팅
// 즉시 이름 없는 Channel을 하나 개설하고(§2.2 - VFS 라우팅은 이름이
// 아니라 MountTable의 channelId로 이뤄지므로 pubreg류 "이름 있는
// Channel"과 달리 이름이 필요 없다), §4가 명시적으로 "일반 FS"로
// 태그한 네 개의 최상위 지점(/sys/etc, /sys/bin, /sys/mnt, /sys/tmp)
// 을 그 Channel로 Mount한다.
//
// **[정직하게 기록, 이 증분의 스코프 결정]** §4가 예시로 든 나머지
// 후보 - `usr/*/mnt`(사용자별 경로, 아직 사용자 계정/`/usr/<name>`
// 생성 모델 자체가 없음)와 `/boot/uefi`(UEFI 부팅 경로 전용, 이
// 프로젝트는 아직 PVH/multiboot2만 지원 - PN-7FBF255A 미착수) -는
// 이번 증분에서 제외했다. `/sys/dev`(devmgr 담당)/`/sys/live`(커널
// 자신이 이미 `mountKernel()`로 직접 마운트, SP-7CC5693A §2.4)는
// 애초에 이 서비스의 대상이 아니다.
//
// **[미착수] 실제 파일시스템 드라이버 연결(§3.1a/§3.2)과 Open/Read
// 프로토콜(§9, SP-2AAD7C8D)**: 이 네 마운트 지점 뒤에 아직 어떤
// 실제 파일시스템도 없다(libext4/libswapfs/libvfat 전부 "예정,
// 미구현") - 이번 증분은 각 지점이 라우팅 테이블에 정확히 등록되고
// 그 소유 Channel의 accept 왕복이 실제로 동작하는지만 검증한다
// (accept 즉시 close, pubreg 항목3과 동일 패턴).
//
// **[신규, 2026-09-20, QU-1FB6A7A4 답변 - "블록 디바이스는 그냥 아예
// fs한테 던져버려. 인식/인식 해제까지 전부."]** 블록 스토리지 장치
// (AHCI 등)의 PCI 열거/매칭/드라이버 구동을 devmgr이 아니라 이
// 프로세스가 직접 수행한다 - devmgr이 fork()로 스폰한 자식이 개설한
// 이름 없는 Channel을 이 프로세스가 나중에 찾아야 하는 문제
// (SP-C2670F69 §3.1이 남겨 둔 설계 공백) 자체가 이 결정으로 사라진다
// (같은 프로세스 안이므로 핸드오프가 필요 없음). AHCI 실제 하드웨어
// 코드는 ahci.h/ahci.cpp(PN-4E6EA13D/PN-F60E405A, devmgr.cpp에서 이관) -
// `AhciBlockDevice`(block_device.h `fs::BlockDevice` 구현)까지 이
// 증분에서 구성하지만, 그걸 실제 `FileSystemDriver::mount()`(§3.1a,
// libext4/libvfat 자체가 아직 미구현)에 넘기는 건 여전히 범위 밖 -
// PN-452FF696 항목5.
//
// **[전환, 2026-09-20, SP-43331889/QU-5FC58B06 - fs를 유저랜드
// 프로세스에서 Process 없는 순수 커널 Task로 완전 흡수]** 설계자
// 답변(QU-5FC58B06) - "전부 옮긴 후에 맵핑을 후순위로 미뤄": devmgr
// 파일럿(§7-1-a)과 똑같은 패턴으로 fs 전체(VFS 마운트 라우팅 +
// AHCI 인식/구동 전부)를 Process 없는 KernelThread로 옮기되,
// `AllocDmaBuffer`의 커널 모드 매핑 설계 자체는 아직 정하지 않는다
// (dma_buffer.h 문서 주석 참고) - 그 결과 AHCI 실제 I/O(ahci.cpp가
// 요구하는 모든 DMA 버퍼 alloc)는 그 설계가 결정되기 전까지 전부
// 우아하게 실패한다(VFS 마운트 지점 라우팅 자체엔 영향 없음). 이
// 파일은 더 이상 유저랜드 ELF(예전엔 `crt0.S`가 넘겨주던 `_start()`
// 였다)가 아니다 - `kmain.cpp`가 `kSpawnKernelThread(kFsKernelMain,
// nullptr)`로 직접 띄우는 ring0 `KernelThread`의 entry 함수다.
// `libmc`(트랩 기반 syscall 왕복)는 더 이상 쓰지 않고 커널 내부 동기
// 함수를 직접 호출한다(devmgr과 동일한 이유 - 같은 주소공간, 트랩
// 자체가 무의미). 6개 syscall 중 유일하게 블로킹하는
// `AcceptFromChannel`만은 `AsyncTask::submit()` 직접 호출 +
// `submitterTask` 수동 캡처 + `AsyncTaskWaitGroup::waitAll()` 패턴을
// 쓴다(SP-43331889 §3-3 설계, 이 전환이 그 첫 실제 실행이다).
// **[정리, 2026-09-21, 설계자 지시, PN-D6A05E78]** 예전 유저랜드
// 진입점 `crt0.S`(devmgr과 공유하던 파일)와 이 디렉터리 자체
// (`minicore/fs`)를 완전히 제거하고 이 파일을 `minicore/kernel/fs.cpp`
// 로, ahci.h/ahci.cpp/block_device.h도 같은 디렉터리로 옮겼다 -
// `minicore_kernel` 소스 목록(CMakeLists.txt)에 직접 들어간다.
#include "ahci.h"
#include "async_task.h"
#include "channel.h"
#include "fs_service.h"
#include "mount_table.h"
#include "pnp.h"
#include "scheduler.h"
#include "syscall.h"
#include "task.h"

namespace kernel {

namespace {

// v1 매칭 후보는 AHCI(classCode=1 "Mass Storage"/subclass=6 "SATA")
// 하나뿐. progIf(AHCI 1.0=1)는 아직 구분 안 함.
constexpr uint32_t kPciClassMassStorage = 1;
constexpr uint32_t kPciSubclassSata = 6;
constexpr uint32_t kMaxDevices = 64;

bool kMatchesAhci(const DeviceDescriptor& dev) {
    return dev.classCode == kPciClassMassStorage && dev.subclass == kPciSubclassSata;
}

// `DeviceDescriptor::mmioBases[6]`은 BAR 인덱스 그대로(mmioBases[0]=
// BAR0 ... mmioBases[5]=BAR5) - AHCI의 ABAR는 관례상 BAR5(실측: QEMU
// ich9-ahci). 첫 번째 실제로 존재하는(0이 아닌) BAR를 찾아 쓴다.
uint64_t kFirstMmioBase(const DeviceDescriptor& dev) {
    for (uint64_t base : dev.mmioBases) {
        if (base != 0) {
            return base;
        }
    }
    return 0;
}

// fs 프로세스 하나당 AHCI 컨트롤러/블록 장치 인스턴스는 최대 1개(v1 -
// 컨트롤러 여러 개/포트 여러 개를 각각 BlockDevice로 노출하는 건
// 후속 과제, 지금은 §3.1 "최소한의 실제 I/O 검증" 수준).
ahci::AhciController gAhciController;
ahci::AhciBlockDevice gAhciBlockDevice;
bool gHasAhciBlockDevice = false;

// devmgr에서 하던 EnumerateDevices->매칭->RequestIoPermission->HBA
// 초기화까지 그대로 이 KernelThread 안에서 직접 호출로 수행한다 -
// 별도 프로세스/Channel 핸드오프가 필요 없다(devmgr 파일럿과 동일한
// 구조). 실패해도(장치 없음/권한 실패/DMA 매핑 미구현) 이 서비스는
// 계속 살아있어야 한다 - VFS 마운트 지점 라우팅 자체는 블록 장치
// 유무와 무관하게 계속 동작해야 하므로.
void kProbeAndInitAhci(const SharedPtr<Task>& self) {
    DeviceDescriptor devices[kMaxDevices];
    uint32_t capacity = kMaxDevices;
    uint32_t totalCount = 0;
    kEnumerateDevicesSync(0, &capacity, devices, &totalCount);
    const uint32_t deviceCount = capacity;  // capacity는 실제로 채운 개수로 덮어써진다

    for (uint32_t i = 0; i < deviceCount; ++i) {
        if (!kMatchesAhci(devices[i]) || kFirstMmioBase(devices[i]) == 0) {
            continue;
        }

        uint64_t mappedAddr = 0;
        uint32_t irqVector = 0;
        ChannelError error = ChannelError::None;
        kRequestIoPermissionSync(self, devices[i].bus, devices[i].device, devices[i].function,
                                  kFirstMmioBase(devices[i]), &mappedAddr, &irqVector, &error);
        if (error != ChannelError::None) {
            continue;  // 이 장치 실패 - 다음 후보로(RM-23F4B687 §4, 장치 하나 실패가 서비스 전체를 막으면 안 됨)
        }

        if (!gAhciController.init(mappedAddr)) {
            continue;
        }

        ahci::PortProbeResult probeResult;
        ahci::AhciPort* port = nullptr;
        if (gAhciController.probeFirstDevice(&probeResult, &port)) {
            gAhciBlockDevice.init(port, probeResult.identifyData);
            gHasAhciBlockDevice = true;
        }
        return;  // v1은 첫 매칭 성공 장치 하나만(§3.1 최소 검증 범위)
    }
}

struct MountPointSpec {
    const char* path;
    uint32_t pathLen;
};

constexpr char kMountEtc[] = "/sys/etc";
constexpr char kMountBin[] = "/sys/bin";
constexpr char kMountMnt[] = "/sys/mnt";
constexpr char kMountTmp[] = "/sys/tmp";

constexpr MountPointSpec kMountPoints[] = {
    {kMountEtc, sizeof(kMountEtc) - 1},
    {kMountBin, sizeof(kMountBin) - 1},
    {kMountMnt, sizeof(kMountMnt) - 1},
    {kMountTmp, sizeof(kMountTmp) - 1},
};
constexpr uint32_t kMountPointCount = sizeof(kMountPoints) / sizeof(kMountPoints[0]);

// [신규, 2026-09-20, SP-43331889 §3-3] fs가 실제로 쓰는 6개 syscall
// 중 유일하게 블로킹하는 AcceptFromChannel 전용 - 트랩(Syscall::
// submit()/wait())은 `Scheduler::currentTask()`를 무조건
// `UserThread*`로 캐스팅해 KernelThread 자기-트랩이 성립하지 않는다
// (§3-2에서 이미 확인됨). 그래서 `Syscall::submit()`이 내부적으로
// 하는 일(AsyncTask::submit + submitterTask 캡처)만 직접 하고, 완료
// 대기는 `AsyncTaskAwaiter`(코루틴 전용) 대신 평범한 Task 레벨
// 블로킹인 `AsyncTaskWaitGroup::waitAll()`을 쓴다 - syscall.cpp의
// `Syscall::submit()` 본문과 동일한 자원 관리 규약(autoFree=false,
// preemptive=true)을 그대로 따른다.
void kAcceptFromChannelDirect(const SharedPtr<Task>& self, AcceptFromChannelArgs* args) {
    AsyncTaskSubjectCode subjectCode = 0;
    if (!SyscallRegistry::resolveSubjectCode(kSyscallEndpointAcceptFromChannel, &subjectCode)) {
        args->error = ChannelError::NotFound;
        return;
    }
    AsyncTask* task = nullptr;
    {
        // [실측으로 발견, syscall.cpp의 Syscall::submit() 선례 그대로]
        // AsyncTask::submit()은 내부에서 곧바로 AsyncReactor::
        // submitCompletion()을 불러 이 코어의 실행 큐에 올린다 - 그
        // 직후 submitterTask를 채우기 전에 리액터가 먼저 끼어들어
        // onExec()을 실행해 버리면(예: 이 스레드가 즉시 선점됨) 아직
        // 비어 있는 submitterTask를 역참조하게 된다. 이 스코프 전체를
        // 선점 금지로 감싸 "제출 -> submitterTask 캡처"를 원자적
        // 구간으로 만든다.
        PreemptionGuard guard;
        task = AsyncTask::submit(subjectCode, 0, args, /*autoFree=*/false, /*preemptive=*/true);
        if (!task) {
            args->error = ChannelError::ResourceExhausted;
            return;
        }
        task->submitterTask = TaskOwnerRef::capture(WeakPtr<Task>(self));
    }

    AsyncTaskWaitGroup group;
    group.add(task);
    group.waitAll();
}

}  // namespace

// [교체, 2026-09-20, SP-43331889 §7] `kSpawnKernelThread()`가 새
// `KernelThread`의 TaskTcb에 이 함수 포인터를 직접 실어 최초 진입 시
// 호출한다(Task::init() 재사용 - SP-43331889 §2 참고) - `arg`는 현재
// 안 씀(항상 nullptr로 스폰).
void kFsKernelMain(void* /*arg*/) {
    auto* self = static_cast<KernelThread*>(Scheduler::currentTask());
    SharedPtr<Task> selfShared = self->weakAsTask().lock();

    ChannelId channelId = 0;
    BridgeHandle channelHandle = 0;
    ChannelError openError = ChannelError::None;
    kOpenNamelessChannelSync(selfShared, &channelId, &channelHandle, &openError);
    if (openError != ChannelError::None) {
        // 자원 고갈 등 - 이 서비스는 계속 존재할 이유가 없다. 예전
        // 유저랜드 시절엔 essential Process 정책이 이 상황을 패닉으로
        // 처리했으나, Process 없는 KernelThread는 그 안전망이 없다
        // (§7-1 미해결 갭) - 자연 반환도 위험하므로(같은 이유) 대신
        // 무한 대기로 조용히 멈춘다.
        for (;;) {
            asm volatile("pause");
        }
    }

    for (uint32_t i = 0; i < kMountPointCount; ++i) {
        MountTable::mount(kMountPoints[i].path, kMountPoints[i].pathLen, channelId);
    }

    // 블록 스토리지 장치 인식/구동 - VFS 마운트 지점 라우팅 등록
    // 이후, accept 루프 진입 전에 한 번(실패해도 이 프로세스는 계속
    // 존재 - kProbeAndInitAhci() 문서 주석 참고).
    kProbeAndInitAhci(selfShared);

    for (;;) {
        AcceptFromChannelArgs acceptArgs;
        acceptArgs.channelHandle = channelHandle;
        kAcceptFromChannelDirect(selfShared, &acceptArgs);
        if (acceptArgs.error != ChannelError::None) {
            break;
        }

        // TODO(§9 Open/Read 프로토콜, SP-2AAD7C8D - 아직 미확정): 실제
        // 파일 열기/읽기/쓰기는 여기서 ChannelRead/ChannelWrite로
        // 처리해야 하지만 그 프로토콜 자체가 없어 연결만 받고 바로
        // 닫는다(accept 왕복 자체의 실측 검증 목적, pubreg 항목3과
        // 동일 패턴).
        ChannelError closeError = ChannelError::None;
        kCloseBridgeSync(selfShared, acceptArgs.bridge, &closeError);
    }

    // §7-1 미해결 갭(kTaskOnFallingToEnd의 Kernel-Level 분기가 정리
    // 없이 즉시 퇴역시킴) 때문에 devmgr과 동일하게 자연 반환 대신
    // 무한 대기로 마무리한다 - accept 루프가 깨졌다는 것 자체가 이미
    // 비정상 상황(Channel이 파괴됨 등)이라 재시도할 것도 없다.
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace kernel
