#ifndef MINICORE_KERNEL_PROCESS_H
#define MINICORE_KERNEL_PROCESS_H

#include "address_space.h"
#include "libkenv/types.h"
#include "signal.h"
#include "syscall.h"

namespace elf {
class Image;  // 전방 선언(minicore/libs/libelf/elf.h) - Process::execImage 시그니처용
}

namespace kernel {

class UserThread;

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
class Process {
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
    // 프로세스(init, kSpawnInitProcess)는 부모가 없는 루트라 nullptr
    // 그대로 남는다 - kSpawnServiceProcesses가 만드는 고정 스폰
    // KernelService들도 SpawnProcess syscall 경로를 타지 않으므로
    // 마찬가지로 nullptr(이 트리는 SpawnProcess로 만들어진 프로세스의
    // 부모-자식 관계만 표현한다 - 고정 스폰 서비스들의 생명주기는
    // 이미 별도의 resurrect/essential 메커니즘(§6.3/§6.4)이 관리).
    Process* parent = nullptr;

    // 이 프로세스가 SpawnProcess로 만든 자식들의 목록 - `wait()`가
    // 좀비(§6, 다음 증분에서 배선)를 찾을 때, 프로세스 종료 시 고아를
    // init에게 입양시킬 때 쓴다. 청크 용량 8은 프로세스당 자식 수가
    // 보통 많지 않을 거라는 가정의 순수 구현 세부(실측 후 조정 가능,
    // RM-23F4B687 §4) - `pendingSignals`와 달리 하드웨어 버킷 크기에
    // 맞출 이유가 없어 그냥 작게 시작한다.
    static constexpr uint32_t kMaxChildrenChunkCapacity = 8;
    ChunkedList<Process*, kMaxChildrenChunkCapacity> children;

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

    // Signal 전달 인프라(SP-0666DB3C §4.3, PN-71E50394) - 아직 대기
    // 중(전달 시도 전)인 신호들의 목록 + 각 신호 번호별 처리 방식.
    // 둘 다 init()에서 명시적으로 리셋한다 - Resurrect(§6.2)가 같은
    // 정적 Process 인스턴스를 재사용하므로, 이전 생애의 신호 상태가
    // 새 생애로 새어 들어가면 안 된다.
    ChunkedList<PendingSignal, kPendingSignalChunkCapacity> pendingSignals;
    SignalDisposition dispositions[kSignalCount];

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
    UserThread* execImage(const elf::Image& image, UserThread* thread);

    // Signal 전달(SP-0666DB3C §4.4, PN-71E50394 항목 2) - number를
    // pendingSignals에 기록하고, mainThread가 지금 대기 중이면
    // (blockedOn != nullptr) 그 자리에서 즉시 강제로 깨운다(§9.5,
    // Waitable::cancel 경유). mainThread가 실행 중/비대기 상태면 여기서는
    // 아무 것도 더 하지 않는다 - 체크포인트 방식(실행 중인 코드가 다음
    // syscall 진입/ring3 재진입 시점에 pendingSignals를 확인하는 것)은
    // 아직 어디에도 배선돼 있지 않다(별도 후속 항목). 실패(자원 고갈)
    // 시 false.
    bool raiseSignal(SignalNumber number);

    // [SP-6BEAE0C1, PN-543C0CE9 착수 4번째 증분] SpawnProcess(RM-48E1E610
    // 59번) syscall 엔드포인트를 SyscallRegistry에 등록한다 - Channel::
    // registerSyscallEndpoints()와 같은 관례(부팅 시 BSP에서 한 번,
    // kmain.cpp가 호출).
    static void registerSyscallEndpoints();
};

// RM-48E1E610 59번 - SpawnProcess.
constexpr SyscallEndpointId kSyscallEndpointSpawnProcess = 59;

// SpawnProcess 실패 사유(SP-6BEAE0C1 §3) - `ChannelError`와 같은 관례
// (out 파라미터로 결과 코드 하나 + 부가 값).
enum class SpawnProcessError : uint32_t {
    None = 0,
    InvalidImageRange,   // imageBuffer/imageSize가 Paging::isUserRangeValid를 통과 못함
    ImageTooLarge,       // kMaxSpawnImageSize 초과
    OutOfMemory,         // 커널 버퍼/Process·UserThread slab/페이지 고갈
    ElfParseFailed,      // elf::Image::parse 실패(BadMagic 등)
    ExecImageFailed,     // Process::execImage 실패(유저 스택 확보 등)
};

// [SP-6BEAE0C1 §3] SpawnProcess syscall 인자 - `imageBuffer`/`argv`/
// `envp`는 전부 유저 포인터(untrusted, 핸들러 내부에서 Paging::
// isUserRangeValid로 검증 후에만 역참조). **이번 증분 한계**: `argv`/
// `envp`는 아직 실제로 새 프로세스에 전달되지 않는다(§4 "인자/환경변수
// 전달 규약"이 프로젝트 전체에서 아직 미착수 - execImage()가 지금은
// 그런 스택 프레임을 구성하지 않는다, 이 syscall만의 제약이 아니다) -
// ABI 자리만 미리 잡아 두고 §4가 준비되면 이어붙인다.
struct SpawnProcessArgs {
    const void* imageBuffer = nullptr;  // 유저 포인터 - ELF64 이미지 원본 바이트
    uint64_t imageSize = 0;
    char* const* argv = nullptr;  // 유저 포인터, NULL 종단 - 아직 미사용(위 참고)
    char* const* envp = nullptr;  // 유저 포인터, NULL 종단 - 아직 미사용(위 참고)
    // out
    SpawnProcessError error = SpawnProcessError::None;
    int64_t pid = -1;  // 성공 시 새 Process*를 재해석한 값(AsyncTaskManageCode의
                        // "포인터를 그대로 토큰으로" 관례와 동일) - 실패 시 -1 유지.
};

// 단일 요청 안에서 커널 버퍼로 복사하는 이미지 바이트의 상한 -
// GenericSlabAllocator::alloc()이 2048B 초과 요청을 PageFrameAllocator::
// allocOrder로 그대로 위임하는데, 그 buddy 할당자의 최대 order(10,
// page_frame_allocator.cpp의 kMaxOrder)가 4MiB라 이 값을 넘는 단일
// 할당은 애초에 성공할 수 없다 - 그 한도에 정확히 맞춘 값(실측 후
// 조정 가능한 순수 구현 세부, RM-23F4B687 §4).
constexpr uint64_t kMaxSpawnImageSize = 4UL * 1024 * 1024;

}  // namespace kernel

#endif  // MINICORE_KERNEL_PROCESS_H
