#include "task.h"

#include "acpi.h"
#include "libkenv/mem.h"
#include "libkenv/spinlock.h"
#include "libkenv/types.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "scheduler.h"

// linker.ld가 정의하는 thread_local 템플릿 경계(PN-22E5E9E7 항목1) -
// kTlsTemplateStart~kTlsTemplateTdataEnd는 파일에 실제 바이트가 있는
// .tdata 범위(memcpy 대상), kTlsTemplateTdataEnd~kTlsTemplateEnd는
// .tbss(항상 0으로 memset) 범위.
extern "C" char kTlsTemplateStart[];
extern "C" char kTlsTemplateTdataEnd[];
extern "C" char kTlsTemplateEnd[];

namespace {

extern "C" void kTaskStartTrampoline();

// stackSize를 담기에 충분한 최소 버디 order(4KiB 단위) - 스택 크기는
// 항상 4KiB의 배수로 반올림된다(호출부가 딱 떨어지는 값을 넘기지
// 않으면 실제 확보량이 커질 수 있음, 방어적 반올림이라 문제 없음).
kernel::uint32_t kOrderForStackSize(kernel::uint64_t stackSize) {
    kernel::uint64_t pages = (stackSize + 4095UL) / 4096UL;
    kernel::uint32_t order = 0;
    while ((1UL << order) < pages) {
        ++order;
    }
    return order;
}

#if MINICORE_TASK_STACK_GUARD_PAGE

// direct map(paging.h의 kDirectMapBase)은 1GiB 거대페이지로 통짜
// 매핑돼 있어(Paging::init) 그 안에서는 4KiB 단위로 구멍을 낼 수
// 없다 - 그래서 가드 페이지가 켜지면 direct map을 아예 쓰지 않고,
// 스택마다 이 전용 가상주소 구간에 4KiB 페이지 단위로 개별
// 매핑한다(Paging::mapPage) - 다른 용도(direct map/lazy zone/LAPIC
// 등 MMIO)가 쓰는 구간과 겹치지 않는 별도 슬롯이다(관계도 참고).
constexpr kernel::uint64_t kTaskStackVirtBase = 0xFFFF902000000000UL;

kernel::Spinlock gStackVirtLock;
kernel::uint64_t gNextStackVirtBase = kTaskStackVirtBase;

// pageCount개짜리 스택 + 바로 아래 가드 페이지 1개를 합친 만큼 가상
// 주소를 순차로 떼어준다(재사용/반납은 아직 없음 - Task 소멸 자체가
// 구현 안 됨, PL-2D3184BC 8단계 이후 과제). 반환값은 가드 페이지의
// 시작 주소이고, 그 바로 위(+4096)부터가 실제 스택이다.
kernel::uint64_t kReserveStackVirtRange(kernel::uint32_t pageCount) {
    kernel::SpinlockGuard guard(gStackVirtLock);
    const kernel::uint64_t guardBase = gNextStackVirtBase;
    gNextStackVirtBase += static_cast<kernel::uint64_t>(pageCount + 1) * 4096UL;
    return guardBase;
}

#endif  // MINICORE_TASK_STACK_GUARD_PAGE

// [신규, 2026-09-18, PN-22E5E9E7 항목2/4, SP-29D652AA §4.1] .tdata/
// .tbss 템플릿을 복사해 이 Task 전용 TCB 블록을 만들고 그 FS_BASE
// 값(템플릿 바로 뒤에 이어붙인 self-pointer 헤더의 주소)을 반환한다 -
// x86_64 TLS variant II 레이아웃.
//
// [실측 정정, 2026-09-18] 처음엔 `-ftls-model=local-exec`(cmake/
// toolchain-x86_64.cmake)면 컴파일러가 FS:0을 절대 역참조하지 않을
// 것이라 보고 self-pointer 헤더 없이 순수 템플릿 복사본만 만들었으나,
// QEMU 실측(TEMP thread_local 프로브, 이 계획의 검증 절 참고)에서
// 즉시 페이지 폴트로 크래시했다 - `objdump`로 원인을 추적한 결과,
// `gTlsSlots`가 extern(다른 번역 단위에서 접근)이라 Itanium C++ ABI가
// 요구하는 **TLS 래퍼 함수**(`_ZTWN6kernel9gTlsSlotsE`, 같은 헤더를
// include하는 모든 TU가 이 심볼을 통해서만 접근)가 자동 생성됐는데,
// 이 래퍼는 `-ftls-model`과 무관하게 항상 `mov %fs:0x0, %rax`로
// "스레드 포인터 자신"부터 읽은 뒤 거기에 링크 타임 음수 오프셋을
// 더하는 모델-불가지론적(model-agnostic) 관례를 쓴다 - `-ftls-model`은
// **같은 TU 안에서의 직접 접근**에만 영향을 준다. 그래서 FS:0에 정말로
// self-pointer(자기 자신의 주소)가 있어야 한다 - dtv는 불필요(이 래퍼가
// dtv 인덱싱을 쓰지 않고 오프셋을 직접 더하는 것까지 실측으로 확인함).
//
// **정렬**: 이 함수 자신은 정렬을 별도로 강제하지
// 않고 `memsz`(kTlsTemplateEnd - kTlsTemplateStart, 링커가 이미
// PT_TLS.p_align에 맞춰 반올림해 둔 값 그대로)를 그대로 슬랩 요청
// 크기로 쓴다 - 실측(`readelf -l`) 결과 GCC/binutils가 이 세그먼트에
// 고른 `p_align`은 16(linker.ld의 `.tdata`/`.tbss` `ALIGN(8)` 지시보다
// 큼 - 링커 자체 기본값으로 보인다)이었지만, `GenericSlabAllocator`의
// 버킷 할당(현재 128B 버킷)이 매번 페이지 내 등분 오프셋이라 이미
// 그보다 넓게 정렬돼 있어(§2.3 - 버킷 크기의 배수 오프셋이면 항상
// 그 버킷 크기만큼 정렬됨) 지금은 우연히 문제가 없다. **정렬 요구가
// 실제 버킷 크기를 넘어서게 되면**(예: 16바이트보다 넓은 정렬이 필요한
// thread_local 변수가 추가되고 버킷이 그보다 작아지면) 이 함수가 명시적
// 정렬 보정을 해야 한다 - 지금은 그런 변수가 없어 미루고 이 사실만
// 기록해 둔다(RM-23F4B687 §4 원칙, 실측 후 조정).
//
// 실패(슬랩 고갈) 시 0을 반환 - 호출부(Task::init())는 이미 다른
// 실패 가능 할당(kernelStackPhys)도 확인하지 않는 것과 같은 수준으로
// 그대로 둔다(이 프로젝트가 OOM을 이 함수 하나만 특별 취급하지 않음) -
// kernelFsBase가 0으로 남으면 kSyncFsBase(scheduler.cpp)가 FS_BASE에
// 0을 실어 이후 이 Task의 thread_local 접근이 잘못된 주소를 건드리게
// 되지만, 이미 커널 스택 할당 자체가 실패하는 것과 동급의 치명적
// OOM 상황이라 이 함수만 별도로 방어하지 않는다.
kernel::uint64_t kMakeTaskTlsBlock() {
    const auto templateStart = reinterpret_cast<kernel::uint64_t>(kTlsTemplateStart);
    const auto tdataEnd = reinterpret_cast<kernel::uint64_t>(kTlsTemplateTdataEnd);
    const auto templateEnd = reinterpret_cast<kernel::uint64_t>(kTlsTemplateEnd);
    const kernel::uint64_t filesz = tdataEnd - templateStart;
    const kernel::uint64_t memsz = templateEnd - templateStart;
    constexpr kernel::uint64_t kTcbHeaderSize = 8;  // self-pointer 하나뿐(위 문서 주석)

    void* block = kernel::GenericSlabAllocator::alloc(memsz + kTcbHeaderSize);
    if (!block) {
        return 0;
    }
    memcpy(block, reinterpret_cast<const void*>(templateStart), filesz);
    memset(static_cast<kernel::uint8_t*>(block) + filesz, 0, memsz - filesz);
    const kernel::uint64_t fsBase = reinterpret_cast<kernel::uint64_t>(block) + memsz;
    *reinterpret_cast<kernel::uint64_t*>(fsBase) = fsBase;  // FS:0 self-pointer
    return fsBase;
}

}  // namespace

namespace kernel {

// [신규, 2026-09-20, PN-C536F352] task.h의 TaskOwnerRef 선언 참고 -
// Scheduler::currentCoreIndex()/currentTask()를 참조해야 하는데
// scheduler.h가 이미 task.h를 include하므로 여기(task.cpp, 이미
// scheduler.h를 include함)에 정의를 둔다.
TaskOwnerRef TaskOwnerRef::capture(WeakPtr<Task> owner) {
    TaskOwnerRef ref;
    ref._ownerTask = owner;
    ref._ownerCoreIndexAtCapture = Scheduler::currentCoreIndex();
    return ref;
}

bool TaskOwnerRef::isCurrentCoreOwner() const {
    SharedPtr<Task> owner = resolve();
    return owner && owner.get() == Scheduler::currentTask();
}

void Task::init(TaskEntry entry, void* arg, uint64_t stackSize) {
    // [신규, 2026-09-17, PN-73E61BD1 항목1] `waitQueueLink`(libkcont
    // `Node`)의 "비어 있음" 표현은 nullptr이 아니라 자기 자신을
    // 가리키는 self-reference다(intrusive_list.h 참고) -
    // `UserThread::allocate()`가 raw 슬랩 메모리를 `memset(0)`으로만
    // 준비하므로(placement new 없음, 이 프로젝트 전역 관례) 이 필드는
    // 반드시 여기서 명시적으로 self-reference 상태로 되돌려야 한다
    // (PN-633BF2D8 TEMP 검증 중 발견한 `List::init()`의 `_sentinel
    // = Node{}` 버그와 근본적으로 같은 함정 - 대입이 아니라 필드를
    // 직접 채워야 한다).
    waitQueueLink.prev = &waitQueueLink;
    waitQueueLink.next = &waitQueueLink;

    // [신규, PN-A74871F2] 이 Task를 생성 중인 코어의 NUMA 노드를 사후
    // 기록 - 할당 정책 자체는 이미 PageFrameAllocator::allocOrder()가
    // "현재 코어 노드 우선"으로 하고 있으므로 여기서는 그 사실을 나중에
    // 다시 조회할 수 있게 값만 남긴다.
    numaNode = Acpi::cpuNumaNode(Scheduler::currentCoreIndex());

    // [신규, 2026-09-18, PN-22E5E9E7 항목2] 이 Task 전용 thread_local
    // TCB 인스턴스 - Scheduler::currentTask()가 이 Task를 가리키기 전
    // (아직 어느 큐에도 없음)에 만들어도 안전하다(순수 로컬 계산, 다른
    // Task/코어 상태를 안 건드림). kSyncFsBase(scheduler.cpp)가 이
    // Task가 실제로 디스패치되는 순간 FS_BASE에 실제로 반영한다.
    kernelFsBase = kMakeTaskTlsBlock();

    const uint32_t order = kOrderForStackSize(stackSize);
    kernelStackPhys = PageFrameAllocator::allocOrder(order);
    kernelStackSize = 4096UL << order;

#if MINICORE_TASK_STACK_GUARD_PAGE
    // 가드 페이지(guardBase, 의도적으로 안 매핑) 바로 위부터 스택을
    // 페이지 단위로 매핑한다 - 스택이 이 아래로 넘치면 #PF가 걸린다.
    // **주의(실측 확인, DC-3D3212A4)**: 이 #PF는 CR2가 가드 페이지를
    // 정확히 가리키긴 하지만, 이미 다 찬 스택에 인터럽트 프레임을
    // 푸시하려다 재폴트 -> #DF -> 트리플 폴트(조용한 리셋)로
    // 이어진다 - IST 없이는 idt.cpp의 kPanic 진단 로그까지 도달하지
    // 못한다. 지금은 "진단 없는 확실한 크래시"까지만 보장(설계자
    // 결정 대기 중, QU-4E00C118).
    const uint32_t pageCount = static_cast<uint32_t>(kernelStackSize / 4096UL);
    const uint64_t guardBase = kReserveStackVirtRange(pageCount);
    const uint64_t stackVirtBase = guardBase + 4096UL;
    for (uint32_t i = 0; i < pageCount; ++i) {
        Paging::mapPage(stackVirtBase + static_cast<uint64_t>(i) * 4096UL,
                         kernelStackPhys + static_cast<uint64_t>(i) * 4096UL, PAGE_WRITABLE);
    }
    const uint64_t stackTop = stackVirtBase + kernelStackSize;
#else
    const uint64_t stackTop = kPhysToVirt(kernelStackPhys) + kernelStackSize;
#endif
    kernelStackTop = stackTop;

    // [갱신, 2026-09-20, PN-81E49523 1단계, QU-AA1AA7F9] kContextSwitch가
    // 이제 전체 GPR+RFLAGS를 pop한다(context_switch.S 참고) - **push는
    // 스택을 감소 방향으로 채우므로 "쓰는 순서"는 "pop되는 순서"의
    // 정반대다**: 실제 pop 순서는 r15,r14,...,rax,popfq,ret(가장 먼저
    // pop되는 r15가 가장 낮은 주소=savedRsp) - 그래서 여기서는 높은
    // 주소부터 retaddr, rflags, rax, rbx, ..., r15 순으로 써야 마지막
    // 쓰기(r15)가 가장 낮은 주소(=savedRsp)에 정확히 오게 된다. rbx=entry,
    // r12=arg로 채워 kTaskStartTrampoline이 그대로 꺼내 쓰게 하고,
    // 나머지 GPR은 안 쓰므로 0으로 채운다. RFLAGS는 IF=1(인터럽트 허용,
    // 비트9)만 켜서 시작한다.
    auto* sp = reinterpret_cast<uint64_t*>(stackTop);
    *(--sp) = reinterpret_cast<uint64_t>(&kTaskStartTrampoline);  // "return address"
    *(--sp) = 0x202;                                              // RFLAGS: IF=1 + 예약된 비트1
    *(--sp) = 0;                                                  // rax
    *(--sp) = reinterpret_cast<uint64_t>(entry);                  // rbx -> 트램폴린이 call
    *(--sp) = 0;                                                  // rcx
    *(--sp) = 0;                                                  // rdx
    *(--sp) = 0;                                                  // rsi
    *(--sp) = 0;                                                  // rdi
    *(--sp) = 0;                                                  // rbp
    *(--sp) = 0;                                                  // r8
    *(--sp) = 0;                                                  // r9
    *(--sp) = 0;                                                  // r10
    *(--sp) = 0;                                                  // r11
    *(--sp) = reinterpret_cast<uint64_t>(arg);                    // r12 -> 트램폴린이 rdi로 옮김
    *(--sp) = 0;                                                  // r13
    *(--sp) = 0;                                                  // r14
    *(--sp) = 0;                                                  // r15

    savedRsp = reinterpret_cast<uint64_t>(sp);
    state = TaskState::Ready;
    hasEverRun = false;  // PN-44C91D6E - task.h 문서 주석 참고
    // [신규, 2026-09-19, PN-414BF822] kContextSwitchToFreshTask()가
    // onTick()에서 이 Task를 직접(인터럽트 컨텍스트에서) 첫 디스패치할
    // 때 쓸 값 - 위 가짜 콜리세이브 프레임에 이미 같은 값을 심어 뒀지만
    // (rbx/r12 자리), 그건 kContextSwitch의 pop 규약 전용이라 별도
    // 명명 필드로도 남겨 둔다(둘 다 같은 entry/arg를 가리키는 병행
    // 표현일 뿐, 이 Task가 실제로 도달해야 하는 지점은 동일하다).
    entryFn = entry;
    entryArg = arg;
    // [신규, 2026-09-19, PN-8726CDBD] async_task.h의 AsyncTask::init()이
    // selfWaitable을 명시적으로 리셋해 두는 것과 같은 이유 - 이 Task가
    // (memset(0)을 새로 거치지 않고) 재사용되는 경로가 있다면 이전
    // 수명에서 남은 TaskFpuContext가 새어나가지 않도록 여기서도
    // 명시적으로 반납한다(fresh memset 직후 호출되는 정상 경로에서는
    // 이미 nullptr이라 무해한 재확인).
    fpuContext.reset();
}

}  // namespace kernel
