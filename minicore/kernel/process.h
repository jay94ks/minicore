#ifndef MINICORE_KERNEL_PROCESS_H
#define MINICORE_KERNEL_PROCESS_H

#include "address_space.h"
#include "debug_session.h"
#include "libkenv/shared_ptr.h"
#include "libkenv/types.h"
#include "mount_table.h"  // MountKind/KernelFsDriver/FileHandle - Process::fileDescriptors(SP-2AAD7C8D §9.2)용
#include "signal.h"
#include "syscall.h"

namespace elf {
class Image;  // 전방 선언(minicore/libs/libelf/elf.h) - Process::execImage 시그니처용
}

namespace kernel {

class UserThread;
class ResourceGroup;  // 포인터로만 참조(Process::group) - 전체 정의는 resource_group.h(SP-245D130B)
struct BridgePipe;  // 포인터로만 참조(Process::openBridges) - 전체 정의는 channel.h(PN-9CC66142)

// 프로세스 신원 - 이 프로세스가 신뢰할 수 있는 커널 서비스인지를
// syscall 레벨에서 판정하는 불변 속성(SP-EAB162FC §2.1). 생성
// 시점에 스폰한 코드(kmain.cpp의 부팅 매니폐스트 인식, 또는 devmgr의
// PnP 드라이버 로더 - §2.2)가 직접 채운다 - 이 필드를 바꾸는 API는
// 의도적으로 두지 않는다(재조정/위임 체인 금지, §1).
enum class ProcessRole : uint8_t {
    Normal = 0,        // 기본값 - fork()/exec() 등 일반 경로로 만들어진 모든 프로세스
    KernelService = 1, // devmgr/fs/net/tty 및 그 PnP 드라이버 자식 - 고정 스폰 경로로만 부여
};

// 프로세스 생성 시점에 고정되는 시작 플래그(SP-EAB162FC §6) -
// `ProcessRole`과 마찬가지로 생성 이후 바꾸는 API를 두지 않는다.
struct ProcessStartFlags {
    // true면 이 프로세스가 종료됐을 때 `respawn`으로 재생성을
    // 시도한다(§6.3) - **[개정, 설계자 지시, 2026-09-15] 더 이상
    // "즉시"가 아니다.** `essential`이 이 재생성을 어떻게 다룰지를
    // 완전히 가른다: `essential==true`면 재생성 자체를 시도하지
    // 않고 이 값과 무관하게 즉시 커널 패닉(§6.3), `essential==false`
    // 면 §6.4의 지연/백오프 일정(`kResurrectIntervalMinutes()`)에
    // 따라 `DelayedExecutionQueue::schedule()`로 재스폰을 예약한다
    // (패닉 없이 무한정 재시도).
    bool resurrect = false;

    // **[추가, 2026-09-15, 설계자 지시]** "커널 서비스가 죽으면
    // 커널이 정상 동작하지 않는다"는 기존 암묵적 전제를 이 플래그로
    // 명시했다(§6.3) - true(기본값, 기존 전제와 동일)면 이 프로세스가
    // 죽었을 때 `resurrect` 값과 무관하게 즉시 커널 패닉. false면
    // 패닉하지 않고 `resurrect==true`일 때만 §6.4의 지연/백오프
    // 일정으로 재스폰을 시도한다(무제한 재시도 - 더 이상 시도 횟수
    // 상한으로 패닉하지 않는다, 패닉 방아쇠가 "재시도 횟수 초과"에서
    // 이 플래그 하나로 완전히 이전됨).
    bool essential = true;

    // resurrect==true일 때만 유효 - 이 프로세스를 원래와 동일한
    // 방식으로 다시 스폰하는 함수(스폰 헤퍼 자신을 가리킨, §6.2 -
    // 모듈 버퍼/경로를 이미 들고 있는 고정 스폰 경로만 v1 대상).
    // 인자는 새로 만들 Process에 그대로 이어 담을
    // `consecutiveFailures`(§6.4 - 더 이상 "패닉까지 남은 횟수"가
    // 아니라 "백오프 계산용 연속 실패 횟수" - 스폰 헤퍼는 이 값을
    // 새 Process::consecutiveFailures에 대입하기만 하면 된다).
    void (*respawn)(uint32_t consecutiveFailures) = nullptr;
};

// §6.4 비필수(essential==false) 서비스 재스폰 지연/백오프 일정 -
// 실패 0~4회는 기본 간격(1분), 이후 5회마다 1분씩 늘어 최대 10분
// 상한에서 멈춘다(그 이후로는 영원히 10분 간격으로 계속 시도 -
// 패닉하지 않는다는 게 이 정책의 핵심).
constexpr uint32_t kResurrectBaseIntervalMinutes = 1;
constexpr uint32_t kResurrectBackoffStepMinutes = 1;
constexpr uint32_t kResurrectBackoffEveryNFailures = 5;
constexpr uint32_t kResurrectMaxIntervalMinutes = 10;

inline uint32_t kResurrectIntervalMinutes(uint32_t consecutiveFailures) {
    const uint32_t steps = consecutiveFailures / kResurrectBackoffEveryNFailures;
    const uint32_t interval = kResurrectBaseIntervalMinutes + steps * kResurrectBackoffStepMinutes;
    return interval < kResurrectMaxIntervalMinutes ? interval : kResurrectMaxIntervalMinutes;
}

// 유저 프로세스의 커널 쪽 표현(SP-8B6B8D25 §2-B/§5, 유저랜드 준비
// 마일스톤 - 계획 PN-16CA347D) - 이름 자체는 제안일 뿐 확정이 아니다
// (UserThread/Task와 동일 각주, SP-04EE2A18). "커널 Task와 (유저)
// 쓰레드는 다른 개념"(설계자 지시, 2026-09-14)이라는 구분을 그대로
// 이어받아, Process는 **주소공간(전용 PML4)의 소유자**이고
// `UserThread`(Task 상속)는 그 안에서 실제로 스케줄링되는 실행
// 흐름이다 - 지금은 프로세스당 스레드 하나만 지원(v1, 멀티스레드
// 프로세스는 후속 과제).
//
// **범위 안내**: 이 구조체는 "주소공간 소유 + 유저 모드 폴트 정보
// 보관" 부분만 다룬다(§2-B가 명시적으로 요구하는 부분, 지금 바로
// 구현/검증 가능) - ELF 로더/프로세스 생성-exec/ring3 진입은 유저
// 주소공간 레이아웃(128TiB, 코드 베이스 0x400000, 스택은 끝점부터,
// 코드/스택 양쪽 가드 페이지 - SP-8B6B8D25 §5/§5-A, QU-72108298/
// QU-7043EA6D 답변으로 확정 완료) 자체는 준비되었지만, **QU-FF3F0CAA
// ("첫 프로세스"의 정체/유저랜드 빌드 체계) 답변이 아직 없어** 실제로
// 실측 검증할 방법이 정해지기 전까지는 별도로 이어간다.
// [신규, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2] 커널이 발급하는
// 불투명 프로세스 핸들 - "pid는 사실 Process*를 reinterpret_cast한
// 값"이던 예전 관례(`SpawnProcessArgs::pid`/`WaitArgs::targetPid`/
// `reapedPid`)를 대체한다. `kChannelId`(channel.cpp)가 이미 확립한
// "세대 태그 슬롯 테이블" 패턴과 동일 - 유저가 넘긴 값을 절대
// `reinterpret_cast<Process*>`로 역참조하지 않고, 인덱스 범위 확인 +
// generation 일치 확인만으로 안전하게 해석한다. **이 인코딩은
// `SpawnProcess`로 만들어진 프로세스 트리에만 적용된다** - 고정 스폰
// KernelService(kmain.cpp가 SpawnProcess syscall 경로 없이 직접
// 만드는 devmgr/fs/net/tty 등)는 `kAllocateProcessId()`를 거치지
// 않으므로 `processId`가 항상 `kInvalidProcessId`로 남는다(SP-9CB55C5B
// §1 "이 트리는 SpawnProcess로 만들어진 프로세스의 부모-자식 관계만
// 표현한다"와 동일한 스코프). **[범위 안내]** 이 PN은 `SpawnProcess`/
// `Wait`의 pid 필드 인코딩 마이그레이션만 다룬다 - `Kill`의 권한
// 스코프 확장(`PN-88E62419`)은 별도 계획으로, 그쪽은 여전히 기존
// raw-pointer 비교(`reinterpret_cast<int64_t>(child.get())`)를 그대로
// 쓴다(설계 문서 자신이 명시한 의도적 과도기적 불일치).
using ProcessId = int64_t;
constexpr ProcessId kInvalidProcessId = -1;

// [확정, 2026-09-17, QU-78E4159E 답변] 시스템 전체 동시 생존 프로세스
// 개수 상한 - UINT16_MAX. 슬롯 인덱스가 이 값 미만이어야 하므로
// `kAllocateProcessId()`/`kResolveProcessId()`/`kFreeProcessId()`
// (process.cpp, 테이블 자체는 파일 스코프 static)가 공유하는 상수.
constexpr uint32_t kMaxProcessTableSlots = 65535;

// [신규, 2026-09-17, PN-E2A114C1, DC-21647E46/QU-76409699 "(B) 포함으로
// 읽자"] `EnableSharedFromThis<Process>` 상속 - `Process`의 모든
// 인스턴스가 이제 `kMakeShared<Process>(...)`로 감싸져 관리되므로(아래
// `parent`/`children` 필드 및 `UserThread::process`가 그 컨트롤 블록을
// 공유하려면), `Process` 자신의 멤버 함수(`execImage()`)가 `this`를
// 가리키는 `WeakPtr<Process>`를 스스로 재구성할 방법(`weakFromThis()`)
// 이 필요하다 - `Process::allocate()`가 내주는 raw 포인터만으로는
// 그 컨트롤 블록에 접근할 방법이 없기 때문(shared_ptr.h 참고).
class Process : public EnableSharedFromThis<Process> {
public:
    // 유저 모드 페이지 폴트 정보(§2-B "그 폴트 정보는 그 프로세스
    // 자신의 자료구조(PCB)에 매달아 둔다") - 폴트가 나면 커널을 멈추지
    // 않고 이 프로세스만 멈춘 뒤 여기 기록해 둔다.
    //
    // **깨우기 정책 확정(QU-0C4D25EF 답변, 2026-09-14)**: 이 폴트
    // 정보를 읽어 "프로세스를 죽일지, swapfs에서 읽어와 페이지를
    // 갈아 끼울지"를 판단하는 비동기 작업(AsyncTaskHandler)이 나중에
    // 처리한다 - `Task::state`는 새 상태 없이 **기존 `Blocked`를 그냥
    // 재사용**한다(그 처리가 끝나기 전까지 이 프로세스가 실행되지만
    // 않으면 되고, "왜 Blocked인지"는 이 `pending` 플래그 자체가
    // 이미 말해 준다). 그 비동기 작업/swapfs 자체는 아직 이
    // 프로젝트에 없다 - 그 인프라가 생길 때 실제로 연결한다.
    struct FaultInfo {
        bool pending = false;
        uint64_t faultAddr = 0;    // CR2
        uint64_t errorCode = 0;    // 하드웨어가 스택에 남긴 에러 코드
        uint64_t rip = 0;          // 폴트를 일으키 명령어
    };

    uint64_t pml4Phys = 0;        // 이 프로세스 전용 주소공간의 PML4 물리 프레임(Paging::createAddressSpace)
    UserThread* mainThread = nullptr;  // v1: 프로세스당 스레드 하나(위 클래스 주석 참고)
    FaultInfo lastFault;

    // [확정, 2026-09-16, QU-52253384 답변] 프로세스 트리(SP-6BEAE0C1
    // §6) - 이 프로세스를 만든 부모(SpawnProcess 호출자). 최초
    // 프로세스(init, kSpawnInitProcess)는 부모가 없는 루트라 비어
    // 있는 채로 남는다 - kSpawnServiceProcesses가 만드는 고정 스폰
    // KernelService들도 SpawnProcess syscall 경로를 타지 않으므로
    // 마찬가지로 비어 있음(이 트리는 SpawnProcess로 만들어진 프로세스의
    // 부모-자식 관계만 표현한다 - 고정 스폰 서비스들의 생명주기는
    // 이미 별도의 resurrect/essential 메커니즘(§6.3/§6.4)이 관리).
    //
    // [수정, 2026-09-17, PN-E2A114C1] `Process*`에서 `WeakPtr<Process>`
    // 로 전환 - 자식이 부모의 생사에 영향을 주면 안 된다(부모가 먼저
    // 죽는 고아 시나리오가 이미 §6에 구현돼 있어 이 방향과 자연히
    // 맞는다). 부모의 진짜 소유자는 `children`(아래, 조부모 또는
    // `gInitProcess`/`gServiceProcess[]`)이지 자식이 아니다 - 자식이
    // 강한 참조까지 쥐면 부모<->자식 순환 참조가 생겨 서로 절대
    // 해제되지 않는다.
    WeakPtr<Process> parent;

    // 이 프로세스가 SpawnProcess로 만든 자식들의 목록 - `wait()`가
    // 좀비(§6)를 찾을 때, 프로세스 종료 시 고아를 init에게 입양시킬
    // 때 쓴다. 청크 용량 8은 프로세스당 자식 수가 보통 많지 않을
    // 거라는 가정의 순수 구현 세부(실측 후 조정 가능, RM-23F4B687
    // §4) - `pendingSignals`와 달리 하드웨어 버킷 크기에 맞출 이유가
    // 없어 그냥 작게 시작한다.
    //
    // [수정, 2026-09-17, PN-E2A114C1] `ChunkedList<Process*, 8>`에서
    // `ChunkedList<SharedPtr<Process>, 8>`로 전환 - **부모가 자식의
    // 진짜(유일한) 강한 소유자다.** 이 결정은 SP-6BEAE0C1 §6/QU-76409699
    // 가 이미 확정해 둔 기존 동작(부모가 `children`에 자식을 넣어 두고
    // `wait()`로 회수하기 전까지 아무도 그 Process 구조체를 반납하지
    // 않는다)을 SharedPtr 어휘로 그대로 옮긴 것뿐이다(순수 매핑, 새
    // 정책 아님) - "누가 강한 참조를 쥐는가"라는 질문 자체가 §6이
    // 이미 답해 뒀다. 위 `parent`가 WeakPtr인 것과 대칭(소유는 항상
    // 위→아래로만 흐른다 - 부모가 자식을 소유, 자식은 부모를 관찰만).
    // 고아 입양(reparent)은 이 SharedPtr을 옛 부모의 `children`에서
    // 복사해 `orphanRoot()`의 `children`에 넣는 것으로 자연히
    // 처리된다(옛 슬롯은 곧 `clear()`로 비워짐 - ChunkedList::clear()/
    // erase()가 이제 슬롯 값을 실제로 반납한다는 전제, chunked_list.h
    // 참고).
    static constexpr uint32_t kMaxChildrenChunkCapacity = 8;
    ChunkedList<SharedPtr<Process>, kMaxChildrenChunkCapacity> children;

    // [신규, 2026-09-17, PN-9CC66142, DC-21647E46 로드맵 Phase 4,
    // 설계자 답변("BridgePipe가 그걸 이용하는 이용자 객체 양쪽에
    // 매달려야 하는게 맞는거야")] 이 프로세스가 열어 둔 Channel IPC
    // 반쪽(BridgePipe)들의 진짜(유일한) 강한 소유자 - `children`과
    // 완전히 같은 패턴(청크 기반 SharedPtr 컨테이너). `AcceptFrom
    // ChannelHandler::onExec()`(channel.cpp)이 accept 완료 시 양쪽
    // Process(제출자는 `AsyncTask::submitterTask.lock()` 체이닝으로
    // 얻음, PN-DB5153B6)의 이 목록에 각자의 `SharedPtr<BridgePipe>`를
    // 하나씩 넣는다 - `BridgeHandle`(유저에게 돌려주는 raw 포인터
    // 값)은 이후 read/write/close syscall이 "호출자 자신의 이
    // 목록에서" 실제로 찾아야만 유효하다(임의의 64비트 값을 그냥
    // reinterpret_cast하던 기존 방식의 보안 공백을 이 검증이 막는다).
    // `BridgePipe::peer`는 이제 `WeakPtr<BridgePipe>`라 상대 쪽이
    // 자기 프로세스의 이 목록에서 빠지면(닫힘/프로세스 종료)
    // `peer.lock()`이 자연히 빈 값을 반환한다 - 예전 `closedLocal`
    // 두 플래그 프로토콜이 하던 "양쪽 다 닫혀야 반납"을 참조 카운팅이
    // 대신한다.
    static constexpr uint32_t kMaxOpenBridgesChunkCapacity = 8;
    ChunkedList<SharedPtr<BridgePipe>, kMaxOpenBridgesChunkCapacity> openBridges;

    // [신규, SP-2AAD7C8D §9.2, PN-EA4EE935] 표준 파일 API의 프로세스별
    // fd 테이블. §9.2 원안은 `ownerChannelId`/`fsHandle`/`offset`/
    // `isDirectory`만 뒀지만, `PN-ABD23ACE` 항목2가 지적한 대로 그것만으론
    // 이 fd가 Channel 소비자인지 커널 드라이버 소비자인지 구분할 수 없어
    // `kind`(`MountKind` 재사용, mount_table.h §2.1)를 추가로 싣는다.
    // `fd` 자신을 값 안에 함께 저장해 두는 이유는 `BridgeHandle`과 같은
    // 이유 - openBridges처럼 포인터 동일성이 아니라 정수 하나로
    // 찾아야 하므로, `find([fd](...){ return e.fd == fd; })`가 "호출자
    // 자신의 이 목록에 실제로 존재하는 fd인지"를 검증하는 유일한
    // 진입점이 된다(임의의 정수를 그냥 믿지 않음 - BridgeHandle 검증과
    // 동일한 보안 원칙, PN-CE6A04AB 참고).
    struct FileDescriptor {
        int32_t fd = -1;
        MountKind kind = MountKind::Channel;
        uint64_t ownerChannelId = 0;        // kind==Channel일 때만 유효 - [미구현] Channel 경로는 아직 없음(PN-EA4EE935 스코프 결정)
        KernelFsDriver* kernelDriver = nullptr;  // kind==KernelDriver일 때만 유효
        FileHandle fsHandle;
        uint64_t offset = 0;
        bool isDirectory = false;
        bool used = false;
    };
    static constexpr uint32_t kMaxFileDescriptorsChunkCapacity = 8;
    ChunkedList<FileDescriptor, kMaxFileDescriptorsChunkCapacity> fileDescriptors;

    // [신규, 2026-09-16, SP-6BEAE0C1 §6, PN-543C0CE9 착수 5번째 증분(2/2)]
    // 좀비 상태 - self-terminate 시(SelfTerminateHandler::onExec)
    // `destroy()`로 주소공간은 즉시 반납하지만, `parent != nullptr`이면
    // 이 Process 구조체 자신(및 `mainThread`)은 그 자리에서 바로 반납하지
    // 않고 좀비로 남겨 부모가 `wait()`(RM-48E1E610 35번)로 회수(reap)할
    // 때까지 보존한다 - POSIX 좀비 프로세스와 동일한 개념. `parent ==
    // nullptr`(고정 스폰 KernelService, 또는 SpawnProcess의 caller가
    // 이론상 없었던 경우)이면 이 필드는 아예 세팅되지 않는다 - 회수할
    // 부모 자체가 없기 때문(§6.3/§6.4의 기존 resurrect/essential
    // 메커니즘은 이 좀비 개념과 무관하게 그대로 동작).
    bool isZombie = false;
    int32_t exitCode = 0;

    // [신규, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2] 이 프로세스에
    // 커널이 발급한 불투명 핸들(kInvalidProcessId로 시작 - `SpawnProcess`
    // 성공 경로의 `kAllocateProcessId()`만 채운다, 위 `ProcessId`
    // 문서 주석 참고). `processTableIndex`는 `kFreeProcessId()`가
    // O(1)로 슬롯을 찾기 위한 역참조(SP-9CB55C5B §2 "Process 쪽에
    // 자기 슬롯 인덱스를 역으로 저장해 둬야 해제 시 O(1)로 슬롯을
    // 찾을 수 있다"). 둘 다 init()에서 매번 리셋(Resurrect가 같은
    // 정적 Process를 재사용할 수 있으므로 - 다만 고정 스폰
    // KernelService는 애초에 이 필드들을 채운 적이 없어 항상
    // kInvalidProcessId/kInvalidProcessTableIndex로 남는다).
    ProcessId processId = kInvalidProcessId;
    static constexpr uint32_t kInvalidProcessTableIndex = 0xFFFFFFFFu;
    uint32_t processTableIndex = kInvalidProcessTableIndex;

    // [신규, 2026-09-17, SP-245D130B §1/§4] 이 프로세스가 속한 자원
    // 그룹(cgroup류) - `parent`/`children`(프로세스 트리)과 완전히
    // 별개의 축이다. 소유 관계가 아니라 순수 관찰용 raw 포인터다
    // (`ResourceGroup`은 지금 전부 정적 전역이라 이 포인터의 수명을
    // 넘어설 걱정이 없다 - `gRootResourceGroup` 하나뿐, 동적 그룹
    // 생성이 생기면 그때 소유권 모델을 재검토). `joinResourceGroup()`
    // 이 아니면 직접 대입하지 않는다. init()에서 명시적으로 nullptr로
    // 리셋(Resurrect §6.2가 같은 정적 Process를 재사용할 수 있으므로
    // 이전 생애의 그룹 소속이 새 생애로 새어 들어가면 안 된다).
    ResourceGroup* group = nullptr;

    // [신규, 2026-09-17, SP-245D130B §6/SP-B26CDBDD §6.2, PN-158B6B2F]
    // 이 프로세스가 쓴 메모리의 coarse(정확한 페이지 단위 실시간 추적
    // 아님) 계정 - `execImage()`가 로드된 이미지(PT_LOAD 세그먼트
    // memsz 합) + 유저 스택 크기만큼 가산, `destroy()`가 전액 감산.
    // 향후 mmap 서브시스템(SP-2AAD7C8D)이 실제로 페이지를 매핑/해제할
    // 때의 가산/감산 배선은 이 문서 범위 밖(그 서브시스템을 직접
    // 다루는 후속 계획이 결정). Resurrect(§6.2)가 같은 정적 Process를
    // 재사용할 수 있으므로 init()에서도 명시적으로 0으로 리셋한다
    // (group/frozenByGroup과 동일한 이유).
    uint64_t memoryBytesUsed = 0;

    // [신규, 2026-09-17, SP-245D130B §4] `group->freeze()`가 이
    // 프로세스를 실제로 멈췄는지 - `ResourceGroup::thaw()`가 이 값이
    // true인 프로세스만 다시 깨운다(freeze() 호출 이후 새로 스폰돼
    // 애초에 멈춘 적 없는 프로세스와 구분하기 위함). init()에서 매번
    // 리셋(위 group과 동일한 이유).
    bool frozenByGroup = false;

    // 신원/시작 플래그(SP-EAB162FC) - 둘 다 스폰 시점에 호출부가 직접
    // 채우고, 그 이후 바꾸는 setter는 두지 않는다(§1/§6 원칙).
    ProcessRole role = ProcessRole::Normal;
    ProcessStartFlags startFlags;

    // 이 프로세스를 스폰할 때 커널이 알고 있던 이름(예: "devmgr") -
    // `role`과 같은 이유로 스폰 시점에 호출부가 직접 채우고 이후
    // 바꾸는 setter는 없다. **왜 필요한가(SP-00CA7175 §2.0/RM-C65F7760,
    // PN-71C2B857 조사 중 발견)**: `LiveFs::open("kernel/<name>")`이
    // 호출자가 정말 그 이름으로 스폰된 `ProcessRole::KernelService`
    // 프로세스 자신인지 검사해야 하는데, 이 필드가 생기기 전까지는
    // 그 비교 대상 자체가 Process에 없었다 - `KernelReservedEntry::
    // name`(livefs.h)과 같은 관례(char 배열 + 길이)로 맞춘다.
    static constexpr uint32_t kMaxSpawnNameLen = 32;
    char spawnName[kMaxSpawnNameLen] = {};
    uint32_t spawnNameLen = 0;

    // §6.4 - `role`/`startFlags`와 달리 살아있는 동안 불변인 값이
    // **아니다**. 재스폰마다 이어지는 런타임 카운터라 별도 필드로
    // 둔다 - 재스폰 트리거 지점(SelfTerminateHandler::onExec)이 옛
    // Process의 이 값을 읽어 +1 한 값을 새 Process에 명시적으로
    // 이어줘야만 "연속 실패 횟수"가 유지된다(그러지 않으면 새
    // Process가 항상 0으로 시작해 백오프 간격이 매번 기본값으로
    // 되돌아간다). **[개명, 2026-09-16, SP-EAB162FC §6.4 개정]**
    // 옛 이름 `resurrectCount`에서 개명 - 더 이상 "패닉까지 남은
    // 횟수"가 아니라 순수하게 `kResurrectIntervalMinutes()` 백오프
    // 계산에만 쓰인다(패닉 방아쇠는 `ProcessStartFlags::essential`
    // 하나로 완전히 이전됨).
    uint32_t consecutiveFailures = 0;

    // 이 프로세스가 소유한 VMA(코드/데이터/스택 - execImage()가 채움)의
    // 장부(PN-71C3D483 항목 3, SP-2AAD7C8D §2/§4) - destroy()가 이걸로
    // 실제 페이지를 찾아 반납한다. init()이 pml4Phys 확보 직후 초기화.
    ProcessAddressSpaceManager addressSpace;

    // [신규, 2026-09-18, PN-22E5E9E7 항목5, SP-29D652AA §5.1/§5.2] 이
    // 프로세스의 ELF `PT_TLS` 세그먼트(있으면) 템플릿 - `execImage()`가
    // `elf::kSegmentTypeLoad` memsz 합산과 같은 세그먼트 스캔에서 함께
    // 채운다. **이번 증분(항목5)은 순수 파싱/저장까지만** - 실제
    // UserThread별 TLS 인스턴스 생성(항목6)/FS_BASE 스왑(항목7)은 아직
    // 이 필드를 소비하지 않는다. `hasTlsTemplate`가 false면 나머지
    // 세 필드는 의미 없음(PT_TLS 세그먼트 자체가 없는 바이너리 - v1
    // 유저 바이너리는 아직 thread_local을 쓰지 않아 항상 이 경우).
    // Resurrect(§6.2)가 같은 정적 Process를 재사용할 수 있으므로
    // group/frozenByGroup과 동일한 이유로 init()에서 매번 리셋한다.
    uint64_t tlsTemplateVaddr = 0;
    uint64_t tlsTemplateFilesz = 0;
    uint64_t tlsTemplateMemsz = 0;
    uint64_t tlsTemplateAlign = 0;
    bool hasTlsTemplate = false;

    // Signal 전달 인프라(SP-0666DB3C §4.3, PN-71E50394) - 아직 대기
    // 중(전달 시도 전)인 신호들의 목록 + 각 신호 번호별 처리 방식.
    // 둘 다 init()에서 명시적으로 리셋한다 - Resurrect(§6.2)가 같은
    // 정적 Process 인스턴스를 재사용하므로, 이전 생애의 신호 상태가
    // 새 생애로 새어 들어가면 안 된다.
    ChunkedList<PendingSignal, kPendingSignalChunkCapacity> pendingSignals;
    SignalDisposition dispositions[kSignalCount];

    // 프로세스 디버깅(SP-9A6D579F §3.1, PN-87D6B615) - 이 프로세스가
    // "디버기"일 때만 의미가 있다(`active==true`) - 디버기 자신이
    // 소유하는 세션이라 debug_session.h의 문서 주석 참고대로 원
    // 설계의 전역 고정 배열 대신 이 필드로 직접 대체했다. init()에서
    // 명시적으로 리셋(Resurrect §6.2 재사용 대비, pendingSignals와
    // 동일한 이유).
    DebugSession debugSession;

    // Brk(RM-48E1E610 19번, SP-2AAD7C8D §5, PN-012E8C1A) - init()이
    // 최소 크기(kMinHeapLength)의 힙 VMA를 즉시 만들어 둘 값들 -
    // brk(newBrk)가 POSIX처럼 newBrk를 항상 절대 주소로 다루려면
    // 첫 호출 이전에도 유효한 "현재 브레이크"가 있어야 하기 때문
    // (mapRegion()이 특정 주소를 강제 지정할 방법이 없어 findGap이
    // 고른 결과를 그대로 받아들인다). init()에서 매번 새로 만든다
    // (Resurrect 재사용, 위 pendingSignals와 동일한 이유).
    // **불변조건(실측으로 발견)**: `heapBrk - heapStart`는 항상 힙
    // VMA의 실제 등록된(addressSpace의 Maple Tree에 store된) 길이와
    // 정확히 같아야 한다 - `resizeAnonymousRegion()`이 그 값을 그대로
    // `oldLength`로 넘겨 기존 범위를 찾는 데 쓰기 때문에, "논리적
    // 브레이크가 실제 매핑보다 작을 수 있다"는 여유를 두면 그 즉시
    // 범위 불일치로 실패한다. init() 직후에도 heapBrk는 반드시
    // heapStart + kMinHeapLength(0이 아님)여야 한다.
    uint64_t heapStart = 0;
    uint64_t heapBrk = 0;

    // [SP-6BEAE0C1 §5] 동적 Process 풀 - GenericSlabAllocator에서 이
    // 구조체 하나 크기의 raw 메모리를 얻어 0으로 채운 뒤 Process*로
    // 돌려준다(placement new 없이, 이 클래스의 모든 필드가 0/nullptr
    // NSDMI라 memset 결과가 실제 생성자 결과와 동일함을 이용) - 실패
    // (슬랩 고갈) 시 nullptr. **반환값은 아직 init()을 부르지 않은
    // "빈 자리"** - 정적 전역 Process를 선언만 해 두고 별도로 init()을
    // 부르는 기존 관례(kmain.cpp의 gInitProcess 등)와 동일하게, 호출부가
    // 이어서 init()을 불러야 한다.
    static Process* allocate();

    // allocate()가 내준 슬랩 메모리를 반납한다 - 호출부가 먼저 destroy()
    //로 이 프로세스가 소유한 자원(주소공간/VMA)을 전부 반납했다는
    // 전제(순서를 안 지키면 자원 누수 - destroy()가 안전을 강제하지
    // 않는다, Vma의 free() 관례와 동일).
    static void release(Process* proc);

    // pml4Phys를 새로 확보하고 커널 상위 절반(higher-half)을 공유하는
    // 상태로 초기화한다(Paging::createAddressSpace 참고 - 하위 절반은
    // 전부 비어 있는 채로 시작, ELF 로더가 채울 자리). 실패(Slab/페이지
    // 고갈) 시 false - 블로킹하지 않는다.
    bool init();

    // pml4Phys를 반납한다 - **이 함수 자신이 먼저 `addressSpace.
    // unmapAll()`로 이 프로세스가 실제로 매핑한 하위 절반의 모든
    // VMA(코드/데이터/스택)를 Paging::unmapPage로 해제한 뒤에 PML4
    // 프레임을 반납한다**(PN-71C3D483 항목 3 완료 - 예전엔 이 순서를
    // 호출부가 직접 챙겨야 했으나, VMA 추적 자료구조가 생겨 이제
    // 이 함수 하나로 완결된다). **알려진 한계**: `Paging::
    // destroyAddressSpace`는 PML4 프레임 자체만 반납하고, 그 하위에
    // 매달린 PDPT/PD/PT 중간 테이블 프레임은 건드리지 않는다(leaf
    // 데이터 페이지만 이 함수가 회수 - 중간 테이블 프레임 회수는 이
    // 항목의 스코프 밖, 별도로 추적한다.
    void destroy();

    // ELF 이미지를 이 프로세스 주소공간에 로드하고, thread(호출부가
    // 마련해 둔, 아직 init() 전인 UserThread)를 그 진입점으로 ring3
    // 진입하도록 준비시킨다(PN-16CA347D 6번/PN-124C105B) - 성공하면
    // thread 자신을 반환한다(호출부가 Scheduler::enqueue해야 실제로
    // 실행 시작), 실패(ELF 로드/유저 스택 확보 실패) 시 nullptr.
    //
    // **v1 한계**:
    // - 유저 스택은 고정 크기(64KiB)/고정 주소, 가드 페이지 없음
    //   (커널 스택 가드 페이지와 같은 패턴으로 후속 추가 예정, 새 DC
    //   불필요 수준).
    // - syscall MSR 경로(STAR/LSTAR/SFMASK)가 아직 없어 유저 코드는
    //   반드시 `int 0x80`으로만 트랩해야 한다(PN-124C105B 남은 항목).
    // - 프로세스당 스레드 하나 전제(위 클래스 주석과 동일) - thread는
    //   호출부가 소유(동적 할당/해제는 이번 범위 밖, PN-40E976F2).
    // [확장, PN-E35294B8 항목2, QU-B9EB45E4 답변] argvEnvpScratch가
    // 있으면(SpawnProcessHandler가 이미 유저 argv/envp를 검증+복사해
    // 넘긴 것) 그 문자열 데이터까지 실은 완전한 SysV 초기 스택 프레임을
    // 짓는다 - 기본값(nullptr/0)이면 기존 그대로(빈 argc=0/argv=[NULL]/
    // envp=[NULL]) 동작해 kEnterInitProcess/kSpawnServiceProcesses
    // 호출부는 전혀 안 바뀐다.
    UserThread* execImage(const elf::Image& image, UserThread* thread, const uint8_t* argvEnvpScratch = nullptr,
                          uint64_t stringsSize = 0, const uint64_t* argOffsets = nullptr, uint32_t argCount = 0,
                          const uint64_t* envOffsets = nullptr, uint32_t envCount = 0);

    // [신규, 2026-09-18, PN-22E5E9E7 항목6, SP-29D652AA §5.2] 이
    // 프로세스의 PT_TLS 템플릿(항목5, `hasTlsTemplate`)이 있으면 그
    // 프로세스 주소공간 안에 `thread` 전용 TLS 인스턴스를 만들어
    // `thread->userFsBase`를 채운다("새 UserThread를 만들 때마다"의
    // 범용 루틴 - v1의 유일한 호출부는 execImage() 안이지만, 프로세스가
    // 아니라 UserThread 생성에 결부돼 있어 나중에 멀티스레딩 syscall
    // (PN-543C0CE9류)이 새 UserThread를 여러 개 만들어도 코드 변경
    // 없이 재사용한다). 템플릿이 없으면(v1 유저 바이너리 전부 해당)
    // 즉시 true, `userFsBase`는 0으로 남는다. 실패(할당/매핑 고갈)
    // 시 false - 호출부(execImage())가 OOM으로 취급.
    bool makeUserTlsInstance(UserThread* thread);

    // Signal 전달(SP-0666DB3C §4.4, PN-71E50394 항목 2) - number를
    // pendingSignals에 기록하고, mainThread가 지금 대기 중이면
    // (blockedOn != nullptr) 그 자리에서 즉시 강제로 깨운다(§9.5,
    // Waitable::cancel 경유). mainThread가 실행 중/비대기 상태면 여기서는
    // 아무 것도 더 하지 않는다 - 체크포인트 방식(실행 중인 코드가 다음
    // syscall 진입/ring3 재진입 시점에 pendingSignals를 확인하는 것)은
    // 아직 어디에도 배선돼 있지 않다(별도 후속 항목). 실패(자원 고갈)
    // 시 false.
    bool raiseSignal(SignalNumber number);

    // [신규, 2026-09-17, SP-245D130B §1] 이 프로세스를 `newGroup`으로
    // 옮긴다(옛 그룹에서 빼고 새 그룹에 넣음, `newGroup==nullptr`이면
    // 그냥 빼기만) - `ResourceGroup::addMember()`가 `WeakPtr<Process>`를
    // 받는데 `EnableSharedFromThis<Process>::weakFromThis()`가
    // `protected`라 외부 클래스가 못 부르므로, `Process` 자신의 이
    // 메서드가 대신 호출해 넘긴다(resource_group.h의 addMember 문서
    // 주석과 동일한 이유).
    void joinResourceGroup(ResourceGroup* newGroup);

    // [SP-6BEAE0C1, PN-543C0CE9 착수 4번째/5번째 증분] SpawnProcess(59번)/
    // Wait(35번) syscall 엔드포인트를 SyscallRegistry에 등록한다 -
    // Channel::registerSyscallEndpoints()와 같은 관례(부팅 시 BSP에서
    // 한 번, kmain.cpp가 호출).
    static void registerSyscallEndpoints();

    // [신규, 2026-09-16, SP-6BEAE0C1 §6, PN-543C0CE9 착수 5번째 증분(2/2)]
    // 고아 입양 대상(init 프로세스)을 가리키는 전역 - kmain.cpp가
    // `kSpawnInitProcess()`에서 `gInitProcess`를 `kMakeShared`로 감싼
    // 직후 딱 한 번 등록한다. self-terminate 시 이 프로세스에게 살아있는
    // 자식이 있었다면(자신도 부모였던 경우) 그 자식들을 전부 이
    // 루트로 reparent한다(§6 "고아는 init이 입양"). init 자체가 아직
    // 스폰되지 않았거나(gInitImageFound==false) 실패했다면 빈 상태로
    // 남아 있을 수 있다 - 그 경우 orphan reparent 단계는 방어적으로
    // 그냥 건너뛴다(고아가 root 없는 상태로 남는 건 이번 증분 스코프
    // 밖의 부팅 실패 시나리오 - 커널이 정상 부팅했다면 항상 세팅돼
    // 있다).
    //
    // [수정, 2026-09-17, PN-E2A114C1] `Process*`에서 `WeakPtr<Process>`
    // 로 전환 - 이 정적 전역이 `gInitProcess`(이제 `SharedPtr<Process>`)
    // 를 향한 또 다른 강한 참조가 될 이유가 없다(`gInitProcess` 자신이
    // 이미 영구 소유자). 호출부는 `orphanRoot().lock()`으로 사용한다.
    static void setOrphanRoot(const SharedPtr<Process>& root) { gOrphanRoot = WeakPtr<Process>(root); }
    static WeakPtr<Process> orphanRoot() { return gOrphanRoot; }

private:
    static WeakPtr<Process> gOrphanRoot;
};

// [신규, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2] 위 `ProcessId` 세대
// 태그 슬롯 테이블의 발급/해석/해제 3종(`kAllocateProcessId`/
// `kResolveProcessId`/`kFreeProcessId`) - 테이블 자체를 포함해 전부
// process.cpp의 익명 네임스페이스 안에 있다(channel.cpp의
// `kAllocateChannelId`/`kResolveChannelId`/`kFreeChannelId`와 동일한
// 관례 - `SpawnProcessHandler`/`WaitHandler`/`KillHandler` 전부 같은
// process.cpp 파일 안에 있어 헤더에 노출할 이유가 없다).

// [갱신, 2026-09-17, SP-E9B44929] Process 그룹(0) - SpawnProcess.
constexpr SyscallEndpointId kSyscallEndpointSpawnProcess = kMakeSyscallEndpointId(0, 4);

// SpawnProcess 실패 사유(SP-6BEAE0C1 §3) - `ChannelError`와 같은 관례
// (out 파라미터로 결과 코드 하나 + 부가 값).
enum class SpawnProcessError : uint32_t {
    None = 0,
    InvalidImageRange,   // imageBuffer/imageSize가 Paging::isUserRangeValid를 통과 못함
    ImageTooLarge,       // kMaxSpawnImageSize 초과
    OutOfMemory,         // 커널 버퍼/Process·UserThread slab/페이지 고갈
    ElfParseFailed,      // elf::Image::parse 실패(BadMagic 등)
    ExecImageFailed,     // Process::execImage 실패(유저 스택 확보 등)
    InvalidArgument,     // flags에 정의되지 않은 비트가 세팅됨(PN-A6E01B8A, QU-9585F6C4)
    // [신규, PN-E35294B8 항목2, QU-B9EB45E4 답변("상한을 두되, huge
    // page 1개만큼으로 제한해. 이 상한을 넘어서면 거부하고, 할당을
    // 시도했는데 실패해도 거부")] argv/envp 문자열 총합이
    // kMaxSpawnArgsTotalSize를 넘거나, 항목 수가 kMaxSpawnArgsEntryCount
    // 를 넘거나, 실제 대상 스택(kUserStackSize)에 다 안 들어갈 때.
    // 스크래치 버퍼 확보 실패는 (답변이 명시한 대로) 기존
    // OutOfMemory로 함께 처리한다 - 별도 코드 불필요.
    ArgsTooLarge,
};

// [신규, 2026-09-16, QU-9585F6C4 답변("일반화된 정수로 플래그 셋을
// 받는 형태로 설계해놔. (확장성)"), PN-A6E01B8A] SpawnProcess의
// `flags` 비트마스크 - 단일 `debugStart: bool` 대신 확장 가능한
// 비트마스크로 설계해, 앞으로 새 on/off 옵션이 필요해질 때마다
// syscall 시그니처 자체를 바꾸지 않고 새 비트만 추가하면 되게 한다.
enum SpawnProcessFlags : uint32_t {
    kSpawnNone = 0,
    // SP-9A6D579F §3.3 - 디버깅 대상으로 스폰된 자식은 첫 명령어 실행
    // 전 정지 상태로 시작(TaskState::Blocked) - 디버거(부모)가 이
    // 플래그를 세팅해 호출. **[완료, PN-87D6B615 항목8]** 실제 동작은
    // process.cpp SpawnProcessHandler::onExec 5단계 끝에서 소비한다 -
    // Ready 큐에 넣는 대신 새 Task를 state=Blocked로 남긴다. 이 자체로
    // DebugSession을 만들지는 않는다 - 디버거는 별도로 DebugAttach를
    // 불러야 실제로 그 세션을 쥔다(§3.3 "Attach/Detach 동작은 유지").
    kSpawnDebugStart = 1u << 0,
    // 이후 필요해지는 옵션은 여기 비트를 계속 추가(예: 1u << 1, ...).
};

// 현재 정의된 비트 전부의 OR - `flags`에 이 마스크 밖의 비트가 하나라도
// 세팅되면 InvalidArgument(§3 "조용히 무시하지 않음, 표준 커널 syscall
// 관례"). 새 비트를 추가할 때마다 이 마스크도 같이 넓혀야 한다.
constexpr uint32_t kSpawnProcessFlagsMask = SpawnProcessFlags::kSpawnDebugStart;

// [SP-6BEAE0C1 §3] SpawnProcess syscall 인자 - `imageBuffer`/`argv`/
// `envp`는 전부 유저 포인터(untrusted, 핸들러 내부에서 Paging::
// isUserRangeValid로 검증 후에만 역참조). **[완료, PN-E35294B8 항목2,
// QU-B9EB45E4 답변]** `argv`/`envp`는 이제 실제로 새 프로세스에
// 전달된다 - SpawnProcessHandler::onExec이 각 배열을 NULL 종단까지
// 걸으며 문자열을 검증+커널 스크래치 버퍼로 복사하고(총합
// kMaxSpawnArgsTotalSize 초과 시 거부), `Process::execImage()`가 그
// 결과로 SysV AMD64 초기 스택 프레임(argc/argv/envp/auxv 전부 실제
// 값)을 짓는다(§4). `flags`는 `kSpawnDebugStart` 비트까지 실제로
// 소비한다(PN-87D6B615 항목8) - 유효성 검증(kSpawnProcessFlagsMask)과
// 실제 동작 둘 다 구현 완료.
struct SpawnProcessArgs {
    const void* imageBuffer = nullptr;  // 유저 포인터 - ELF64 이미지 원본 바이트
    uint64_t imageSize = 0;
    char* const* argv = nullptr;  // 유저 포인터, NULL 종단 - PN-E35294B8 항목2에서 실제 소비
    char* const* envp = nullptr;  // 유저 포인터, NULL 종단 - PN-E35294B8 항목2에서 실제 소비
    uint32_t flags = SpawnProcessFlags::kSpawnNone;  // SpawnProcessFlags 비트마스크
    // out
    SpawnProcessError error = SpawnProcessError::None;
    // [수정, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2/§6] 성공 시 새
    // 프로세스의 `ProcessId`(세대 태그 슬롯 인코딩, `kAllocateProcessId()`)
    // - 예전엔 `reinterpret_cast<int64_t>(Process*)` 그 자체였으나(값
    // 형식 마이그레이션, 이미 승인된 ABI 변경 - QU-AB5247DD), 이제 유저는
    // 이 값으로 어떤 Process도 직접 역참조할 수 없다. 실패 시
    // `kInvalidProcessId`(-1) 유지.
    int64_t pid = kInvalidProcessId;
};

// [신규, 2026-09-18, PN-44C91D6E, SP-6BEAE0C1] `fork()` - 이 syscall만
// `int 0x80` 경로에서 유일하게 지원된다(`syscall` 명령 경로는 SYSRET용
// rcx/r11만 보존해 자식 재개에 필요한 나머지 GPR 스냅샷이 아예 없다 -
// idt.cpp의 `kHandleSyscallTrap`이 `kDispatchSyscallVerb` 공용
// 디스패치를 타기 전에 이 verb만 직접 가로채 전체 `InterruptFrame`을
// 그대로 넘긴다). `Syscall::submit()`의 submit-then-wait 모델과 근본적
// 으로 안 맞아(부모/자식 양쪽이 "즉시" 반환해야 함) 이 verb 하나로
// 완결된다 - args 구조체도, 별도 AsyncTaskHandler도 없다. `frame`을
// 그대로 받아 그 안에서 끝까지 처리하고, 부모의 반환값(`frame->rax`
// = 자식 ProcessId, 실패 시 -1)까지 여기서 직접 채운다 - 자식은
// `kResumeForkedRing3`(process.cpp)가 별도로 재개시킨다(그 함수
// 문서 주석 참고).
constexpr SyscallEndpointId kSyscallEndpointFork = kMakeSyscallEndpointId(0, 8);
void kHandleForkSyscall(InterruptFrame* frame);

// [신규, PN-E35294B8 항목2, QU-B9EB45E4 설계자 답변 그대로] argv+envp
// 문자열 데이터 총합의 v1 상한 - "huge page 1개만큼으로 제한해"를
// 그대로 반영(x86_64의 2MiB 대형 페이지 크기). 이 상한을 넘으면
// ArgsTooLarge로 거부한다 - isUserRangeValid만으로는 "길이를 모르는
// NULL 종단 배열을 얼마나 걸을지"를 결정할 수 없어(QU-B9EB45E4가
// 지적한 이 코드베이스의 새로운 검증 유형) 이 상한이 스캔 자체를
// 유한하게 만드는 역할도 한다.
constexpr uint64_t kMaxSpawnArgsTotalSize = 2UL * 1024 * 1024;

// [신규, PN-E35294B8 항목2, 구현 세부 - RM-23F4B687 §4 취지, 답변이
// 직접 정하지 않은 순수 구현 상수] argv/envp 항목 "개수"의 v1 상한 -
// kMaxSpawnArgsTotalSize(바이트)와는 별개 차원이다(빈 문자열만 잔뜩
// 넣으면 바이트 예산 안에서도 수백만 개까지 만들 수 있어, 오프셋
// 추적 배열 자체가 비대해지는 것을 막는 별도 방어). 현실적인 실행
// 인자 개수보다 넉넉히 크다.
constexpr uint32_t kMaxSpawnArgsEntryCount = 4096;

// 단일 요청 안에서 커널 버퍼로 복사하는 이미지 바이트의 상한 -
// GenericSlabAllocator::alloc()이 2048B 초과 요청을 PageFrameAllocator::
// allocOrder로 그대로 위임하는데, 그 buddy 할당자의 최대 order(10,
// page_frame_allocator.cpp의 kMaxOrder)가 4MiB라 이 값을 넘는 단일
// 할당은 애초에 성공할 수 없다 - 그 한도에 정확히 맞춘 값(실측 후
// 조정 가능한 순수 구현 세부, RM-23F4B687 §4).
constexpr uint64_t kMaxSpawnImageSize = 4UL * 1024 * 1024;

// [갱신, 2026-09-17, SP-E9B44929] Process 그룹(0) - Wait.
constexpr SyscallEndpointId kSyscallEndpointWait = kMakeSyscallEndpointId(0, 3);

// [신규, 2026-09-16, SP-6BEAE0C1 §6, PN-543C0CE9 착수 5번째 증분(2/2)]
// Wait syscall 인자 - **v1은 논블로킹**(POSIX `waitpid(pid, status,
// WNOHANG)`과 동일한 의미 - §11 "wait() syscall ABI(반환값 형태,
// waitpid(-1,...)류 지원 여부) - 순수 구현 세부, 착수하며 정한다"의
// 답을 이렇게 내렸다: 진짜 블로킹(자식이 죽을 때까지 호출자를 실제로
// 재우는)을 지원하려면 이 Process/UserThread 쌍을 걸어 둘 새 대기열
// 서브시스템이 통째로 더 필요한데, 지금은 그걸 실제로 쓸 유저랜드
// 소비자(예: init의 자동 회수 루프)가 전혀 없어 미리 만들 근거가
// 없다 - 나중에 필요해지면 이 ABI를 그대로 두고 "좀비가 없으면
// hadZombieChild=false로 즉시 반환" 대신 "생길 때까지 블로킹"으로
// 내부 구현만 바꿔 확장할 수 있다.
struct WaitArgs {
    // -1이면 아무 자식이나(POSIX wait(-1, ...)와 동일) - 그 외 값이면
    // [수정, 2026-09-17, PN-C39882D0] `SpawnProcessArgs::pid`와 동일한
    // 새 `ProcessId` 인코딩(자식 `Process::processId`)과 정확히
    // 일치하는 자식만 찾는다. 스코프는 여전히 "호출자의 직계 자식만"
    // (POSIX `wait()`와 동일 의미론, §5 - Wait은 임의 대상으로 확장된
    // 적 없다) - 바뀐 건 순수 값 형식뿐.
    int64_t targetPid = kInvalidProcessId;

    // out - hadZombieChild==true일 때만 reapedPid/exitCode가 유효하다.
    bool hadZombieChild = false;
    int64_t reapedPid = kInvalidProcessId;
    int32_t exitCode = 0;

    // out - targetPid 조건에 맞는 자식이(좀비든 아니든) 하나라도
    // 있었는지 - POSIX의 ECHILD("애초에 기다릴 자식이 없다") 판정을
    // 호출부가 hadZombieChild==false와 구분할 수 있게 한다.
    bool hasAnyChild = false;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PROCESS_H
