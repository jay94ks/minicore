#include "process.h"

#include "gdt.h"
#include "libelf/elf.h"
#include "libkenv/mem.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "scheduler.h"
#include "syscall.h"
#include "waitable.h"

namespace {

// §5-A "스택 공간은 유저 공간의 끝점에서부터 맵핑" - 128TiB 유저
// 공간의 상한 바로 아래(4KiB 정렬)를 스택 top으로 고정한다. v1은
// 고정 크기(64KiB)/고정 주소이고 가드 페이지가 없다 - 커널 스택
// 가드 페이지(task.cpp, MINICORE_TASK_STACK_GUARD_PAGE)와 같은
// 패턴으로 후속 추가할 수 있는 자리만 남겨 둔다(새 DC 불필요 수준).
constexpr kernel::uint64_t kUserStackTop = 0x00007FFFFFFFF000UL;
constexpr kernel::uint64_t kUserStackSize = 16UL * 4096UL;  // 64KiB

// Process::addressSpace(ProcessAddressSpaceManager)의 findGap 탐색
// 범위(mmap 가능 영역, SP-2AAD7C8D §5-A "코드 공간 위쪽 ~ 스택 하단
// 사이 전부") - Mmap syscall(RM-48E1E610 17-19)이 아직 없어 이 범위
// 자체는 지금은 어느 mapRegion() 호출도 실제로 겪지 않는다(execImage()
// 의 코드/스택 매핑은 registerFixedRegion으로 findGap 없이 직접
// 등록). 정확한 정렬/여유 공간 상수는 SP-2AAD7C8D 자신이 "구현 시점
// 튜닝"으로 명시해 둔 항목이라(§5-A 이전 논의 상속) 새 DC 없이 이
// 자리에서 정한다 - 코드 베이스(0x400000)보다 충분히 위, 고정 유저
// 스택(kUserStackTop - kUserStackSize)보다 충분히 아래.
constexpr kernel::uint64_t kMmapRegionFloor = 0x10000000UL;         // 256MiB
constexpr kernel::uint64_t kMmapRegionCeil = kUserStackTop - kUserStackSize - 0x100000UL;  // 스택 아래 1MiB 여유

// PN-124C105B/PN-16CA347D 6번 - UserThread가 ring3으로 "처음" 진입하는
// 자리. Task::init()의 entry로 등록되어 kTaskStartTrampoline이 평범한
// ring0 함수처럼 호출하지만(`call rbx`, context_switch.S), 이 함수는
// 절대 돌아오지 않는다 - iretq가 특권 레벨 자체를 바꿔 버리기 때문이다.
// 이후 이 UserThread가 다시 ring0으로 오는 유일한 경로는 트랩/
// 인터럽트뿐이고(아래 RSP0 설정이 그 경로의 스택을 마련해 둔다),
// 스케줄러가 이 Task를 선점했다 재개하는 경우도 그 트랩의 iretq를
// 통해서만 ring3로 되돌아간다 - 기존 Task 컨텍스트 스위칭
// 메커니즘(kContextSwitch)이 이미 일반적으로 지원한다(InterruptFrame의
// iretq가 특권 레벨 전환까지 그대로 복원하므로 이 부분에 별도 코드가
// 필요 없다). 진입 파라미터(entryPoint/userStackTop)는 Task::init()의
// void* arg 슬롯이 아니라 이 Task 자신의 `ring3EntryPoint`/
// `ring3UserStackTop` 필드에서 직접 읽는다(PN-D0ED9611 - 예전엔 전역
// 인스턴스 하나를 공유해 두 번째 프로세스 exec() 시 첫 번째 값이
// 덮어써지는 결함이 있었다).
//
// **CR3 설정은 이 함수가 더 이상 직접 하지 않는다**(SP-83A07867,
// QU-892AB38A 설계자 답변, 2026-09-15 - CR3 동기화를 스케줄러 디스패치
// 공통 경로로 통합) - `kTaskStartTrampoline`(context_switch.S)이 이
// entry 콜백을 부르기 **직전**에 `kSyncCr3OnTaskStart()`(scheduler.cpp)
// 를 호출해 이미 `self->userPml4Phys`로 맞춰 둔다. 예전엔 이 함수가
// UserThread 한정으로 직접 CR3를 설정했는데(PN-63BCFE45 실측 발견 -
// 반드시 이 Task 자신의 안전한 스택으로 넘어온 뒤에만 `mov cr3`가
// 안전하다는 제약 자체는 그대로 유효), 그 로직이 이 함수 하나에만
// 있어 재디스패치/커널 Task/yieldCurrent 재개 등 다른 경로는 전혀
// CR3를 동기화하지 않는 문제가 반복 재발했다(PN-71C3D483) - 이제는
// "Task가 실행을 (재)시작하는 모든 지점"이 같은 `kSyncCr3()` 로직을
// 공유한다(SP-83A07867 §3.2).
[[noreturn]] void kEnterRing3(void*) {
    auto* self = kernel::Scheduler::currentTask();

    // RSP0은 이 함수에 도달하기 전에 이미 스케줄러가 맞춰 둔다
    // (Scheduler::runLoop()의 idle->Task 디스패치, PN-AEA74E1B) - GDT
    // TSS 구조체 쓰기는 지금 어떤 스택 위에서 실행 중이든 안전해서
    // (메모리 매핑과 무관한 순수 데이터 쓰기) runLoop() 자신이 아직
    // 이 Task 고유의 스택으로 넘어오기 전(idle 컨텍스트)에 해도 된다.

    // 아래 인라인 asm은 리터럴 0x1b/0x23을 직접 쓴다(피연산자 제약
    // 안에서 이름 있는 상수를 쓰면 크기 불일치 등으로 더 위험할 수
    // 있어, 리터럴+static_assert 조합을 택했다) - gdt.h의 값이
    // 바뀌면 여기서 빌드 타임에 바로 걸린다.
    static_assert(kernel::kGdtUserDataSelector == 0x1b, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");
    static_assert(kernel::kGdtUserCodeSelector == 0x23, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");

    const kernel::uint64_t entryPoint = self->ring3EntryPoint;
    const kernel::uint64_t userStackTop = self->ring3UserStackTop;

    // 세그먼트 레지스터는 유저 데이터 셀렉터로 미리 맞춰 두고(SS
    // 자체는 iretq 프레임이 담당), iretq 프레임(SS/RSP/RFLAGS/CS/RIP)
    // 을 쌓은 뒤 iretq로 실제 특권 레벨 전환을 일으킨다.
    asm volatile(
        "mov $0x1b, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq $0x1b\n\t"
        "pushq %0\n\t"
        "pushq $0x202\n\t"
        "pushq $0x23\n\t"
        "pushq %1\n\t"
        "iretq\n\t"
        :
        : "r"(userStackTop), "r"(entryPoint)
        : "rax", "memory");
    __builtin_unreachable();
}

}  // namespace

namespace kernel {

// [SP-6BEAE0C1 §5, PN-543C0CE9 착수 2번째 증분] 동적 Process 풀 - 지금까지
// 모든 Process 인스턴스는 정적 전역(kmain.cpp의 gInitProcess/
// gServiceProcess[])이라 컴파일러가 프로그램 시작 시 NSDMI(pml4Phys=0,
// mainThread=nullptr, pendingSignals의 내부 _head=nullptr 등)를 전부
// 실제로 적용해 준다 - 그래서 Resurrect(§6.2)가 그 위에 init()을 다시
// 불러도(pendingSignals.clear() 등) 항상 "이미 한 번은 진짜로 생성된
// 적 있는 객체" 상태였다.
//
// 이 GenericSlabAllocator 슬랩 메모리는 그런 보장이 전혀 없다(이
// 프로젝트는 placement new를 쓰지 않는 관례라 실제 생성자를 부를
// 방법도 없다) - 그대로 Process*로 캐스팅해 init()을 부르면
// pendingSignals.clear()가 쓰레기 값인 _head를 유효한 Chunk*로 착각해
// 걷다가 힙을 깨뜨린다(실측 전 코드 추적으로 발견 - Process의 거의
// 모든 필드가 정확히 0/nullptr NSDMI이므로, 실제 생성자가 만들어 낼
// 결과와 "전부 0으로 memset"이 비트 단위로 동일하다는 점을 이용해
// 이 한 줄로 그 전제를 다시 세워 준다).
Process* Process::allocate() {
    void* raw = GenericSlabAllocator::alloc(sizeof(Process));
    if (!raw) {
        return nullptr;
    }
    memset(raw, 0, sizeof(Process));
    return reinterpret_cast<Process*>(raw);
}

// 호출부가 먼저 destroy()로 이 프로세스가 소유한 자원(주소공간/VMA)을
// 전부 반납한 뒤에만 불러야 한다(Process 구조체 자신의 슬랩 메모리만
// 반납 - destroy()와 역할이 분리된 이유는 Vma/AsyncTask 등 이
// 코드베이스의 다른 "소유 자원 반납 vs 컨테이너 메모리 반납" 분리
// 관례와 동일).
void Process::release(Process* proc) {
    GenericSlabAllocator::free(proc, sizeof(Process));
}

// [신규, 2026-09-16, PN-543C0CE9 착수 5번째 증분(2/2)] Process::
// setOrphanRoot() 문서 주석 참고 - kmain.cpp가 부팅 중 딱 한 번만 채운다.
WeakPtr<Process> Process::gOrphanRoot;

bool Process::init() {
    pml4Phys = Paging::createAddressSpace();
    if (!pml4Phys) {
        return false;
    }
    mainThread = nullptr;
    lastFault = FaultInfo{};
    // 프로세스 트리(§6) - Resurrect(§6.2)가 같은 정적 Process를
    // 재사용할 수 있으므로, 이전 생애의 부모/자식 관계가 새 생애로
    // 새어 들어가지 않도록 매번 명시적으로 리셋한다(아래 pendingSignals
    // 와 동일한 이유). parent는 이 시점엔 아직 누가 부모인지 모르므로
    // 빈 WeakPtr로만 리셋해 두고, 실제 부모-자식 연결은 스폰 경로(예:
    // SpawnProcessHandler)가 init() 이후 직접 채운다.
    //
    // [수정, 2026-09-17, PN-E2A114C1] `parent = nullptr;`(raw 포인터
    // 대입)는 이제 `parent = WeakPtr<Process>();`(진짜 대입 연산자)가
    // 돼야 한다 - 옛 `parent`가 실제로 뭔가를 가리키고 있었다면(이론상
    // Resurrect 재사용 경로에서만, §6.3/§6.4 고정 스폰 프로세스는
    // 애초에 parent를 안 씀) 그 대상의 weakCount를 제대로 내려놔야
    // 하기 때문(단순 대입이면 옛 `_block`을 그냥 덮어써 weakCount가
    // 영원히 하나 새는 것과 같은 문제 - `children.clear()`가 이미
    // 겪은 것과 동일한 종류의 함정, chunked_list.h 수정 참고).
    // `children.clear()`도 같은 이유로 이제 슬롯의 SharedPtr을 실제로
    // 반납한다(ChunkedList::clear() 수정 참고) - 이 호출 자체는 바뀌지
    // 않았다.
    parent = WeakPtr<Process>();
    children.clear();
    // [신규, 2026-09-17, PN-9CC66142] children.clear()와 동일한 이유로
    // 매번 리셋 - Resurrect가 같은 정적 Process를 재사용할 수 있으므로
    // 이전 생애에 열려 있던 BridgePipe 강한 참조가 새 생애로 새어
    // 들어가면 안 된다.
    openBridges.clear();
    // 좀비 상태(§6, PN-543C0CE9 착수 5번째 증분(2/2)) - parent/children과
    // 동일한 이유(Resurrect가 같은 정적 Process를 재사용)로 매번 리셋.
    isZombie = false;
    exitCode = 0;
    addressSpace.init(pml4Phys, kMmapRegionFloor, kMmapRegionCeil);
    // Resurrect(§6.2)가 같은 정적 Process를 재사용할 수 있으므로,
    // 이전 생애의 신호 상태가 새 생애로 새어 들어가지 않도록 매번
    // 명시적으로 리셋한다(pml4Phys/addressSpace와 동일한 이유).
    pendingSignals.clear();
    for (uint32_t i = 0; i < kSignalCount; ++i) {
        dispositions[i] = SignalDisposition::Default;
    }
    // Brk(PN-012E8C1A §5) - 힙 VMA를 최소 크기(kMinHeapLength)로 지금
    // 즉시 만들어 heapStart/heapBrk를 처음부터 유효한 절대 주소로
    // 확정해 둔다. brk(newBrk)가 POSIX처럼 newBrk를 항상 "절대 주소"로
    // 다루려면(상대 크기가 아니라) 첫 호출 이전에도 이미 현재 브레이크
    // 값이 존재해야 하는데, mapRegion()은 특정 가상주소를 강제 지정할
    // 방법이 없어(findGap이 항상 고름) 그 결과를 그대로 heapStart로
    // 받아들이는 수밖에 없다 - 그래서 "브레이크가 존재하는 시점"
    // 자체를 Process::init()으로 앞당겼다. 실패하면(극히 드묾 - 방금
    // 만든 새 주소공간에 페이지 1개도 못 넣을 정도의 메모리 고갈)
    // 이 함수 자신도 실패로 보고한다(pml4Phys 확보 실패와 같은 급).
    if (!addressSpace.mapRegion(kMinHeapLength, PAGE_WRITABLE, VmaBacking::Anonymous, 0, &heapStart)) {
        return false;
    }
    // heapBrk - heapStart는 항상 힙 VMA의 실제 등록된 길이와 정확히
    // 같아야 한다(resizeAnonymousRegion이 oldLength로 그 값을 그대로
    // 받아 트리에서 기존 범위를 찾는 데 쓰므로) - 방금 mapRegion()이
    // 실제로 매핑한 크기(kMinHeapLength)를 그대로 반영한다. "논리적
    // 브레이크는 실제 매핑보다 작을 수 있다"는 여유를 두지 않는다 -
    // 그 여유가 곧 이 둘의 불변조건을 깨는 원인이었다(실측으로 발견).
    heapBrk = heapStart + kMinHeapLength;
    return true;
}

void Process::destroy() {
    if (pml4Phys) {
        // PN-71C3D483 항목 3 - execImage()가 registerFixedRegion으로
        // 장부에 남겨 둔 코드/데이터/스택 VMA를 전부 찾아 실제 페이지를
        // 반납한다. Paging::destroyAddressSpace(PML4 프레임 자체만
        // 반납)보다 반드시 먼저 불러야 한다(address_space.h의
        // unmapAll() 문서 주석과 동일한 전제).
        addressSpace.unmapAll();
        Paging::destroyAddressSpace(pml4Phys);
        pml4Phys = 0;
    }
}

UserThread* Process::execImage(const elf::Image& image, UserThread* thread) {
    if (!elf::loadIntoAddressSpace(image, pml4Phys, &addressSpace)) {
        return nullptr;
    }

    const uint64_t stackStart = kUserStackTop - kUserStackSize;
    for (uint64_t off = 0; off < kUserStackSize; off += 4096UL) {
        const uint64_t phys = PageFrameAllocator::allocPage();
        if (!phys) {
            // 이미 매핑한 세그먼트/스택 일부의 롤백은 Process::destroy()
            // 호출부 책임(elf::loadIntoAddressSpace와 동일한 관례).
            return nullptr;
        }
        Paging::mapPage(stackStart + off, phys, PAGE_WRITABLE | PAGE_USER, pml4Phys);
    }
    if (!addressSpace.registerFixedRegion(stackStart, kUserStackSize, PAGE_WRITABLE | PAGE_USER,
                                           VmaBacking::Anonymous)) {
        // 장부 등록 실패(트리 포화 등) - 이미 매핑된 스택 페이지 자체의
        // 롤백은 elf::loadIntoAddressSpace와 동일하게 호출부(Process::
        // destroy()) 책임으로 남긴다.
        return nullptr;
    }

    // [수정, 2026-09-17, PN-E2A114C1] `thread->process = this;`(raw
    // 포인터)에서 `weakFromThis()`로 전환 - 호출부(SpawnProcessHandler/
    // kSpawnInitProcess/kSpawnServiceProcesses)가 execImage()를 부르기
    // 전에 이미 `kMakeShared<Process>(this)`로 이 인스턴스를 감싸 뒀다는
    // 전제(그래야 `_weakThis`가 채워져 있어 `weakFromThis()`가 빈
    // WeakPtr이 아닌 진짜 값을 돌려준다) - 세 호출부 전부 그 순서를
    // 지키도록 갱신했다.
    thread->process = weakFromThis();
    thread->isUserLevel = true;
    thread->userPml4Phys = pml4Phys;
    thread->ring3EntryPoint = image.entryPoint();
    thread->ring3UserStackTop = kUserStackTop;
    thread->init(kEnterRing3, nullptr);
    // [신규, PN-523B779F] `thread`가 `UserThread::allocate()`를 거치지
    // 않은 정적 전역(kmain.cpp의 gInitThread/gServiceThread[])이면
    // `_selfRef`가 비어 있어 `weakAsTask()`/`submitterTask` 체이닝이
    // 전부 조용히 실패한다(syscall.h의 ensureSelfRef() 문서 주석
    // 참고) - allocate() 경로면 이미 채워져 있어 멱등하게 아무 일도
    // 안 한다. 실패(Slab 고갈)해도 이 함수 자체를 실패시키지 않는다 -
    // submitterTask 관련 기능(CR3 동기화/유저 포인터 검증)만 못 쓰게
    // 될 뿐 프로세스 기동 자체는 그 없이도 가능했던 기존 동작이다.
    thread->ensureSelfRef();
    mainThread = thread;
    return thread;
}

bool Process::raiseSignal(SignalNumber number) {
    if (number == SignalNumber::None) {
        return false;
    }
    pendingSignals.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    PendingSignal sig;
    sig.number = number;
    sig.used = true;
    if (!pendingSignals.insert(sig)) {
        return false;  // 자원 고갈 - 다른 ChunkedList 소비자와 동일한 정책
    }
    // §9.5 - 대기 중이면 그 자리에서 즉시 강제로 깨운다. cancel()의
    // 반환값(성공/실패)은 여기서 참고하지 않는다 - 실패는 "이미
    // 정상적으로 깨어난 뒤"라는 뜻이라 어차피 체크포인트 쪽에서 이
    // pendingSignals를 나중에 발견하면 되고, 이 함수 자신은 "기록은
    // 됐다"만 보장하면 된다.
    // [수정, 2026-09-17, PN-B41D8C0E] `blockedOn`이 이제 `WeakPtr<Waitable>`
    // 이라 `.lock()`으로 유효성을 확인해야 한다 - 대상 Mutex/Semaphore가
    // kMakeShared로 안 만들어졌으면 빈 값이라 이 강제 웨이크업만
    // 조용히 스킵된다(task.h의 blockedOn 주석 참고).
    if (mainThread) {
        if (SharedPtr<Waitable> waitable = mainThread->blockedOn.lock()) {
            waitable->cancel(mainThread, WaitCancelReason::Signal);
        }
    }
    return true;
}

namespace {

// [SP-6BEAE0C1 §3, PN-543C0CE9 착수 4번째 증분] SpawnProcess 본체 -
// 앞선 세 증분(PageFrameAllocator::retain/refCount, Process::
// allocate()/UserThread::allocate(), Paging::isUserRangeValid)을 실제로
// 엮는다. ChannelReadHandler/ChannelWriteHandler와 완전히 같은 관례
// (AsyncTaskHandler 하나 = syscall 엔드포인트 하나, args를 그 자리에서
// 직접 채워 co_return).
class SpawnProcessHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<SpawnProcessArgs*>(argsRaw);

        // 0단계 - [PN-A6E01B8A, QU-9585F6C4] flags 유효성 검증 - 정의
        // 안 된 비트가 하나라도 세팅되면 조용히 무시하지 않고 거부한다
        // (SP-6BEAE0C1 §3 "오타/버전 불일치를 바로 드러내기 위함" -
        // 표준 커널 syscall 관례). kSpawnDebugStart 비트 자체의 실제
        // 동작(자식을 Blocked로 시작)은 아직 미착수 - SP-9A6D579F
        // 착수 시 함께 구현(지금은 유효한 비트로만 인정될 뿐 효과 없음).
        if ((args->flags & ~kSpawnProcessFlagsMask) != 0) {
            args->error = SpawnProcessError::InvalidArgument;
            co_return;
        }

        // 1단계 - imageBuffer/imageSize 검증(SP-6BEAE0C1 §3 "기본적으로
        // untrusted"). isUserRangeValid는 length==0도 true를 돌려주므로
        // imageSize==0은 별도로 걸러야 한다(빈 ELF는 어차피 파싱
        // 실패하겠지만, 크기 상한 검사 이전에 명확히 거부).
        if (args->imageSize == 0 || args->imageSize > kMaxSpawnImageSize ||
            !Paging::isUserRangeValid(reinterpret_cast<uint64_t>(args->imageBuffer), args->imageSize)) {
            args->error = SpawnProcessError::InvalidImageRange;
            co_return;
        }

        // 2단계 - 검증을 통과한 뒤에만, 커널 버퍼로 딱 한 번 복사한다
        // (§3 "복사를 최소화하는 경로" - 이 요청 안에서 다시 복사하지
        // 않고 이 버퍼를 그대로 파싱+로드에 재사용한다).
        void* kernelImage = GenericSlabAllocator::alloc(args->imageSize);
        if (!kernelImage) {
            args->error = SpawnProcessError::OutOfMemory;
            co_return;
        }
        memcpy(kernelImage, args->imageBuffer, args->imageSize);

        elf::Image image;
        if (elf::Image::parse(kernelImage, args->imageSize, &image) != elf::Error::None) {
            GenericSlabAllocator::free(kernelImage, args->imageSize);
            args->error = SpawnProcessError::ElfParseFailed;
            co_return;
        }

        // 3단계 - 동적 Process/UserThread 확보(§5, allocate()가 이미
        // memset(0)까지 끝내 둠 - init() 호출 전제 조건).
        Process* proc = Process::allocate();
        UserThread* thread = proc ? UserThread::allocate() : nullptr;
        if (!proc || !thread || !proc->init()) {
            if (thread) {
                UserThread::release(thread);
            }
            if (proc) {
                Process::release(proc);
            }
            GenericSlabAllocator::free(kernelImage, args->imageSize);
            args->error = SpawnProcessError::OutOfMemory;
            co_return;
        }

        // [신규, 2026-09-17, PN-E2A114C1] init() 직후, execImage() 이전에
        // 이 Process를 kMakeShared로 감싼다 - execImage()가 내부에서
        // `weakFromThis()`를 부르므로(thread->process 세팅) 그보다
        // 먼저 `_weakThis`가 채워져 있어야 한다. 이 시점부터 `procShared`
        // 가 이 Process의 유일한 강한 소유자다 - 실패 시 `procShared`를
        // `reset()`하면 `proc->destroy()`+슬랩 반납이 자동으로 일어난다
        // (기본 삭제자 `kDestroyAndFree<Process>`, `destroy()`는
        // pml4Phys==0이면 아무 일도 안 하는 멱등 함수라 이미 destroy()를
        // 부른 뒤 다시 불러도 안전 - process.h `destroy()` 문서 참고).
        SharedPtr<Process> procShared = kMakeShared<Process>(proc);
        if (!procShared) {
            UserThread::release(thread);
            proc->destroy();
            Process::release(proc);
            GenericSlabAllocator::free(kernelImage, args->imageSize);
            args->error = SpawnProcessError::OutOfMemory;
            co_return;
        }

        // 4단계 - kEnterInitProcess/kSpawnServiceProcesses와 동일한
        // execImage 경로. **elf::Image는 원본 버퍼를 복사하지 않고
        // 그대로 가리키므로(elf.h 문서 주석) kernelImage는 execImage가
        // 끝난 뒤에만 반납한다** - loadIntoAddressSpace가 이 버퍼에서
        // 새 주소공간으로 실제 페이지 복사를 끝내는 지점이 execImage
        // 안이다.
        UserThread* started = procShared->execImage(image, thread);
        GenericSlabAllocator::free(kernelImage, args->imageSize);

        if (!started) {
            UserThread::release(thread);
            procShared.reset();  // destroy()+슬랩 반납(위 주석 참고)
            args->error = SpawnProcessError::ExecImageFailed;
            co_return;
        }

        // 5단계 - [확정, 2026-09-16, QU-52253384 답변] 프로세스 트리
        // 등록(§6) - 이 syscall을 부른 UserThread 자신의 프로세스가
        // 부모다. enqueue() 이전에 반드시 끝내야 한다 - 실패(슬랩
        // 고갈)하면 이미 시작된 스레드를 스케줄러에 올리기 전에 안전하게
        // 되돌릴 수 있는 마지막 지점이기 때문이다(엔큐 이후엔 이미
        // 실행 중일 수 있어 되돌릴 수 없다).
        //
        // [수정, 2026-09-17, PN-E2A114C1] `Process::children`이 이제
        // 자식의 유일한 강한 소유자다(process.h 문서 참고) - `caller->
        // process.lock()`으로 얻은 부모가 `procShared`를 자기
        // `children`에 복사해 넣는 순간부터가 진짜 소유의 시작이고,
        // 이 함수 끝에서 지역 변수 `procShared`가 스코프를 벗어나도
        // (강한 참조 하나 감소) 그 슬롯의 몫이 남아 있어 안전하다.
        auto* caller = static_cast<UserThread*>(Scheduler::currentTask());
        SharedPtr<Process> parentProc = caller ? caller->process.lock() : SharedPtr<Process>();
        if (!parentProc) {
            // 이론상 도달 불가(SpawnProcess는 항상 실제 UserThread
            // 실행 흐름에서만 온다, Syscall 클래스 문서 주석과 동일한
            // 전제) - 방어적으로만, 진짜 부모가 없으면 orphanRoot(init)
            // 를 대신 소유자로 삼는다(기존에도 "고아처럼 취급"이라고
            // 문서화돼 있던 그 상태를 SharedPtr 세계에서 실제로 안전하게
            // 구현한 것 - 소유자가 전혀 없으면 이 함수가 끝나는 순간
            // `procShared`가 스코프를 벗어나며 막 시작한 프로세스가
            // 그대로 파괴되는 use-after-free가 된다).
            parentProc = Process::orphanRoot().lock();
        }
        if (parentProc) {
            parentProc->children.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
            if (!parentProc->children.insert(procShared)) {
                UserThread::release(thread);
                procShared.reset();
                args->error = SpawnProcessError::OutOfMemory;
                co_return;
            }
            procShared->parent = WeakPtr<Process>(parentProc);
        }
        // parentProc이 이 시점에도 비어 있으면(진짜 root조차 없는 -
        // init도 아직 스폰 안 된 부팅 극초반) 정말 아무도 소유할 수
        // 없다 - 이 역시 이론상 도달 불가(SpawnProcess 자체가 유저
        // 프로세스 실행 흐름에서만 오므로 이미 최소 init은 떠 있어야
        // 함)라 더 방어하지 않는다.

        // argv/envp는 아직 실제로 전달하지 않는다(SpawnProcessArgs
        // 문서 주석 참고 - §4 전체가 미착수).
        Scheduler::enqueue(Scheduler::currentCoreIndex(), started);
        args->pid = reinterpret_cast<int64_t>(procShared.get());
        args->error = SpawnProcessError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SpawnProcessHandler gSpawnProcessHandler;

// [SP-6BEAE0C1 §6, RM-48E1E610 35번, PN-543C0CE9 착수 5번째 증분(2/2)]
// Wait 본체 - **논블로킹**(WaitArgs 문서 주석 참고). 호출자 자신의
// `children`에서 (targetPid 조건에 맞는) 좀비를 하나 찾아 회수한다.
class WaitHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask*, void* argsRaw) override {
        auto* args = static_cast<WaitArgs*>(argsRaw);
        args->hadZombieChild = false;
        args->reapedPid = -1;
        args->exitCode = 0;
        args->hasAnyChild = false;

        // SpawnProcessHandler::onExec과 동일한 관례로 호출자 자신의
        // Process를 얻는다 - 커널 Task(process가 비어 있음)가 이
        // syscall을 부를 일은 없다(SelfTerminateHandler와 동일한 전제),
        // 방어적으로만 확인.
        auto* caller = static_cast<UserThread*>(Scheduler::currentTask());
        SharedPtr<Process> self = caller ? caller->process.lock() : SharedPtr<Process>();
        if (!self) {
            co_return;
        }

        // [수정, 2026-09-17, PN-E2A114C1] `children`이 이제
        // `ChunkedList<SharedPtr<Process>, 8>`이므로 `zombie`도
        // `SharedPtr<Process>`(단순 복사 - 강한 참조 +1, 슬롯 자신의
        // 몫과는 별개로 이 지역 변수가 하나 더 쥔다). `erase()`가
        // 슬롯 몫을 내려놔도(chunked_list.h 수정 참고) 이 지역 변수가
        // 함수 끝까지 살려 두므로 `zombie->exitCode` 등을 그 뒤에
        // 읽어도 안전하다.
        SharedPtr<Process> zombie;
        decltype(self->children)::Slot* zombieSlot = nullptr;
        self->children.forEach([&](SharedPtr<Process>& child, auto* slot) {
            if (!child) {
                return;
            }
            if (args->targetPid != -1 && reinterpret_cast<int64_t>(child.get()) != args->targetPid) {
                return;
            }
            args->hasAnyChild = true;
            if (!zombie && child->isZombie) {
                zombie = child;
                zombieSlot = slot;
            }
        });

        if (zombie) {
            args->hadZombieChild = true;
            args->reapedPid = reinterpret_cast<int64_t>(zombie.get());
            args->exitCode = zombie->exitCode;
            // mainThread는 아직 SharedPtr 관리 대상이 아니다(PN-B4987BF6
            // 별도 계획, Task 자체를 SharedPtr로 옮기는 더 위험한 작업 -
            // 이번 범위 밖) - 명시적으로 계속 반납해야 한다.
            if (zombie->mainThread) {
                UserThread::release(zombie->mainThread);
            }
            // [수정, 2026-09-17, PN-E2A114C1] `Process::release(zombie)`
            // 를 더 이상 직접 부르지 않는다 - `erase()`가 슬롯의 강한
            // 참조를 내려놓고(위 `zombie` 지역 변수가 아직 하나를 쥐고
            // 있어 이 시점엔 파괴 안 됨), 이 함수 끝에서 `zombie` 자신이
            // 스코프를 벗어나며 마지막 강한 참조를 내려놓는 순간 실제로
            // 파괴된다(`kDestroyAndFree<Process>` - `destroy()`는
            // self-terminate 시점에 이미 불려 멱등하게 아무 일도 안 하고,
            // `GenericSlabAllocator::free`만 실제로 수행됨) - 여기서
            // `Process::release()`를 또 부르면 이중 반납이 된다.
            self->children.erase(zombieSlot);
        }
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

WaitHandler gWaitHandler;

}  // namespace

void Process::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointSpawnProcess, &gSpawnProcessHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointWait, &gWaitHandler);
}

}  // namespace kernel
