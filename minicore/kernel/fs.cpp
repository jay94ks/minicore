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
// **[갱신, 2026-09-22, PN-452FF696 항목5 완료]** 아래 `kTryAutoMountBlockDevice()`
// 가 §3.1 우선순위(ext4 → FAT32)로 실제 `gAhciBlockDevice`에 `Ext4Driver`/
// `Fat32Driver::mount()`를 시도해 슈퍼블록을 판별하고, 성공하면 4개
// 마운트 지점 중 `/sys/mnt`의 Channel 등록을 해제하고 그 자리에 실제
// `KernelDriver`로 다시 마운트한다 - 이때부터 `/sys/mnt`는 실제 디스크
// 내용을 서비스한다. **[정정, 2026-09-22, PN-452FF696 참고]** 이 절이
// 원래 "§9 Open/Read syscall 프로토콜이 아직 없다"고 적어 뒀던 건
// 착오였다 - `vfs_syscall.cpp`의 9개 syscall 핸들러(Open/Close/Read/
// Write/Lseek/Stat/Readdir/Mkdir/Unlink)는 이미 5일 전에 완료돼
// 있었다(`PN-EA4EE935` 등). 유저 프로세스는 실제로 `/sys/mnt`의
// `KernelDriver` 경로를 통해 파일을 열고 읽을 수 있다. 알려진 포맷을
// 못 찾으면(장치
// 없음/미지원 포맷) `/sys/mnt`는 그대로 기존 Channel 라우팅으로
// 남는다 - 나머지 세 지점(`/sys/etc`/`/sys/bin`/`/sys/tmp`)의 accept
// 왕복 검증(accept 즉시 close, pubreg 항목3과 동일 패턴)은 이번
// 증분에서 안 건드림.
//
// **[신규, 2026-09-20, QU-1FB6A7A4 답변 - "블록 디바이스는 그냥 아예
// fs한테 던져버려. 인식/인식 해제까지 전부."]** 블록 스토리지 장치
// (AHCI 등)의 PCI 열거/매칭/드라이버 구동을 devmgr이 아니라 이
// 프로세스가 직접 수행한다 - devmgr이 fork()로 스폰한 자식이 개설한
// 이름 없는 Channel을 이 프로세스가 나중에 찾아야 하는 문제
// (SP-C2670F69 §3.1이 남겨 둔 설계 공백) 자체가 이 결정으로 사라진다
// (같은 프로세스 안이므로 핸드오프가 필요 없음). AHCI 실제 하드웨어
// 코드는 ahci.h/ahci.cpp(PN-4E6EA13D/PN-F60E405A, devmgr.cpp에서 이관) -
// `AhciBlockDevice`(block_device.h `fs::BlockDevice` 구현).
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
#include "libext4/ext4_driver.h"
#include "libswapfs/swapfs.h"
#include "libvfat/vfat_driver.h"
#include "mount_table.h"
#include "pnp.h"
#include "scheduler.h"
#include "swap_backend.h"
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

// [구현, 2026-09-22, PN-452FF696 항목5] 실제 감지된 블록 장치에
// 연결할 후보 FileSystemDriver - §3.1 우선순위(ext4 → FAT32/16)로
// 차례로 mount()를 시도한다. exFAT/NTFS는 §4 통합 계층(ExfatDriver/
// NtfsDriver, PN-09970F05/PN-52C577F3)이 아직 없어 이번 자동 감지
// 순서에서 제외 - 그 계획들이 완료되면 이 목록에 추가한다.
ext4::Ext4Driver gExt4Driver;
vfat::Fat32Driver gFat32Driver;

// [구현, 2026-09-23, PN-4859FDE9 준비 작업, SP-D02C4A73 §2 명시 지시]
// libswapfs(SwapfsBackend)는 FileSystemDriver를 구현하지 않는 별도
// 인터페이스(SwapBackend)라 VFS 마운트 지점에는 붙지 않지만,
// "자신만의 블록 장치(파티션)에 마운트된다 - SP-7CC5693A §5 5단계
// (우선순위대로 슈퍼블록 판별)가 그대로 적용된다"고 이 문서가
// 명시적으로 확정해 뒀다(새 DC 불필요라고까지 적어 둠) - 그래서
// ext4/FAT32와 같은 우선순위 체인의 마지막 후보로 같은 물리 장치를
// 그대로 재사용해 탐지한다(v1은 장치가 하나뿐이므로 셋 중 정확히
// 하나만 실제로 일치할 수 있다). 마운트에 성공해도 VFS 경로에는
// 아무것도 연결하지 않는다 - `kActiveSwapBackend()`(아래)로 다른
// 파일(회수 스캔/페이지폴트 스왑인, 아직 미배선)이 나중에 꺼내
// 쓴다.
fs::SwapfsBackend gSwapBackend;
bool gHasSwapBackend = false;

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

        // [갱신, 2026-09-22, PN-FFFE892E] irqVector를 더 이상 버리지
        // 않는다 - AhciController::init()이 이 벡터로 fs 자신을
        // 구독시키고 GHC.IE/PxIE를 켜 인터럽트 기반 완료 대기로
        // 전환한다(0이면 기존 폴링 그대로).
        if (!gAhciController.init(mappedAddr, irqVector)) {
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

// [구현, 2026-09-22, PN-452FF696 항목5, SP-7CC5693A §6 4-5단계]
// 실제 감지된 블록 장치(gAhciBlockDevice)에 §3.1 우선순위(위 후보
// 선언부 문서 주석 참고)로 순서대로 mount()를 시도해 슈퍼블록을
// 판별하고, 성공한 첫 FileSystemDriver를 실제 마운트 지점에 연결한다.
// **"어떤 블록 장치를 어떤 마운트 지점에 붙일지"는 SP-7CC5693A §4
// 항목3이 "각 드라이버 착수 시점의 구현 세부"로 이미 열어 둔 결정이라
// 여기서 확정한다**: 4개 사전 등록 지점(§4-A) 중 `/sys/mnt`가 "범용
// 마운트 지점"이라는 UNIX 관례에 가장 부합해 채택했다. 판별에
// 성공하면 그 경로의 기존 Channel 등록을 해제하고 KernelDriver로
// 다시 마운트한다(§6 5단계 - "2단계에서 예약해 둔 마운트 지점 중
// 알맞은 곳에 연결") - 실패하면(장치 없음/알려진 포맷 아님)
// `/sys/mnt`는 그대로 기존 Channel 라우팅으로 남는다(유저랜드 fs
// 서비스가 나중에 그 경로를 실제로 서비스할 수 있는 여지를 남겨 둠).
void kTryAutoMountBlockDevice() {
    if (!gHasAhciBlockDevice) {
        return;
    }
    if (gExt4Driver.mount(&gAhciBlockDevice, /*readOnly=*/true)) {
        MountTable::unmount(kMountMnt, sizeof(kMountMnt) - 1);
        MountTable::mountKernel(kMountMnt, sizeof(kMountMnt) - 1, &gExt4Driver);
        return;
    }
    if (gFat32Driver.mount(&gAhciBlockDevice, /*readOnly=*/true)) {
        MountTable::unmount(kMountMnt, sizeof(kMountMnt) - 1);
        MountTable::mountKernel(kMountMnt, sizeof(kMountMnt) - 1, &gFat32Driver);
        return;
    }
    // [구현, 2026-09-23, SP-D02C4A73 §2] 위 두 후보와 같은 우선순위
    // 체인의 마지막 - VFS 마운트 지점에는 연결하지 않는다(스왑은 VFS
    // 개념이 없음, gSwapBackend 선언부 주석 참고).
    if (gSwapBackend.mount(&gAhciBlockDevice)) {
        gHasSwapBackend = true;
        return;
    }
}

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
        // [정정, 2026-09-22, PN-7030D201 실측 확인 - 시도했다가 되돌림]
        // 이전 주석("PreemptionGuard로 원자적 구간을 만든다")은 틀렸다
        // - `PreemptionGuard`는 코어별 카운터일 뿐 EFLAGS.IF를 건드리지
        // 않아 `preemptive=true`가 즉시 쏘는 self-IPI(kAsyncDrainVector)
        // 전달 자체를 막지 못한다. 이 함수를 부르는 지점(kFsKernelMain,
        // IF가 이미 켜진 일반 실행 흐름)에서는 그 self-IPI가 곧바로
        // 전달돼 `submitterTask`를 채우기 전에 `AcceptFromChannelHandler::
        // onExec`이 실행될 수 있다는 것 자체는 실측으로 확인된 사실이다
        // (`PN-FFFE892E`가 AHCI Subscribe 제출에서 먼저 재현, 자세한
        // 경위는 PN-7030D201 참고).
        //
        // **[중요] 그 race를 피하려고 AHCI Subscribe와 동일하게
        // `preemptive=false`로 바꿔 봤으나, 표준 GRUB SMP4+AHCI 반복
        // 실행에서 그 즉시 크래시율이 0%(15회 무결함, 기존 알려진
        // PN-1DFCB337 신호와도 무관)에서 3/15(20%)로 뛰었다 -
        // Invalid Opcode/Page Fault, rip/rbp가 `0x53f000ff53f000e2`류
        // 명백한 스택 손상 패턴으로 이전에 관측된 적 없는 새 크래시
        // 서명이었다. 근본 원인은 이번 세션에서 규명하지 못했다(추정:
        // `gExecQueues`로 들어가면 `AsyncTaskWaitGroup::waitAll()`의
        // 드레인 루프가 이 accept 작업보다 다른 코어/작업을 먼저
        // 처리하게 돼, 이 함수 특유의 무언가와 겹쳐 노출되는 기존
        // 잠복 결함으로 보이나 확정하지 못함) - **그래서 이 함수는
        // `preemptive=true`로 되돌렸다.** 즉 이 함수는 PN-7030D201이
        // 문서화한 race에 여전히 노출된 상태로 남아 있다 - 정직하게
        // 미해결로 유지한다(고치려는 시도가 이미 알려진 것보다 더 심각한
        // 회귀를 만들어, "고치지 않은 채로 알려진 race"가 "고쳤다고
        // 착각한 채 숨겨진 크래시"보다 낫다고 판단했다). 후속 세션이
        // 이어받을 때는 `gExecQueues`/`gPreemptiveQueues` 드레인 순서
        // 차이가 실제로 무엇을 깨뜨리는지부터 먼저 재현/추적할 것.
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

// [구현, 2026-09-23, PN-4859FDE9 준비 작업] fs.cpp 밖의 소비자(회수
// 스캔/페이지폴트 스왑인, 아직 미배선)가 감지된 스왑 파티션을 꺼내
// 쓰는 유일한 통로 - kTryAutoMountBlockDevice()가 아직 안 불렸거나
// (부팅 극초반) 감지된 블록 장치가 스왑 포맷이 아니면 nullptr.
fs::SwapBackend* kActiveSwapBackend() {
    return gHasSwapBackend ? &gSwapBackend : nullptr;
}

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
    kTryAutoMountBlockDevice();

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
