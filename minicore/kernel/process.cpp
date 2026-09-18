#include "process.h"

#include "gdt.h"
#include "libelf/elf.h"
#include "libkenv/mem.h"
#include "libkenv/spinlock.h"
#include "libkmm/slab.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "resource_group.h"
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

    // 세그먼트 레지스터는 유저 데이터 셀렉터로 먼저 맞춘다(SS 자체는
    // iretq 프레임이 담당).
    asm volatile(
        "mov $0x1b, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        :
        : "rax", "memory");

    // [신규, 2026-09-18, PN-22E5E9E7 항목7, 실측 수정] `self`는 이
    // 함수의 유일한 호출부(execImage()의 `thread->init(kEnterRing3,
    // nullptr)`)가 항상 UserThread에만 거는 entry라 안전하게
    // 캐스팅할 수 있다(kSyncDebugRegs 등 기존 `static_cast<UserThread*>
    // (task)` 관례와 동일). **반드시 위 세그먼트 셀렉터 reload
    // 다음에** 와야 한다 - `mov %%ax, %%fs`(FS 셀렉터 재적재)가 x86_64
    // 에서도 그 세그먼트의 숨은 base를 GDT 디스크립터 값(평범한 플랫
    // 데이터 세그먼트라 0)으로 되돌린다는 것을 QEMU 실측으로 처음
    // 발견했다 - 이 wrmsr을 셀렉터 reload **이전에** 뒀더니 방금 세운
    // FS_BASE가 즉시 0으로 도로 지워져, ring3 첫 명령(`%fs:0x0` 읽기,
    // clang이 thread_local 접근에 쓰는 self-pointer 관례)이 cr2=0
    // 페이지 폴트로 죽었다(TEMP 트레이스로 확인). 이 wrmsr이 없으면
    // FS_BASE가 이전 커널 TCB(`kSyncCr3OnTaskStart`가 방금 맞춘
    // kernelFsBase) 값으로 남아 유저 thread_local 접근이 커널 메모리를
    // 가리키게 된다.
    kernel::kSyncFsBaseToUser(static_cast<kernel::UserThread*>(self));

    // iretq 프레임(SS/RSP/RFLAGS/CS/RIP)을 쌓은 뒤 iretq로 실제 특권
    // 레벨 전환을 일으킨다.
    asm volatile(
        "pushq $0x1b\n\t"
        "pushq %0\n\t"
        "pushq $0x202\n\t"
        "pushq $0x23\n\t"
        "pushq %1\n\t"
        "iretq\n\t"
        :
        : "r"(userStackTop), "r"(entryPoint)
        : "memory");
    __builtin_unreachable();
}

// [신규, 2026-09-18, SP-76250478 §2.2, PN-0EB2FABF] `CreateThread`가
// 만든 스레드 전용 ring3 진입점 - `kEnterRing3`와 거의 동일하지만
// (세그먼트 reload -> `kSyncFsBaseToUser` -> iretq 순서, 그 함수 문서
// 주석이 실측으로 확인해 둔 "셀렉터 reload가 FS_BASE를 지운다" 함정을
// 똑같이 피한다) **RDI에 `threadStartArg`를 실어 SysV 관례대로
// `entry(arg)` 호출처럼 보이게 한다** - `kEnterRing3`이 진입하는
// ELF `_start`는 인자를 스택(argc/argv/envp)으로 받지 레지스터로
// 받지 않아 이 차이가 필요했다. `self`는 이 함수의 유일한 호출부
// (`CreateThreadHandler::onExec()`의 `thread->init(kEnterRing3Thread,
// nullptr)`)가 항상 `UserThread`에만 거는 entry라 안전하게 캐스팅할
// 수 있다(kEnterRing3와 동일한 근거).
[[noreturn]] void kEnterRing3Thread(void*) {
    auto* self = static_cast<kernel::UserThread*>(kernel::Scheduler::currentTask());

    static_assert(kernel::kGdtUserDataSelector == 0x1b, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");
    static_assert(kernel::kGdtUserCodeSelector == 0x23, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");

    const kernel::uint64_t entryPoint = self->ring3EntryPoint;
    const kernel::uint64_t userStackTop = self->ring3UserStackTop;
    const kernel::uint64_t startArg = self->threadStartArg;

    asm volatile(
        "mov $0x1b, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        :
        : "rax", "memory");

    kernel::kSyncFsBaseToUser(self);

    // `"D"(startArg)` - 컴파일러가 이 값을 RDI에 실어 두게 강제한다
    // (SysV 첫 인자 레지스터). 그 뒤 push들은 RDI를 전혀 건드리지
    // 않으므로 iretq가 실행되는 순간에도 RDI는 그대로 startArg다.
    asm volatile(
        "pushq $0x1b\n\t"
        "pushq %0\n\t"
        "pushq $0x202\n\t"
        "pushq $0x23\n\t"
        "pushq %1\n\t"
        "iretq\n\t"
        :
        : "r"(userStackTop), "r"(entryPoint), "D"(startArg)
        : "memory");
    __builtin_unreachable();
}

// [신규, PN-44C91D6E, fork() 자식 재개 경로] `kEnterRing3`와 정반대
// 전제 - 고정 entryPoint/새 스택이 아니라, 부모가 트랩한 시점의 전체
// InterruptFrame(`UserThread::forkResumeFrame`, fork() 핸들러가
// 부모 프레임을 복사해 rax만 0으로 덮어써 채워 둔다)을 그대로 iretq해
// "부모가 트랩한 바로 그 지점에서" 재개한다. 세그먼트 셀렉터 reload +
// `kSyncFsBaseToUser` 순서는 `kEnterRing3`와 완전히 동일(그 함수
// 문서 주석이 이미 실측으로 확인해 둔 "셀렉터 reload가 FS_BASE를
// 지운다" 함정을 그대로 피한다).
[[noreturn]] void kResumeForkedRing3(void*) {
    auto* self = static_cast<kernel::UserThread*>(kernel::Scheduler::currentTask());
    // 로컬 스택 복사본 - 아래 인라인 asm이 이 사본의 주소 하나만
    // 읽어 그 자리에서 바로 push하므로, self->forkResumeFrame 원본을
    // 이후 다시 쓸 필요가 없다(1회성 소비).
    const kernel::InterruptFrame frame = self->forkResumeFrame;

    static_assert(kernel::kGdtUserDataSelector == 0x1b, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");
    static_assert(kernel::kGdtUserCodeSelector == 0x23, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");
    static_assert(sizeof(kernel::InterruptFrame) == 176,
                  "InterruptFrame 크기가 바뀌면 아래 push 오프셋도 같이 재확인할 것");

    asm volatile(
        "mov $0x1b, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        :
        : "rax", "memory");

    kernel::kSyncFsBaseToUser(self);

    // isr_common_stub(isr.S)의 push 순서(r15..rax, 마지막 push=rax가
    // 최저 주소)와 정확히 같은 레이아웃을 이 함수 자신의 스택 위에
    // 그대로 재현한 뒤 isr_common_epilogue(같은 pop+iretq 시퀀스,
    // 이미 실전 검증됨)로 점프한다. `%0`(frame의 주소) 레지스터
    // **하나만** 읽고 다른 GPR은 전혀 건드리지 않는다 - `push m64`는
    // 스택 포인터만 갱신할 뿐 스크래치 레지스터가 필요 없어서다(레지스터
    // 별로 값을 옮겨 담았다가 나중에 그 레지스터를 또 다른 필드의 주소
    // 계산에 재사용하다 꼬이는 위험을 원천적으로 피한다).
    asm volatile(
        "push %c[ssOld](%0)\n\t"
        "push %c[rspOld](%0)\n\t"
        "push %c[rflags](%0)\n\t"
        "push %c[cs](%0)\n\t"
        "push %c[rip](%0)\n\t"
        "push %c[errorCode](%0)\n\t"
        "push %c[vector](%0)\n\t"
        "push %c[r15](%0)\n\t"
        "push %c[r14](%0)\n\t"
        "push %c[r13](%0)\n\t"
        "push %c[r12](%0)\n\t"
        "push %c[r11](%0)\n\t"
        "push %c[r10](%0)\n\t"
        "push %c[r9](%0)\n\t"
        "push %c[r8](%0)\n\t"
        "push %c[rbp](%0)\n\t"
        "push %c[rdi](%0)\n\t"
        "push %c[rsi](%0)\n\t"
        "push %c[rdx](%0)\n\t"
        "push %c[rcx](%0)\n\t"
        "push %c[rbx](%0)\n\t"
        "push %c[rax](%0)\n\t"
        "jmp isr_common_epilogue\n\t"
        :
        : "r"(&frame),
          [rax] "i"(__builtin_offsetof(kernel::InterruptFrame, rax)),
          [rbx] "i"(__builtin_offsetof(kernel::InterruptFrame, rbx)),
          [rcx] "i"(__builtin_offsetof(kernel::InterruptFrame, rcx)),
          [rdx] "i"(__builtin_offsetof(kernel::InterruptFrame, rdx)),
          [rsi] "i"(__builtin_offsetof(kernel::InterruptFrame, rsi)),
          [rdi] "i"(__builtin_offsetof(kernel::InterruptFrame, rdi)),
          [rbp] "i"(__builtin_offsetof(kernel::InterruptFrame, rbp)),
          [r8] "i"(__builtin_offsetof(kernel::InterruptFrame, r8)),
          [r9] "i"(__builtin_offsetof(kernel::InterruptFrame, r9)),
          [r10] "i"(__builtin_offsetof(kernel::InterruptFrame, r10)),
          [r11] "i"(__builtin_offsetof(kernel::InterruptFrame, r11)),
          [r12] "i"(__builtin_offsetof(kernel::InterruptFrame, r12)),
          [r13] "i"(__builtin_offsetof(kernel::InterruptFrame, r13)),
          [r14] "i"(__builtin_offsetof(kernel::InterruptFrame, r14)),
          [r15] "i"(__builtin_offsetof(kernel::InterruptFrame, r15)),
          [vector] "i"(__builtin_offsetof(kernel::InterruptFrame, vector)),
          [errorCode] "i"(__builtin_offsetof(kernel::InterruptFrame, errorCode)),
          [rip] "i"(__builtin_offsetof(kernel::InterruptFrame, rip)),
          [cs] "i"(__builtin_offsetof(kernel::InterruptFrame, cs)),
          [rflags] "i"(__builtin_offsetof(kernel::InterruptFrame, rflags)),
          [rspOld] "i"(__builtin_offsetof(kernel::InterruptFrame, rspOld)),
          [ssOld] "i"(__builtin_offsetof(kernel::InterruptFrame, ssOld))
        : "memory");
    __builtin_unreachable();
}

}  // namespace

namespace kernel {

// [SP-6BEAE0C1 §5, PN-543C0CE9 착수 2번째 증분] 동적 Process 풀 - 지금까지
// 모든 Process 인스턴스는 정적 전역(kmain.cpp의 gInitProcess/
// gServiceProcess[])이라 컴파일러가 프로그램 시작 시 NSDMI(pml4Phys=0,
// threads의 내부 _head=nullptr, pendingSignals의 내부 _head=nullptr 등)를 전부
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
    // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] `mainThread = nullptr;`
    // (raw 포인터 대입)를 대체 - children/openBridges와 동일한 이유로
    // 매번 명시적으로 clear()한다(Resurrect가 같은 정적 Process를
    // 재사용할 수 있으므로 이전 생애의 스레드 목록이 새 생애로 새어
    // 들어가면 안 된다). clear()는 ChunkedList 자신이 각 슬롯의
    // SharedPtr을 T{}로 되돌려 강한 참조를 실제로 내려놓는다(chunked_list.h
    // 참고) - 다만 이 시점엔 실제로 채워져 있을 리 없다(Resurrect는
    // 항상 destroy()/좀비 회수를 먼저 거친 뒤에만 같은 Process를
    // 재사용하므로, threads는 그 회수 절차에서 이미 비워져 있다 - 이
    // clear()는 순수 방어적 재확인).
    threads.clear();
    // [신규, 2026-09-18, SP-76250478 §2.1, PN-0EB2FABF] threads와 동일한
    // 이유(Resurrect 재사용) - 이전 생애에 발급된 ThreadId가 새 생애로
    // 새어 들어가면 안 된다.
    nextThreadId = 0;
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
    // [신규, 2026-09-17, PN-C39882D0] ProcessId 발급 상태 - isZombie와
    // 동일한 이유(Resurrect 재사용)로 매번 리셋. 고정 스폰 KernelService는
    // SpawnProcess 경로를 안 타 kAllocateProcessId()를 절대 안 부르므로
    // 이 두 필드는 계속 무효 상태로 남는다(process.h 문서 주석 참고).
    processId = kInvalidProcessId;
    processTableIndex = kInvalidProcessTableIndex;
    // 자원 그룹 소속(SP-245D130B §1/§4) - parent/children과 동일한
    // 이유로 매번 리셋. 실제 그룹 가입은 init() 이후 스폰 경로
    // (joinResourceGroup())가 담당 - init() 자신은 항상 "그룹 없음"
    // 상태로 되돌려 둔다.
    group = nullptr;
    frozenByGroup = false;
    // [신규, 2026-09-17, SP-B26CDBDD §6.2] group/frozenByGroup과 동일한
    // 이유 - Resurrect가 같은 정적 Process를 재사용할 수 있으므로
    // 이전 생애의 메모리 사용량이 새 생애로 새어 들어가면 안 된다.
    memoryBytesUsed = 0;
    // [신규, 2026-09-18, PN-22E5E9E7 항목5] memoryBytesUsed와 동일한
    // 이유(Resurrect 재사용) - execImage()가 매번 새로 채운다.
    tlsTemplateVaddr = 0;
    tlsTemplateFilesz = 0;
    tlsTemplateMemsz = 0;
    tlsTemplateAlign = 0;
    hasTlsTemplate = false;
    addressSpace.init(pml4Phys, kMmapRegionFloor, kMmapRegionCeil, this);
    // Resurrect(§6.2)가 같은 정적 Process를 재사용할 수 있으므로,
    // 이전 생애의 신호 상태가 새 생애로 새어 들어가지 않도록 매번
    // 명시적으로 리셋한다(pml4Phys/addressSpace와 동일한 이유).
    pendingSignals.clear();
    for (uint32_t i = 0; i < kSignalCount; ++i) {
        dispositions[i] = SignalDisposition::Default;
    }
    // [신규, SP-30FCC8AE §1/§2] pendingSignals/dispositions와 동일한
    // 이유(Resurrect 재사용) - 스폰 경로(SpawnProcessHandler/fork())가
    // init() 직후 실제 부모 uid/gid로 덮어쓴다. 부모가 없는 최초
    // 프로세스(init)/고정 스폰 KernelService는 이 root 기본값을 그대로
    // 유지한다.
    uid = kRootUid;
    gid = kRootGid;
    signalPermission = kPermOwnerWrite;
    // 프로세스 디버깅(SP-9A6D579F §3.1, PN-87D6B615) - pendingSignals와
    // 동일한 이유로 매번 리셋(Resurrect §6.2가 같은 정적 Process를
    // 재사용할 수 있으므로 이전 생애의 디버그 세션이 새 생애로 새어
    // 들어가면 안 된다).
    debugSession = DebugSession();
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
    // [신규, 2026-09-17, SP-B26CDBDD §6.2] execImage()가 가산해 둔 몫을
    // 그룹 합계에서 감산 - 다른 어떤 정리보다도 먼저(이 시점 이후로는
    // group이 바뀌지 않는다는 보장이 없으므로 가장 먼저 확실히 처리).
    if (group) {
        group->accounting.totalMemoryBytesUsed -= memoryBytesUsed;
    }
    memoryBytesUsed = 0;
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

// [PN-E35294B8 항목2, QU-B9EB45E4 답변 그대로 반영] argv 또는 envp
// 하나(유저 포인터, NULL 종단 `char* const[]`)를 검증하며 걷는다 -
// 배열 길이를 미리 모르므로 슬롯을 하나씩 `isUserRangeValid`로 확인한
// 뒤에만 읽고, 각 문자열도 남은 예산(`kMaxSpawnArgsTotalSize -
// *ioUsed`) 안에서만 페이지 경계 단위로 나눠 NUL을 찾는다(문자열
// 길이 자체도 미리 모르므로 무한정 읽지 않기 위함 - 이 검증 자체가
// 이 코드베이스에 없던 새 유형이라는 QU-B9EB45E4의 지적에 대한 답).
// 검증을 통과한 문자열은 그 자리에서 `scratch + *ioUsed`로 복사하고
// 오프셋을 `outOffsets[i]`에 남긴다(최종 유저 주소는 프레임 배치가
// 끝나야 정해지므로 지금은 스크래치 버퍼 안 상대 위치만).
bool kCopyUserStringArray(char* const* userArray, uint8_t* scratch, uint64_t* ioUsed, uint64_t* outOffsets,
                           uint32_t* outCount) {
    uint32_t count = 0;
    for (;;) {
        if (count >= kMaxSpawnArgsEntryCount) {
            return false;  // 항목 수 상한(구현 세부, process.h 문서 참고)
        }
        const auto* slot = reinterpret_cast<char* const*>(userArray) + count;
        if (!Paging::isUserRangeValid(reinterpret_cast<uint64_t>(slot), sizeof(char*))) {
            return false;
        }
        char* strPtr = *slot;
        if (!strPtr) {
            break;  // NULL 종단
        }

        const uint64_t remaining = kMaxSpawnArgsTotalSize - *ioUsed;
        uint64_t scanned = 0;
        bool foundNul = false;
        while (scanned < remaining) {
            const uint64_t addr = reinterpret_cast<uint64_t>(strPtr) + scanned;
            const uint64_t pageEnd = (addr & ~0xFFFULL) + 0x1000ULL;
            const uint64_t untilPageEnd = pageEnd - addr;
            const uint64_t chunk = untilPageEnd < (remaining - scanned) ? untilPageEnd : (remaining - scanned);
            if (!Paging::isUserRangeValid(addr, chunk)) {
                return false;
            }
            const auto* chunkPtr = reinterpret_cast<const char*>(addr);
            for (uint64_t i = 0; i < chunk; ++i) {
                if (chunkPtr[i] == '\0') {
                    scanned += i;
                    foundNul = true;
                    break;
                }
            }
            if (foundNul) {
                break;
            }
            scanned += chunk;
        }
        if (!foundNul) {
            return false;  // 예산(kMaxSpawnArgsTotalSize) 안에서 NUL을 못 찾음 - 거부
        }

        const uint64_t strBytes = scanned + 1;  // NUL 포함
        if (*ioUsed + strBytes > kMaxSpawnArgsTotalSize) {
            return false;
        }
        memcpy(scratch + *ioUsed, strPtr, strBytes);
        outOffsets[count] = *ioUsed;
        *ioUsed += strBytes;
        ++count;
    }
    *outCount = count;
    return true;
}

// [신규, PN-6D497EB0/PN-543C0CE9, SP-6BEAE0C1 §4] System V AMD64 ABI
// 관례대로 유저 스택 최상단에 argc/argv/envp(+빈 auxv)를 배치하고
// `_start` 진입 시 기대되는 초기 RSP를 계산한다.
//
// [확장, PN-E35294B8 항목2, QU-B9EB45E4 답변] `argCount`/`envCount`가
// 0이면(기존 `kEnterInitProcess`/`kSpawnServiceProcesses` 호출부)
// 예전과 완전히 동일하게 `argc=0, argv=[NULL], envp=[NULL]`만 쓴다.
// 0보다 크면(`SpawnProcessHandler`가 이미 `kCopyUserStringArray`로
// 검증+복사해 둔 문자열들) 그 문자열 데이터까지 포함한 완전한 프레임을
// 짓는다 - 전체 프레임을 커널 스크래치에 먼저 조립한 뒤(문자열이 여러
// 물리 페이지에 걸칠 수 있어 한 번에 못 쓰므로), 대상 스택의 이미
// 매핑된 물리 페이지들에 페이지 경계마다 나눠 복사한다(PN-E35294B8
// 계획 문서가 미리 요구해 둔 방식 그대로 - elf 로더의 세그먼트 복사와
// 같은 결). 프레임 전체가 `kUserStackSize`(64KiB, 이미 execImage()가
// 전부 매핑해 둔 범위)를 넘으면 실패 - 새 매핑을 만들지 않는다.
bool kSetupInitialUserStack(uint64_t stackTop, uint64_t pml4Phys, const uint8_t* stringsData, uint64_t stringsSize,
                             const uint64_t* argOffsets, uint32_t argCount, const uint64_t* envOffsets,
                             uint32_t envCount, uint64_t* outInitialRsp) {
    const uint64_t argvPtrsSize = static_cast<uint64_t>(argCount + 1) * 8;  // +1 = NULL 종단
    const uint64_t envpPtrsSize = static_cast<uint64_t>(envCount + 1) * 8;
    constexpr uint64_t kAuxvSize = 16;  // AT_NULL 값+타입(이 커널은 아직 aux 벡터 항목을 안 만듦)
    constexpr uint64_t kArgcSize = 8;
    const uint64_t stringsAligned = (stringsSize + 7) & ~7ULL;

    uint64_t total = kArgcSize + argvPtrsSize + envpPtrsSize + kAuxvSize + stringsAligned;
    total = (total + 15) & ~15ULL;  // _start 진입 RSP는 16-정렬이어야 함(SysV 관례)
    if (total > kUserStackSize) {
        return false;  // 호출부가 ArgsTooLarge로 보고
    }

    void* frameMem = GenericSlabAllocator::alloc(total);
    if (!frameMem) {
        return false;  // 호출부가 OutOfMemory로 보고(QU-B9EB45E4 "할당 실패해도 거부")
    }
    auto* frame = reinterpret_cast<uint8_t*>(frameMem);
    memset(frame, 0, total);

    const uint64_t frameBaseUserAddr = stackTop - total;
    const uint64_t stringsFrameOffset = total - stringsSize;
    if (stringsSize) {
        memcpy(frame + stringsFrameOffset, stringsData, stringsSize);
    }
    auto stringUserAddr = [&](uint64_t offsetInStrings) { return frameBaseUserAddr + stringsFrameOffset + offsetInStrings; };

    uint64_t pos = 0;
    auto writeU64 = [&](uint64_t value) {
        *reinterpret_cast<uint64_t*>(frame + pos) = value;
        pos += 8;
    };
    writeU64(argCount);
    for (uint32_t i = 0; i < argCount; ++i) {
        writeU64(stringUserAddr(argOffsets[i]));
    }
    writeU64(0);  // argv[] NULL 종단
    for (uint32_t i = 0; i < envCount; ++i) {
        writeU64(stringUserAddr(envOffsets[i]));
    }
    writeU64(0);  // envp[] NULL 종단
    writeU64(0);  // auxv: AT_NULL 타입
    writeU64(0);  // auxv: AT_NULL 값

    uint64_t copied = 0;
    while (copied < total) {
        const uint64_t destUserAddr = frameBaseUserAddr + copied;
        const uint64_t pageAddr = destUserAddr & ~0xFFFULL;
        const uint64_t phys = Paging::translatePage(pageAddr, pml4Phys);
        if (!phys) {
            GenericSlabAllocator::free(frameMem, total);
            return false;  // execImage()가 이 범위를 이미 전부 매핑해 뒀어야 함 - 실패하면 호출부 버그
        }
        auto* pageVirt = reinterpret_cast<uint8_t*>(kPhysToVirt(phys));
        const uint64_t offsetInPage = destUserAddr - pageAddr;
        const uint64_t untilPageEnd = 4096UL - offsetInPage;
        const uint64_t chunk = untilPageEnd < (total - copied) ? untilPageEnd : (total - copied);
        memcpy(pageVirt + offsetInPage, frame + copied, chunk);
        copied += chunk;
    }
    GenericSlabAllocator::free(frameMem, total);

    *outInitialRsp = frameBaseUserAddr;
    return true;
}

// [신규, 2026-09-18, PN-22E5E9E7 항목6, SP-29D652AA §5.2] 이 프로세스의
// PT_TLS 템플릿(항목5)이 있으면 그 프로세스 주소공간 안에 `thread` 전용
// TLS 인스턴스를 만든다 - x86_64 TLS variant II 레이아웃, task.cpp의
// `kMakeTaskTlsBlock()`(커널 쪽 Task TCB)과 정확히 같은 이유로 템플릿
// 복사본 바로 뒤에 8바이트 self-pointer 헤더를 붙인다: `-ftls-model=
// local-exec`라도 extern thread_local(여러 TU에서 접근)에는 Itanium
// C++ ABI가 강제하는 TLS 래퍼 함수가 모델과 무관하게 항상 FS:0을
// self-pointer로 역참조한다는 것을 커널 쪽에서 실측으로 확인했다
// (task.cpp 문서 주석 참고) - 유저랜드 자체는 아직 그 패턴을 촉발하는
// 소비자(여러 TU에서 접근하는 extern thread_local)가 없어 독립적으로
// 재확인하지 못했지만, 8바이트 비용이 미미해 선제적으로 같은 방어를
// 넣는다.
//
// v1은 템플릿+헤더가 한 페이지(4KiB) 안에 들어간다고 가정한다 -
// `tlsTemplateVaddr`는 `userland/cmake/linker-userland-x86_64.ld`의
// `.tdata ALIGN(4K)` 덕에 항상 페이지 경계에서 시작하고(항목5 문서
// 주석의 정렬 함정 참고), `mapRegion()`이 고르는 목적지 주소도 항상
// 페이지 경계다 - 그래서 각각 `Paging::translatePage()` 한 번으로 전체
// 템플릿/인스턴스를 담은 물리 페이지를 얻을 수 있다. 4KiB를 넘는
// thread_local 총량이 실제로 필요해지면 이 함수를 페이지 단위 다중
// 청크 복사로 확장해야 한다(RM-23F4B687 §4 원칙, 실측 후 조정).
//
// [PN-71C3D483 항목 3] `mapRegion()`으로 만든 VMA는 `addressSpace`의
// 장부에 등록되므로 `Process::destroy()`의 `addressSpace.unmapAll()`이
// 이 인스턴스도 자동으로 반납한다 - v1(멀티스레딩 미구현, 프로세스당
// UserThread 하나)에는 이걸로 충분하다. 나중에 UserThread를 프로세스
// 생존 중에 개별적으로 종료하는 경로가 생기면(PN-543C0CE9) 그 경로가
// 이 함수가 만든 영역을 개별적으로 `addressSpace.unmapRegion()`해야
// 한다 - 지금은 그런 경로 자체가 없어 구현하지 않는다.
bool Process::makeUserTlsInstance(UserThread* thread) {
    if (!hasTlsTemplate) {
        return true;
    }
    constexpr uint64_t kTcbHeaderSize = 8;
    const uint64_t allocSize = tlsTemplateMemsz + kTcbHeaderSize;
    if (allocSize > 4096UL) {
        return false;  // v1 한계 초과 - 위 문서 주석의 다중 페이지 확장 필요
    }

    uint64_t regionAddr = 0;
    if (!addressSpace.mapRegion(allocSize, PAGE_WRITABLE, VmaBacking::Anonymous, 0, &regionAddr)) {
        return false;
    }

    const uint64_t dstPhys = Paging::translatePage(regionAddr, pml4Phys);
    auto* dst = reinterpret_cast<uint8_t*>(kPhysToVirt(dstPhys));
    memset(dst, 0, allocSize);

    if (tlsTemplateFilesz > 0) {
        // 원본 .tdata 바이트는 이미 elf::loadIntoAddressSpace()가
        // PT_LOAD(:udata)의 일부로 이 프로세스 자신의 주소공간에 매핑해
        // 뒀다(linker-userland-x86_64.ld가 .tdata/.tbss를 :udata에도
        // 이중 소속시킴, 항목5 참고) - 원본 ELF 버퍼를 다시 참조할
        // 필요 없이 이미 매핑된 가상주소를 통해 그대로 읽는다.
        const uint64_t srcPhys = Paging::translatePage(tlsTemplateVaddr, pml4Phys);
        const auto* src = reinterpret_cast<const uint8_t*>(kPhysToVirt(srcPhys));
        memcpy(dst, src, tlsTemplateFilesz);
    }

    const uint64_t fsBase = regionAddr + tlsTemplateMemsz;
    *reinterpret_cast<uint64_t*>(dst + tlsTemplateMemsz) = fsBase;  // FS:0 self-pointer
    thread->userFsBase = fsBase;
    return true;
}

UserThread* Process::execImage(const elf::Image& image, UserThread* thread, const uint8_t* argvEnvpScratch,
                                uint64_t stringsSize, const uint64_t* argOffsets, uint32_t argCount,
                                const uint64_t* envOffsets, uint32_t envCount) {
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
    uint64_t initialRsp = kUserStackTop;
    if (!kSetupInitialUserStack(kUserStackTop, pml4Phys, argvEnvpScratch, stringsSize, argOffsets, argCount,
                                 envOffsets, envCount, &initialRsp)) {
        return nullptr;
    }
    thread->ring3UserStackTop = initialRsp;
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
    // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] `mainThread = thread;`
    // (raw 포인터 단일 대입)를 대체 - `threads`(process.h)에 이
    // 스레드의 `sharedSelf()`(방금 ensureSelfRef()로 반드시 채워진
    // `_selfRef`와 컨트롤 블록을 공유)를 등록한다. 실패(슬랩 고갈)해도
    // 이 함수 자체를 실패시키지 않는다 - `thread`는 여전히 유효하고
    // 호출부가 그대로 enqueue할 수 있다(ensureSelfRef() 실패를 이미
    // 같은 이유로 무시하는 바로 위 관례와 동일), 다만 이 경우 그
    // 스레드는 `Process::threads`로 관찰되지 않는다(프로세스 조회/
    // 좀비 회수 등 threads를 순회하는 코드에서 안 보임 - v1은 항상
    // 이 execImage() 호출부(SpawnProcessHandler/kSpawnInitProcess/
    // kSpawnServiceProcesses)가 곧바로 첫 스레드를 만드는 것이라 이
    // 슬랩 고갈 시나리오 자체가 이미 다른 이유로 실패하는 경로들과
    // 같은 급의 드문 경우).
    // [신규, 2026-09-18, SP-76250478 §2.1, PN-0EB2FABF] 이 프로세스의
    // 최초 스레드도 `CreateThread`(process.cpp)가 만드는 스레드들과
    // 같은 id 공간을 공유한다 - 항상 이 함수가 가장 먼저 불리므로
    // 사실상 항상 0.
    thread->threadId = nextThreadId++;
    threads.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    threads.insert(thread->sharedSelf());

    // [신규, 2026-09-17, SP-B26CDBDD §6.2, PN-158B6B2F] 메모리 사용량
    // coarse 계정 - PT_LOAD 세그먼트 memsz 합 + 유저 스택 크기(위에서
    // 이미 매핑 완료). 모든 execImage() 호출부(SpawnProcessHandler/
    // kSpawnInitProcess/kSpawnServiceProcesses) 전부가 여기 하나로
    // 자동 커버된다 - 설계 문서(§6.2)는 SpawnProcessHandler::onExec
    // 쪽에 붙이는 스케치였으나, 세 호출부가 전부 이 함수를 거치므로
    // 여기 한 곳에 두는 편이 중복 없이 더 확실하다(구현 세부 판단).
    uint64_t imageBytes = kUserStackSize;
    for (uint32_t i = 0; i < image.segmentCount(); ++i) {
        const elf::ProgramHeader seg = image.segment(i);
        if (seg.type == elf::kSegmentTypeLoad) {
            imageBytes += seg.memsz;
        } else if (seg.type == elf::kSegmentTypeTls) {
            // [신규, 2026-09-18, PN-22E5E9E7 항목5] 파싱/저장 - 실제
            // 인스턴스 생성은 아래 makeUserTlsInstance() 호출(항목6),
            // FS_BASE MSR 배선은 여전히 항목7(아직 미구현).
            tlsTemplateVaddr = seg.vaddr;
            tlsTemplateFilesz = seg.filesz;
            tlsTemplateMemsz = seg.memsz;
            tlsTemplateAlign = seg.align;
            hasTlsTemplate = true;
        }
    }
    memoryBytesUsed += imageBytes;
    if (group) {
        group->accounting.totalMemoryBytesUsed += imageBytes;
    }

    // [신규, 2026-09-18, PN-22E5E9E7 항목6] 위 스캔이 hasTlsTemplate를
    // 채운 뒤에만 호출 가능 - 템플릿이 없으면(v1 유저 바이너리 전부
    // 해당) 즉시 true라 사실상 no-op.
    if (!makeUserTlsInstance(thread)) {
        // [신규, 2026-09-18, SP-76250478, PN-0EB2FABF] 이 함수의 기존
        // 실패 계약("호출부가 UserThread::release(thread)+Process::
        // destroy()로 되돌린다")은 안 바뀐다 - 다만 방금 위에서
        // `threads`에 이 thread를 이미 등록해 뒀으므로, 그 계약대로
        // 호출부가 곧장 release()를 부르기 전에 여기서 먼저 컨테이너
        // 슬롯을 지워 둬야 한다(process.h의 threads 문서 주석 "순서
        // 중요" 참고 - 그러지 않으면 호출부의 release() 이후
        // `threads`가 이미 반납된 메모리를 가리키는 채로 남고, 그
        // 컨테이너의 청크 메모리 자체도 Process 소멸 시 반납될 기회를
        // 못 만난다, Process::destroy()는 threads를 안 건드리므로).
        threads.clear();
        return nullptr;
    }

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
    // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] 옛 `if (mainThread)`
    // 단일 분기를 `threads` 전체 순회로 대체 - 지금은 프로세스당
    // 스레드가 여전히 하나뿐이라 관찰 가능한 동작은 동일하다(process.h의
    // raiseSignal() 문서 주석에 POSIX 시맨틱 미정 사항을 기록해 뒀다 -
    // 이 순회는 "대기 중인 모든 스레드를 깨운다"는 보수적 동작).
    threads.forEach([&](SharedPtr<UserThread>& threadRef, auto*) {
        UserThread* t = threadRef.get();
        if (!t) {
            return;
        }
        if (SharedPtr<Waitable> waitable = t->blockedOn.lock()) {
            waitable->cancel(t, WaitCancelReason::Signal);
        }
        // [신규, 2026-09-18, PN-B5C2845A] Kill/Terminate는 위
        // `blockedOn`(Waitable 기반 블로킹) 강제 웨이크업만으로는
        // 대상이 `Syscall::wait()`(`acceptFromChannel`/
        // `connectChannel`/`ChannelRead`/`ChannelWrite` 등)로 파킹된
        // 경우에 절대 도달하지 못한다(PN-B5C2845A 발견 - `Scheduler::
        // parkCurrent()`는 `blockedOn`을 전혀 안 씀). Kill/Terminate
        // 둘 다(§4.4 체크포인트/kCheckSignalCheckpoint와 동일한 판정
        // 대상) 이 경로도 함께 켠다 - `Scheduler::cancelPendingSyscalls`
        // 가 아직 안 끝난 pendingSyscalls 항목을 `Cancelled`로 전이시켜
        // 깨우고(waitForAnyOf가 이제 `Cancelled`도 인식, syscall.cpp
        // 참고), 각 핸들러의 `onCancel()`이 `args->error`를 채운다
        // (설계자 답변 "얘들을 실패시키면 되잖아", QU-8E137FFD) - 대상이
        // 실제로 파킹돼 있지 않으면(pendingSyscalls가 비어있거나 전부
        // 이미 끝남) 이 호출은 그냥 아무 일도 안 하는 것과 같다.
        if (number == SignalNumber::Kill || number == SignalNumber::Terminate) {
            Scheduler::cancelPendingSyscalls(t);
        }
    });
    return true;
}

void Process::joinResourceGroup(ResourceGroup* newGroup) {
    if (group == newGroup) {
        return;
    }
    if (group) {
        group->removeMember(this);
    }
    group = newGroup;
    if (group) {
        group->addMember(weakFromThis());
    }
}

namespace {

// [신규, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2] ProcessId의 세대
// 태그 슬롯 테이블 - channel.cpp의 `ChannelTableSlot`/`gChannelTable`과
// 동일한 패턴이되, `Process`는 raw 슬랩 포인터가 아니라 SharedPtr로
// 관리되므로(PN-E2A114C1) `Channel*==nullptr` 대신 `WeakPtr<Process>::
// lock()` 실패로 생존을 판정한다(Process* 역참조 없이 판정, SP-9CB55C5B
// §2 그대로). `WeakPtr<T>`에는 `operator bool()`이 없어(shared_ptr.h
// 확인) "슬롯이 비었는가"는 `!slot.proc.lock()`으로 판정한다 - 기본
// 생성된 빈 WeakPtr도 내부 컨트롤 블록이 nullptr이라 `lock()`이
// 안전하게 빈 SharedPtr을 반환하므로, "한 번도 발급 안 된 슬롯"과
// "발급됐다가 대상이 죽었는데 아직 명시적으로 안 비워진 슬롯"을 굳이
// 구분할 필요가 없다(둘 다 안전하게 재사용 가능 - 방어적으로도 유리).
struct ProcessTableSlot {
    WeakPtr<Process> proc;
    uint32_t generation = 0;
};

ProcessTableSlot gProcessTable[kMaxProcessTableSlots];
// [수정, 2026-09-17, PN-AA30E4C8, QU-68D76FC4/QU-E847DB03(SP-9F1DB1D8)
// 답변과 동일한 패턴 재사용] 원래 plain Spinlock은 "발급/해제(쓰기)만
// 보호하고 조회(kResolveProcessId, 읽기)는 락 없이 인덱스+세대 비교
// + lock()만 한다"는 주석이 있었으나, 이건 설계가 아니라 실재하는
// 데이터 경쟁이었다 - 다른 코어가 kAllocateProcessId()/kFreeProcessId()
// 로 이 슬롯의 generation/proc을 갱신하는 도중(둘 다 원자적으로 함께
// 바뀌지 않음) 락 없이 읽으면 찢긴(torn) generation+proc 조합을 볼 수
// 있다. 등록/해제는 드물고 조회는 훨씬 잦은 패턴이라(SP-9CB55C5B §5가
// 이미 지적) Scheduler::gCurrentTaskLock(SP-9F1DB1D8)과 정확히 같은
// "쓰기 배타적/읽기는 카운터만 증분" RwSpinlock으로 교체한다.
RwSpinlock gProcessTableLock;

// 발급 - `proc`을 위한 새 슬롯을 확보하고 그 슬롯의 인덱스를
// `proc->processTableIndex`에 되먹여 저장한 뒤 인코딩된 ProcessId를
// 반환한다(실패 시 kInvalidProcessId, proc은 건드리지 않음).
ProcessId kAllocateProcessId(const SharedPtr<Process>& proc) {
    RwSpinlockWriteGuard guard(gProcessTableLock);
    for (uint32_t i = 0; i < kMaxProcessTableSlots; ++i) {
        if (!gProcessTable[i].proc.lock()) {
            gProcessTable[i].generation++;
            gProcessTable[i].proc = WeakPtr<Process>(proc);
            proc->processTableIndex = i;
            return (static_cast<ProcessId>(gProcessTable[i].generation) << 32) | i;
        }
    }
    return kInvalidProcessId;  // 슬롯 고갈(동시 생존 UINT16_MAX개 초과)
}

// 안전 해석 - 유저가 넘긴 pid가 무엇이든 인덱스 범위 검사 + generation
// 일치 확인 + `WeakPtr::lock()`만으로 끝난다(`reinterpret_cast<Process*>`
// 를 단 한 번도 쓰지 않음). **[수정, 2026-09-18, PN-88E62419]** 첫
// 실사용처 배선 완료 - `KillHandler::onExec`가 이제 이 함수로 임의
// 대상을 해석한다(SP-9CB55C5B §3/§4). **[수정, 2026-09-17,
// PN-AA30E4C8]** 읽기 락(카운터만 증분, 다른 읽기와 동시 진행 가능) -
// 위 `gProcessTableLock` 문서 주석 참고.
SharedPtr<Process> kResolveProcessId(ProcessId pid) {
    if (pid == kInvalidProcessId) {
        return {};
    }
    const uint32_t index = static_cast<uint32_t>(pid & 0xFFFFFFFFLL);
    const uint32_t generation = static_cast<uint32_t>(pid >> 32);
    if (index >= kMaxProcessTableSlots) {
        return {};
    }
    RwSpinlockReadGuard guard(gProcessTableLock);
    ProcessTableSlot& slot = gProcessTable[index];
    if (slot.generation != generation) {
        return {};
    }
    return slot.proc.lock();
}

// 해제 - 좀비가 부모에게 Wait으로 회수(reap)되는 시점에 O(1)로 슬롯을
// 비운다(generation은 그대로 - 다음 재사용 때 +1). 애초에 발급받은 적
// 없는 프로세스(고정 스폰 KernelService 등, processTableIndex ==
// kInvalidProcessTableIndex)를 넘기면 안전하게 아무 일도 하지 않는다.
void kFreeProcessId(uint32_t processTableIndex) {
    if (processTableIndex >= kMaxProcessTableSlots) {
        return;
    }
    RwSpinlockWriteGuard guard(gProcessTableLock);
    gProcessTable[processTableIndex].proc = WeakPtr<Process>();
}

// [SP-6BEAE0C1 §3, PN-543C0CE9 착수 4번째 증분] SpawnProcess 본체 -
// 앞선 세 증분(PageFrameAllocator::retain/refCount, Process::
// allocate()/UserThread::allocate(), Paging::isUserRangeValid)을 실제로
// 엮는다. ChannelReadHandler/ChannelWriteHandler와 완전히 같은 관례
// (AsyncTaskHandler 하나 = syscall 엔드포인트 하나, args를 그 자리에서
// 직접 채워 co_return).
class SpawnProcessHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SpawnProcessArgs*>(argsRaw);

        // 0단계 - [PN-A6E01B8A, QU-9585F6C4] flags 유효성 검증 - 정의
        // 안 된 비트가 하나라도 세팅되면 조용히 무시하지 않고 거부한다
        // (SP-6BEAE0C1 §3 "오타/버전 불일치를 바로 드러내기 위함" -
        // 표준 커널 syscall 관례). kSpawnDebugStart 비트 자체의 실제
        // 동작(자식을 Blocked로 시작)은 아래 5단계 끝에서 소비한다
        // ([완료, PN-87D6B615 항목8, SP-9A6D579F §3.3]).
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

        // 3.5단계 - [신규, PN-E35294B8 항목2, QU-B9EB45E4 답변] argv/envp
        // 유저 포인터 배열을 검증+복사한다. 오프셋 추적 배열
        // (kMaxSpawnArgsEntryCount*8바이트, 최대 32KiB)까지 스택에
        // 두면 이 코루틴의 전용 스택(kAsyncTaskStackSize=4KiB, PN-D01B7D07)
        // 을 즉시 넘치므로 반드시 Slab에 둔다 - args->argv/envp 둘 다
        // nullptr이면(호출자가 인자 없이 스폰) 상한 검사 자체를 생략하고
        // 항목 수 0으로 그대로 진행(execImage()가 기존과 동일한 빈
        // 프레임을 만든다 - kEnterInitProcess/kSpawnServiceProcesses와
        // 동일한 결과).
        constexpr uint64_t kOffsetsBytes = static_cast<uint64_t>(kMaxSpawnArgsEntryCount) * sizeof(uint64_t);
        uint8_t* argsScratch = nullptr;
        uint64_t* argOffsets = nullptr;
        uint64_t* envOffsets = nullptr;
        uint64_t stringsUsed = 0;
        uint32_t argCount = 0;
        uint32_t envCount = 0;
        bool argsOk = true;

        if (args->argv || args->envp) {
            argsScratch = static_cast<uint8_t*>(GenericSlabAllocator::alloc(kMaxSpawnArgsTotalSize));
            argOffsets = static_cast<uint64_t*>(GenericSlabAllocator::alloc(kOffsetsBytes));
            envOffsets = static_cast<uint64_t*>(GenericSlabAllocator::alloc(kOffsetsBytes));
            if (!argsScratch || !argOffsets || !envOffsets) {
                argsOk = false;
                args->error = SpawnProcessError::OutOfMemory;  // QU-B9EB45E4 "할당 실패해도 거부"
            } else if (args->argv &&
                       !kCopyUserStringArray(args->argv, argsScratch, &stringsUsed, argOffsets, &argCount)) {
                argsOk = false;
                args->error = SpawnProcessError::ArgsTooLarge;
            } else if (args->envp &&
                       !kCopyUserStringArray(args->envp, argsScratch, &stringsUsed, envOffsets, &envCount)) {
                argsOk = false;
                args->error = SpawnProcessError::ArgsTooLarge;
            }
        }

        if (!argsOk) {
            if (argsScratch) {
                GenericSlabAllocator::free(argsScratch, kMaxSpawnArgsTotalSize);
            }
            if (argOffsets) {
                GenericSlabAllocator::free(argOffsets, kOffsetsBytes);
            }
            if (envOffsets) {
                GenericSlabAllocator::free(envOffsets, kOffsetsBytes);
            }
            UserThread::release(thread);
            procShared.reset();
            GenericSlabAllocator::free(kernelImage, args->imageSize);
            co_return;  // args->error는 위에서 이미 채움
        }

        // 4단계 - kEnterInitProcess/kSpawnServiceProcesses와 동일한
        // execImage 경로. **elf::Image는 원본 버퍼를 복사하지 않고
        // 그대로 가리키므로(elf.h 문서 주석) kernelImage는 execImage가
        // 끝난 뒤에만 반납한다** - loadIntoAddressSpace가 이 버퍼에서
        // 새 주소공간으로 실제 페이지 복사를 끝내는 지점이 execImage
        // 안이다. argsScratch/argOffsets/envOffsets도 마찬가지로
        // execImage()가 그 안에서 kSetupInitialUserStack으로 다 읽어
        // 프레임을 짓고 난 뒤에만 반납한다.
        UserThread* started =
            procShared->execImage(image, thread, argsScratch, stringsUsed, argOffsets, argCount, envOffsets, envCount);
        GenericSlabAllocator::free(kernelImage, args->imageSize);
        if (argsScratch) {
            GenericSlabAllocator::free(argsScratch, kMaxSpawnArgsTotalSize);
        }
        if (argOffsets) {
            GenericSlabAllocator::free(argOffsets, kOffsetsBytes);
        }
        if (envOffsets) {
            GenericSlabAllocator::free(envOffsets, kOffsetsBytes);
        }

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
        //
        // [수정, 2026-09-17, PN-5BBD4301] `Scheduler::currentTask()`에서
        // `task->submitterTask.lock()`으로 전환 - RM-23F4B687이 이미 네
        // 번 문서화한 "onExec() 안에서 Scheduler::currentTask()를 믿으면
        // 안 된다" 함정의 다섯 번째 재발(실측으로 발견 - PN-5BBD4301
        // 참고). `Syscall::wait()`는 항상 호출자를 실제로 파킹시키므로
        // (syscall.cpp) 이 AsyncTask가 나중에 idle 드레인으로 처음
        // 실행될 때 `Scheduler::currentTask()`는 이미 nullptr(코어가
        // 진짜로 idle)이지, 제출자가 절대 아니다 - Channel/Pnp 핸들러가
        // 처음부터 쓰던 `task->submitterTask.lock()`이 유일하게 안전한
        // 방법이다.
        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
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
        // [신규, 2026-09-17, SP-245D130B §1] 자원 그룹 소속 - 명시적으로
        // "다른 그룹으로 가입" 하는 syscall이 아직 없으므로(§8 후속),
        // 부모의 그룹을 그대로 물려받는다(Linux의 새 프로세스가 부모의
        // cgroup을 상속하는 것과 동일한 기본값 - 이 문서가 열어 둔
        // "동적 그룹 생성/명시적 가입" 자체가 아직 없어 실제로는 항상
        // parentProc->group도 gRootResourceGroup으로 귀결된다, 그래도
        // 나중에 동적 그룹이 생겼을 때 자연히 맞물리도록 상속 방식으로
        // 미리 짜 둔다). parentProc이 없으면(도달 불가 방어 경로) 그냥
        // 루트로.
        procShared->joinResourceGroup(parentProc ? (parentProc->group ? parentProc->group : &gRootResourceGroup)
                                                  : &gRootResourceGroup);
        // [신규, SP-30FCC8AE §1] uid/gid 상속 - group 상속과 동일한
        // 패턴/이유(승격 경로가 아직 없어 부모 값을 그대로 물려받는
        // 것 외엔 선택지가 없다). parentProc이 없으면(도달 불가 방어
        // 경로) init()이 이미 세팅해 둔 root 기본값 그대로 둔다.
        if (parentProc) {
            procShared->uid = parentProc->uid;
            procShared->gid = parentProc->gid;
        }
        // parentProc이 이 시점에도 비어 있으면(진짜 root조차 없는 -
        // init도 아직 스폰 안 된 부팅 극초반) 정말 아무도 소유할 수
        // 없다 - 이 역시 이론상 도달 불가(SpawnProcess 자체가 유저
        // 프로세스 실행 흐름에서만 오므로 이미 최소 init은 떠 있어야
        // 함)라 더 방어하지 않는다.

        // argv/envp는 아직 실제로 전달하지 않는다(SpawnProcessArgs
        // 문서 주석 참고 - §4 전체가 미착수).
        //
        // [신규, PN-87D6B615 항목8, SP-9A6D579F §3.3] `kSpawnDebugStart`
        // 비트가 세팅됐으면 이 자식은 첫 명령어를 실행하기 전에 정지
        // 상태로 시작해야 한다(설계자 지시로 옵션이 아님) - Ready
        // 큐에 아예 넣지 않고 `state`만 `Blocked`로 남겨 둔다.
        // `Scheduler::parkCurrent()`(scheduler.cpp)가 파킹을 표현하는
        // 것과 정확히 같은 방식(state=Blocked + 어느 큐에도 없음,
        // `inRunQueue`는 애초에 한 번도 true가 된 적 없으므로 그대로
        // false) - 새 `TaskState`를 추가하지 않는다(RM-23F4B687 §4,
        // 과설계 방지). 나중에 `DebugContinue`(§3.5, 아직 미구현,
        // PN-87D6B615 항목5)가 `Scheduler::enqueue()`로 명시적으로
        // Ready에 올려야만 실행을 시작한다.
        // [신규, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2/§6] ProcessId
        // 발급 - 이 지점 이전엔 아직 실패 가능한 단계(argv/envp 복사,
        // execImage, children.insert())가 남아 있었으나 전부 통과했다 -
        // 원래 코드도 이 지점부터는 더 이상 실패 분기가 없었으므로(항상
        // 성공으로 co_return) 같은 "이 이후엔 실패 없음" 전제를 그대로
        // 따른다. 슬롯 고갈(UINT16_MAX개 동시 생존 프로세스 초과, 이
        // 커널 규모에서 사실상 도달 불가)이라는 극히 드문 경우에도
        // 이미 시작된 프로세스를 되돌리는 것보다(스레드가 곧 Ready
        // 큐에 오르거나 디버그 정지 상태가 되므로, 그 시점 이후 되돌림은
        // children.insert() 등 앞선 단계까지 전부 되감아야 해 원래 코드에
        // 없던 복잡한 unwind를 새로 만들어야 함) `pid`만 무효로 남기고
        // 스폰 자체는 성공시키는 쪽을 택한다(POSIX에 없는 실패 모드를
        // 새로 만들지 않음, RM-23F4B687 §4) - `-1`로도 여전히 `Wait(-1,
        // ...)`(아무 자식이나)로는 회수 가능하다.
        procShared->processId = kAllocateProcessId(procShared);

        if (args->flags & SpawnProcessFlags::kSpawnDebugStart) {
            started->state = TaskState::Blocked;
            // [신규, 2026-09-17, SP-245D130B §9-4 답변] 이 정지도
            // "디버그 사유"로 표시해 둔다 - 지금 당장은 이 자식이
            // ResourceGroup::thaw()의 대상이 될 수 없어(한 번도
            // Running이었던 적이 없어 frozenByGroup이 절대 안 세워짐,
            // resource_group.cpp 참고) 실질적 효과는 없지만, `pausedByDebugger`
            // 가 "이 Task가 지금 디버그 사유로 멈춰 있다"를 항상
            // 정확히 반영해야 한다는 불변조건을 처음부터 지킨다.
            procShared->debugSession.pausedByDebugger = true;
        } else {
            Scheduler::enqueue(Scheduler::currentCoreIndex(), started);
        }
        // [수정, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2/§6, QU-AB5247DD
        // 승인] 예전엔 `reinterpret_cast<int64_t>(procShared.get())`
        // (포인터값 그 자체)였다 - 이제 커널이 발급한 불투명 핸들이라
        // 유저는 이 값으로 어떤 Process도 직접 역참조할 수 없다.
        args->pid = procShared->processId;
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
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<WaitArgs*>(argsRaw);
        args->hadZombieChild = false;
        args->reapedPid = kInvalidProcessId;
        args->exitCode = 0;
        args->hasAnyChild = false;

        // SpawnProcessHandler::onExec과 동일한 관례(및 동일한 수정,
        // PN-5BBD4301 참고)로 호출자 자신의 Process를 얻는다 - 커널
        // Task(process가 비어 있음)가 이 syscall을 부를 일은 없다
        // (SelfTerminateHandler와 동일한 전제), 방어적으로만 확인.
        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
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
            // [수정, 2026-09-17, PN-C39882D0] 예전엔 포인터값을 직접
            // 비교(`reinterpret_cast<int64_t>(child.get())`)했으나, 이제
            // `SpawnProcess`가 발급한 `ProcessId`(child->processId)와
            // 비교한다 - 스코프(직계 자식만 순회)는 그대로, 비교 기준
            // 값의 형식만 바뀌었다.
            if (args->targetPid != kInvalidProcessId && child->processId != args->targetPid) {
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
            args->reapedPid = zombie->processId;  // [수정, PN-C39882D0] 포인터값 대신 ProcessId
            args->exitCode = zombie->exitCode;
            // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] 옛
            // `if (zombie->mainThread) UserThread::release(...)` 단일
            // 반납을 `zombie->threads` 전체 순회로 대체 - 슬랩 반납
            // 자체는 여전히 raw 포인터 기반 `UserThread::release()`가
            // 명시적으로 담당한다(syscall.h/process.h의 threads 문서
            // 주석 참고 - SharedPtr 컨테이너로 바뀌었어도 실제 반납
            // 계약은 그대로). **순서 중요**: `threads.clear()`를
            // release() 호출보다 먼저 실행하면 안 된다 - 그러면 컨테이너
            // 자신의 SharedPtr 사본이 먼저 사라지는 건 무해하지만(no-op
            // 삭제자), 아래 forEach 자체가 이미 비워진 목록을 순회하게
            // 돼 release()가 한 번도 안 불린다. 그래서 먼저 forEach로
            // 전부 release()한 뒤에 clear()로 컨테이너 슬롯을 정리한다.
            zombie->threads.forEach([](SharedPtr<UserThread>& threadRef, auto*) {
                if (UserThread* t = threadRef.get()) {
                    UserThread::release(t);
                }
            });
            zombie->threads.clear();
            // [신규, 2026-09-17, PN-C39882D0, SP-9CB55C5B §2] 좀비가
            // 지금 여기서 실제로 회수(reap)되므로, 그 ProcessId 슬롯도
            // 지금 해제한다(위 zombie->processId를 이미 읽은 뒤라 순서
            // 무관 - kFreeProcessId는 인덱스만 지운다). kInvalidProcessTableIndex
            // (이론상 도달 불가 - 이 zombie는 반드시 SpawnProcess를
            // 거쳐 만들어졌으므로 항상 유효한 인덱스를 가짐)를 넘겨도
            // kFreeProcessId 자체가 방어적으로 무시한다.
            kFreeProcessId(zombie->processTableIndex);
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

// [신규, 2026-09-18, SP-76250478 §2.2, PN-0EB2FABF] CreateThread 본체 -
// 호출자와 같은 프로세스 안에 스레드를 추가한다(process.h의
// CreateThreadArgs/kEnterRing3Thread 문서 주석 참고). SpawnProcess와
// 근본적으로 다른 점: 새 주소공간을 만들지 않고 호출자의 기존
// `pml4Phys`를 그대로 공유하며, 스택 하나만 새로 확보한다.
class CreateThreadHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<CreateThreadArgs*>(argsRaw);

        // WaitHandler::onExec과 동일한 관례(PN-5BBD4301) - onExec() 안에서
        // Scheduler::currentTask()를 직접 쓰지 않는다.
        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> proc = caller ? caller->process.lock() : SharedPtr<Process>();
        if (!proc) {
            args->error = CreateThreadError::InvalidArgument;
            co_return;
        }

        // entry는 유저 포인터 - 최소 1바이트라도 유저 범위 안에 있어야
        // 한다(SpawnProcessArgs::imageBuffer 검증과 동일한 관례). 진짜
        // 실행 가능한 코드인지까지는 검증하지 않는다 - execImage()의
        // ELF 세그먼트 검증과 달리, 이미 실행 중인 프로세스 자신의
        // 임의 함수 포인터를 받으므로 잘못된 값이면 유저 스스로 곧
        // #PF로 대가를 치른다(기존 폴트 처리 경로가 그대로 잡음).
        if (args->entry == 0 || !Paging::isUserRangeValid(args->entry, 1)) {
            args->error = CreateThreadError::InvalidArgument;
            co_return;
        }

        // [설계자 opinion, SP-76250478 §2.1] kMaxThreadsPerProcess
        // 초과 시 단순 거부 - ChunkedList에 size()가 없어 forEach로
        // 직접 센다(청크 개수가 적어 실용적으로 무해한 선형 비용).
        uint32_t threadCount = 0;
        proc->threads.forEach([&threadCount](SharedPtr<UserThread>&, auto*) { ++threadCount; });
        if (threadCount >= kMaxThreadsPerProcess) {
            args->error = CreateThreadError::TooManyThreads;
            co_return;
        }

        UserThread* thread = UserThread::allocate();
        if (!thread) {
            args->error = CreateThreadError::OutOfMemory;
            co_return;
        }

        // [알려진 한계, process.h 문서 주석 참고] ProcessAddressSpaceManager
        // 는 최대 8개 VMA만 지원(address_space.h 클래스 문서) - 이
        // mapRegion() 호출이 그 슬롯을 하나 더 소비한다. stackSize를
        // 4KiB로 직접 올림해 두는 이유는 mapRegion()이 내부에서 하는
        // 올림과 별개로, 우리 자신도 정확한 매핑 길이를 알아야
        // stackTop(=stackBase+길이)을 정확히 계산할 수 있기 때문이다.
        const uint64_t rawStackSize = args->stackSize ? args->stackSize : kUserStackSize;
        const uint64_t stackSize = (rawStackSize + 4095UL) & ~4095ULL;
        uint64_t stackBase = 0;
        if (!proc->addressSpace.mapRegion(stackSize, PAGE_WRITABLE | PAGE_USER, VmaBacking::Anonymous, 0,
                                           &stackBase)) {
            UserThread::release(thread);
            args->error = CreateThreadError::OutOfMemory;
            co_return;
        }
        const uint64_t stackTop = stackBase + stackSize;

        // SysV 관례상 함수 진입 시점의 RSP%16==8을 흉내낸다(call이
        // 방금 반환주소 8바이트를 push한 것처럼 보이게) - 그 8바이트
        // 자리에 0을 심어 둔다. `entry`가 유저랜드 트램폴린(§3 항목2,
        // `SelfTerminateThread`를 대신 호출하는 wrapping) 없이 직접
        // 실수로 ret하면 주소 0으로 점프하는 대신 그 자리에서 곧장
        // NULL 페이지 폴트로 정직하게 죽는다(기존 유저 폴트 처리
        // 경로가 그대로 잡음) - 완전히 정의되지 않은 동작보다 안전한
        // 방어(트램폴린은 유저랜드 C 런타임 몫이라 커널이 강제할 수
        // 없다). kSetupInitialUserStack()
        // 과 동일한 이유로 현재 CR3에 기대지 않고 Paging::translatePage()
        // +direct map으로 직접 쓴다(이 onExec()이 reactor 컨텍스트에서
        // 실행 중일 수 있어 proc->pml4Phys가 지금 CR3라는 보장이 없다).
        const uint64_t retSlotAddr = stackTop - 8;
        const uint64_t retSlotPage = retSlotAddr & ~0xFFFULL;
        if (const uint64_t retSlotPhys = Paging::translatePage(retSlotPage, proc->pml4Phys)) {
            auto* pageVirt = reinterpret_cast<uint8_t*>(kPhysToVirt(retSlotPhys));
            *reinterpret_cast<uint64_t*>(pageVirt + (retSlotAddr - retSlotPage)) = 0;
        }

        thread->process = WeakPtr<Process>(proc);
        thread->isUserLevel = true;
        thread->userPml4Phys = proc->pml4Phys;
        thread->ring3EntryPoint = args->entry;
        thread->ring3UserStackTop = retSlotAddr;
        // [신규, 2026-09-18, SP-76250478 §3 항목2, PN-0EB2FABF]
        // SelfTerminateThreadHandler(scheduler.cpp)가 이 스레드 종료
        // 시 unmapRegion()으로 그대로 넘길 값 - syscall.h의 threadStackBase
        // 문서 주석 참고.
        thread->threadStackBase = stackBase;
        thread->threadStackSize = stackSize;
        thread->threadStartArg = args->arg;
        thread->init(kEnterRing3Thread, nullptr);
        thread->ensureSelfRef();
        thread->threadId = proc->nextThreadId++;

        proc->threads.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
        if (!proc->threads.insert(thread->sharedSelf())) {
            // threads.insert() 실패 - 위 process.h/syscall.h 문서 주석의
            // "순서 중요" 계약대로 release() 전에 이 스레드가 쓴 자원
            // (여기서는 컨테이너 슬롯이 애초에 없으니 스택만)부터 되돌린다.
            proc->addressSpace.unmapRegion(stackBase, stackSize);
            UserThread::release(thread);
            args->error = CreateThreadError::OutOfMemory;
            co_return;
        }

        Scheduler::enqueue(Scheduler::currentCoreIndex(), thread);

        args->threadId = thread->threadId;
        args->error = CreateThreadError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

CreateThreadHandler gCreateThreadHandler;

// [신규, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF] Join이 co_await하는
// 커스텀 awaiter - "대상이 아직 안 끝났으면 이 코루틴을 정지시키고,
// SelfTerminateThreadHandler(scheduler.cpp)가 대상 종료 시 대신
// 재개시킨다"는 SP-76250478 §4가 "착수 세션이 확정할 순수 구현 세부"로
// 남겨 둔 문제의 실제 구현 - 이 커널 전체에서 `co_await`가 실제로
// suspend까지 이어지는 최초의 지점이다(기존 onExec들은 전부 co_await
// 없이 즉시 co_return, async_task.h의 `AsyncTaskWeakRef` 클래스 문서
// 참고). 그 `AsyncTaskWeakRef`(원래 타임아웃 전용이었으나 이번 증분에서
// 공개 재사용 primitive로 승격)를 그대로 재사용해 "이 Join AsyncTask가
// 재개 시점까지 살아있는지"를 그 사이 이 스레드/프로세스가 무슨 일을
// 겪든 안전하게 관찰 가능하게 만든다.
class JoinAwaiter {
public:
    JoinAwaiter(AsyncTask* joinTask, UserThread* target) : _joinTask(joinTask), _target(target) {}

    bool await_ready() const noexcept { return false; }  // 항상 정지(빠른 경로는 co_await 이전에 이미 처리됨)

    // true를 반환하면 실제로 정지, false면 즉시 재개(할당 실패 등 -
    // await_resume()의 반환값으로 호출부에 실패를 알린다).
    bool await_suspend(std::coroutine_handle<>) {
        AsyncTaskWeakRef* ref = _joinTask->ensureWeakRef();
        if (!ref) {
            return false;  // 슬랩 고갈 - 정지하지 않고 곧장 재개
        }
        ref->addRef();  // 이 Join(joiner) 쪽 몫 - SelfTerminateThreadHandler가 소비 후 release()
        _target->joinerAsyncTask = ref;
        _registered = true;
        return true;
    }

    // true면 실제로 정지했다가 재개됨(args는 이미 SelfTerminateThreadHandler
    // 가 채워 뒀다), false면 await_suspend()가 즉시 재개를 선택(할당 실패).
    bool await_resume() const noexcept { return _registered; }

private:
    AsyncTask* _joinTask;
    UserThread* _target;
    bool _registered = false;
};

// [신규, 2026-09-18, SP-76250478 §3.1, PN-0EB2FABF] Join 본체 - 대상을
// 호출자 자신의 threads에서 threadId로 찾는다(process.h JoinArgs 문서
// 주석 참고 - 진짜 블로킹, Wait(35번, 프로세스 좀비 회수)의 "v1은
// 논블로킹"과 다른 점).
class JoinHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<JoinArgs*>(argsRaw);

        // WaitHandler/CreateThreadHandler와 동일한 관례(PN-5BBD4301) -
        // onExec() 안에서 Scheduler::currentTask()를 직접 쓰지 않는다.
        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> proc = caller ? caller->process.lock() : SharedPtr<Process>();
        if (!proc) {
            args->error = JoinError::NotFound;
            co_return;
        }

        auto* slot = proc->threads.find(
            [tid = args->targetThread](const SharedPtr<UserThread>& t) { return t && t->threadId == tid; });
        if (!slot) {
            args->error = JoinError::NotFound;
            co_return;
        }
        UserThread* target = slot->value.get();
        if (target == caller) {
            args->error = JoinError::Self;
            co_return;
        }
        if (target->detached) {
            args->error = JoinError::Detached;
            co_return;
        }
        if (target->joinerAsyncTask) {
            // v1은 대상 하나당 동시 joiner 1명만 지원(§3.1) - 다중
            // joiner 대기열은 실사용처가 생기면 후속.
            args->error = JoinError::AlreadyJoining;
            co_return;
        }

        if (target->isZombie) {
            // 빠른 경로 - 이미 좀비(먼저 정상 종료해 뒀음). 그 자리에서
            // 바로 회수 - 정지할 필요조차 없다.
            args->exitCode = target->exitCode;
            args->error = JoinError::None;
            proc->threads.erase(slot);
            UserThread::release(target);
            co_return;
        }

        // 느린 경로 - 대상이 아직 살아있다. 정지했다가
        // SelfTerminateThreadHandler(scheduler.cpp)가 대신 깨워 줄 때까지
        // 기다린다(위 JoinAwaiter 문서 참고). `proc`(SharedPtr)을 이
        // 코루틴 프레임이 정지 중에도 계속 쥐고 있어, 그동안 이 Process
        // 가 파괴되지 않는다는 보너스가 있다(코루틴 지역 변수의 소멸자는
        // 재개/취소 시점까지 전혀 안 불림 - 표준 C++20 코루틴 프레임
        // 수명 규칙 그대로, 이 프로젝트의 "raw 슬랩엔 placement new
        // 없음" 관례와는 무관한 완전히 별개의 축).
        //
        // **알려진 v1 한계**: 이 Join 자신의 호출자가 정지 중에 먼저
        // 죽으면(강제 종료 등) `onCancel()`(아래)이 불려 이 코루틴
        // 프레임은 안전하게 파괴되지만(async_task.cpp kReleaseAsyncTask()
        // 가 이번 증분에서 고친 부분), `target->joinerAsyncTask`는
        // 정리되지 않은 채 남는다 - 그 결과 target은 이후 영원히
        // AlreadyJoining으로 거부되는(다른 스레드가 다시 join 시도해도)
        // 좀비가 된다. 다중 joiner 대기열과 같은 급의 v1 한계로
        // 문서화만 하고 감수한다(RM-23F4B687 §4) - 정말 필요해지면
        // onCancel()에 target까지 전달하도록 별도 내부 상태를 추가한다.
        const bool registered = co_await JoinAwaiter(task, target);
        if (!registered) {
            args->error = JoinError::OutOfMemory;
        }
        // registered==true면 args는 이미 SelfTerminateThreadHandler가
        // 채워 뒀다 - 여기서 더 할 일 없음.
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    // 위 "알려진 v1 한계" 문서 주석 참고 - target->joinerAsyncTask 정리는
    // 하지 않는다(이 시점엔 target 자체를 모른다 - args/task 어느 쪽에도
    // 안 실려 있음). 코루틴 프레임 자체의 안전한 파괴는 프레임워크
    // (kReleaseAsyncTask)가 담당하므로 이 메서드는 할 일이 없다.
    void onCancel(AsyncTask*, void*) override {}
};

JoinHandler gJoinHandler;

// [신규, 2026-09-18, SP-76250478 §3 항목3, PN-0EB2FABF] Detach 본체 -
// 논블로킹(그 자리에서 플래그만 세우고 즉시 끝남).
class DetachHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<DetachArgs*>(argsRaw);

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> proc = caller ? caller->process.lock() : SharedPtr<Process>();
        if (!proc) {
            args->error = DetachError::NotFound;
            co_return;
        }

        auto* slot = proc->threads.find(
            [tid = args->targetThread](const SharedPtr<UserThread>& t) { return t && t->threadId == tid; });
        if (!slot) {
            args->error = DetachError::NotFound;
            co_return;
        }
        UserThread* target = slot->value.get();
        if (target == caller) {
            args->error = DetachError::Self;
            co_return;
        }
        if (target->joinerAsyncTask) {
            // §3.1 "joinerAsyncTask가 이미 세팅돼 있는 상태에서 Detach()가
            // 호출되면(경쟁 상황) 에러로 거부한다" - 설계 문서 그대로.
            args->error = DetachError::AlreadyJoining;
            co_return;
        }

        // [알려진 한계, RM-23F4B687 §4 취지 - 순수 구현 세부] target이
        // 이미 isZombie(정상 종료했지만 아직 아무도 회수 안 한 상태)
        // 여도 여기서 즉시 회수하지는 않는다 - 설계 문서는 "정상 종료
        // *시점*에 detached면 즉시 회수"만 명시했지 "이미 좀비인
        // 스레드를 뒤늦게 detach하면 그 순간 회수"까지는 요구하지
        // 않는다. 실사용처가 생기면 이 지점에서 즉시 회수하도록 확장할
        // 수 있다.
        target->detached = true;
        args->error = DetachError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

DetachHandler gDetachHandler;

// [신규, 2026-09-18, SP-30FCC8AE §3/§4, PN-88E62419] Kill의 권한
// 판정 - §3이 확정한 순서 그대로: (1) 커널/KernelService는 role 자체가
// 이미 무제한이라 uid 판정을 아예 건너뛴다(uid/gid와 통합 안 함,
// PN-617F4E52 "설계 시 다뤄야 할 것" 2번 항목 답변), (2) target의
// **직계 부모**가 caller면 허용(SP-9CB55C5B가 이 관계의 판정 방식을
// 소유 - "직계 부모"만, 조상 전체가 아니다 - DebugAttach(SP-9A6D579F
// §3.2, debug_session.cpp `kFindDebuggableChild`)가 이미 같은 관계를
// 같은 범위로 구현해 둔 기존 선례와 일관성을 맞춘다), (3) 그 외엔
// `kCheckPermission()`(root 특권 + uid/gid RWX, libkenv/permission.h)
// 최종 판정 - "w" 비트 하나만 실질적 의미를 가진다(r/x는 Process
// 자원에 아직 쓰임이 없어 mode 안에서 항상 0으로 취급, target이 그
// 비트들을 세팅할 방법 자체가 없으므로 자동으로 그렇게 된다).
bool kCanSendSignal(const Process& caller, const Process& target) {
    if (caller.role == ProcessRole::KernelService) {
        return true;
    }
    if (SharedPtr<Process> parent = target.parent.lock()) {
        if (parent.get() == &caller) {
            return true;
        }
    }
    return kCheckPermission(caller.uid, caller.gid, target.uid, target.gid, target.signalPermission,
                             kPermOwnerWrite);
}

// [SP-0666DB3C §4.5, RM-48E1E610 29번, PN-71E50394 항목 4, 수정
// 2026-09-18 PN-88E62419] `Kill` 본체 - signal.h의 `KillArgs` 문서
// 주석 그대로. **[갱신, PN-88E62419]** v1의 "호출자 자신의 직계
// 자식만" 스코프를 걷어내고 `kResolveProcessId()`(SP-9CB55C5B §2,
// PN-C39882D0 구현 완료)로 임의 대상을 안전하게 해석한 뒤
// `kCanSendSignal()`(위)로 권한을 판정한다 - 이 함수가
// `kResolveProcessId`의 첫 실사용처다.
class KillHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<KillArgs*>(argsRaw);

        if (args->signal == SignalNumber::None || static_cast<uint32_t>(args->signal) >= kSignalCount) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }

        // WaitHandler::onExec과 동일한 관례(PN-5BBD4301 - task->
        // submitterTask.lock()으로, Scheduler::currentTask()가 아니다)로
        // 호출자 자신의 Process를 얻는다.
        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> self = caller ? caller->process.lock() : SharedPtr<Process>();
        if (!self) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        // 좀비(이미 죽어 주소공간이 반납된 프로세스)에게 신호를 보내는
        // 건 무의미하므로 존재하지 않는 것과 동일하게 취급한다 -
        // `raiseSignal()` 자체는 좀비에도 안전하게 호출 가능하지만
        // (threads가 이미 비어 있을 수 있어 그냥 pendingSignals에만
        // 쌓이고 끝) 아무 효과가 없어 혼란만 준다(v1과 동일한 정책,
        // 대상 해석 방식만 바뀜).
        SharedPtr<Process> target = kResolveProcessId(args->targetProcessId);
        if (!target || target->isZombie) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        if (!kCanSendSignal(*self, *target)) {
            args->error = ChannelError::PermissionDenied;
            co_return;
        }

        args->error = target->raiseSignal(args->signal) ? ChannelError::None : ChannelError::ResourceExhausted;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

KillHandler gKillHandler;

// [SP-0666DB3C §4.5, RM-48E1E610 30번, PN-71E50394 항목 4] `SignalAction`
// 본체 - signal.h의 `SignalActionArgs` 문서 주석 그대로, 호출자 자신의
// `dispositions[]`만 바꾼다.
class SignalActionHandler : public AsyncTaskHandler {
public:
    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override {
        auto* args = static_cast<SignalActionArgs*>(argsRaw);

        if (args->signal == SignalNumber::None || static_cast<uint32_t>(args->signal) >= kSignalCount) {
            args->error = ChannelError::InvalidArgument;
            co_return;
        }
        if (args->disposition == SignalDisposition::Handler) {
            // §4.4/PN-124C105B 전까지 - 등록만 받아 두고 조용히
            // 무시하지 않는다(signal.h SignalActionArgs 문서 참고).
            args->error = ChannelError::NotSupported;
            co_return;
        }
        if (args->disposition == SignalDisposition::Ignore &&
            (args->signal == SignalNumber::Kill || args->signal == SignalNumber::Stop)) {
            args->error = ChannelError::InvalidArgument;  // 마스킹 불가 원칙(signal.h)
            co_return;
        }

        SharedPtr<Task> submitter = task->submitterTask.lock();
        auto* caller = submitter ? static_cast<UserThread*>(submitter.get()) : nullptr;
        SharedPtr<Process> self = caller ? caller->process.lock() : SharedPtr<Process>();
        if (!self) {
            args->error = ChannelError::NotFound;
            co_return;
        }

        self->dispositions[static_cast<uint32_t>(args->signal)] = args->disposition;
        args->error = ChannelError::None;
        co_return;
    }
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

SignalActionHandler gSignalActionHandler;

}  // namespace

// [신규, 2026-09-18, PN-44C91D6E] `fork()` 본체 - idt.cpp의
// `kHandleSyscallTrap`이 `kSyscallVerbFork`를 직접 가로채 전체
// `InterruptFrame*`을 그대로 넘긴다(process.h의 `kHandleForkSyscall`
// 문서 주석 참고). 이 함수는 부모 자신의 트랩 컨텍스트에서 **동기적
//으로** 실행되므로(AsyncTaskHandler처럼 나중에 리액터 컨텍스트에서
// 실행되는 게 아님) `Scheduler::currentTask()`가 곧 부모 자신이다 -
// SpawnProcessHandler::onExec()이 `task->submitterTask.lock()`을
// 써야 했던 것과 다른 이유(RM-23F4B687이 문서화한 "onExec() 안에서
// Scheduler::currentTask()를 믿으면 안 된다" 함정은 나중에 실행되는
// 비동기 컨텍스트에만 해당 - 여기는 그 함정 자체가 성립하지 않는다).
void kHandleForkSyscall(InterruptFrame* frame) {
    auto* parentThread = static_cast<UserThread*>(Scheduler::currentTask());
    SharedPtr<Process> parentProcShared = parentThread ? parentThread->process.lock() : SharedPtr<Process>();
    if (!parentProcShared) {
        // 이론상 도달 불가(fork()는 항상 실제 UserThread 실행 흐름에서만
        // 온다) - 방어적으로만.
        frame->rax = static_cast<uint64_t>(kInvalidProcessId);
        return;
    }
    Process* parentProc = parentProcShared.get();

    Process* proc = Process::allocate();
    UserThread* thread = proc ? UserThread::allocate() : nullptr;
    if (!proc || !thread || !proc->init()) {
        if (thread) {
            UserThread::release(thread);
        }
        if (proc) {
            Process::release(proc);
        }
        frame->rax = static_cast<uint64_t>(kInvalidProcessId);
        return;
    }
    // Process::init() 직후, 아래에서 weakFromThis()를 쓰기 전에 이
    // Process를 kMakeShared로 감싼다 - execImage() 호출부(SpawnProcessHandler
    // 등)와 정확히 같은 전제(process.cpp execImage() 문서 주석 참고).
    SharedPtr<Process> procShared = kMakeShared<Process>(proc);
    if (!procShared) {
        UserThread::release(thread);
        proc->destroy();
        Process::release(proc);
        frame->rax = static_cast<uint64_t>(kInvalidProcessId);
        return;
    }

    // Process::init()이 자동으로 만들어 둔 힙 VMA(부모와 무관한 임의
    // 주소)는 그대로 두면 안 된다 - 아래 forEachVma 복제가 부모의 진짜
    // 힙 VMA를 그 자리에 대신 채운다(process.h Process::init() 문서
    // 주석의 "Brk 힙 VMA" 절 참고).
    proc->addressSpace.unmapRegion(proc->heapStart, kMinHeapLength);

    // 부모의 VMA 전부를 자식 주소공간에 재현한다 - Anonymous는 COW로
    // (부모/자식 양쪽 PTE를 함께 내려야 함 - 자식만 내리면 부모가 먼저
    // 써도 진짜 복사가 안 일어나 자식을 오염시킨다), FixedPhysical/
    // FileBacked는 COW 없이 그대로 물리 프레임을 공유 매핑만 한다.
    bool copyOk = true;
    parentProc->addressSpace.forEachVma([&](const Vma& vma) {
        if (!copyOk) {
            return;
        }
        const uint64_t length = vma.end - vma.start + 1;
        for (uint64_t off = 0; off < length; off += 4096UL) {
            const uint64_t vaddr = vma.start + off;
            const uint64_t phys = Paging::translatePage(vaddr, parentProc->pml4Phys);
            if (!phys) {
                continue;  // 이론상 도달 불가(장부에 있는 범위는 항상 매핑돼 있음) - 방어적 스킵
            }
            if (vma.backing == VmaBacking::Anonymous) {
                PageFrameAllocator::retain(phys);
                const uint64_t cowFlags = (vma.prot & ~PAGE_WRITABLE) | PAGE_USER | PAGE_COW;
                Paging::mapPage(vaddr, phys, cowFlags, proc->pml4Phys);
                // 부모 쪽도 함께 COW로 내린다 - 지금 이 함수가 부모
                // 자신의 트랩 컨텍스트에서 실행 중이라 parentProc->pml4Phys
                // 가 곧 현재 CR3이므로, Paging::mapPage()가 알아서 이
                // 코어의 TLB도 함께 무효화한다(Paging::mapPage 문서 주석).
                Paging::mapPage(vaddr, phys, cowFlags, parentProc->pml4Phys);
            } else {
                Paging::mapPage(vaddr, phys, vma.prot, proc->pml4Phys);
            }
        }
        if (!proc->addressSpace.registerFixedRegion(vma.start, length, vma.prot, vma.backing)) {
            copyOk = false;
        }
    });
    if (!copyOk) {
        UserThread::release(thread);
        procShared.reset();  // destroy()+슬랩 반납(위 SpawnProcessHandler와 동일한 패턴)
        frame->rax = static_cast<uint64_t>(kInvalidProcessId);
        return;
    }

    // 힙 브레이크 북키핑도 부모 값 그대로 - 자식의 힙 VMA는 위 루프가
    // 부모의 실제 힙 VMA를 이미 재현해 뒀으므로, heapStart/heapBrk도
    // 그 자리를 가리켜야 이후 brk()가 올바른 범위를 찾는다.
    proc->heapStart = parentProc->heapStart;
    proc->heapBrk = parentProc->heapBrk;

    // execImage()가 하던 것과 동일한 UserThread/Process 배선 - ELF
    // 로딩이 없을 뿐 나머지는 전부 같은 절차(process.cpp execImage()
    // 문서 주석과 1:1 대응).
    thread->process = WeakPtr<Process>(procShared);
    thread->isUserLevel = true;
    thread->userPml4Phys = proc->pml4Phys;
    // [신규, 2026-09-18, PN-44C91D6E] 자식은 execImage()의 kEnterRing3
    // (고정 entryPoint+새 스택)이 아니라, 부모가 트랩한 시점의 전체
    // InterruptFrame을 그대로 재현하는 kResumeForkedRing3로 시작한다 -
    // rax만 0으로 덮어써(POSIX fork() 자식 쪽 반환값 규약) "부모가
    // 트랩한 바로 그 지점에서, 자식이라는 것만 다르게" 재개한다.
    thread->forkResumeFrame = *frame;
    thread->forkResumeFrame.rax = 0;
    thread->init(kResumeForkedRing3, nullptr);
    thread->ensureSelfRef();
    // [신규, 2026-09-18, SP-76250478 §2.1, PN-0EB2FABF] fork() 자식의
    // 새 Process는 항상 새로 init()된 상태(nextThreadId=0)에서 시작하므로
    // execImage()와 동일하게 사실상 항상 0 - 부모의 threadId를 그대로
    // 물려받지 않는다(부모/자식은 별개 Process이므로 각자 독립된 id
    // 공간).
    thread->threadId = proc->nextThreadId++;
    // [수정, 2026-09-18, SP-76250478, PN-0EB2FABF] execImage()와 동일한
    // 대체(위 execImage()의 threads.insert() 문서 주석 참고) - fork()
    // 자식도 스레드를 정확히 하나만 만드므로 관찰 가능한 동작은 동일.
    proc->threads.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    proc->threads.insert(thread->sharedSelf());

    // TLS 인스턴스(PN-22E5E9E7 항목5/6) - 부모가 PT_TLS 템플릿을 갖고
    // 있었으면 그 사실 자체도 복제해야 makeUserTlsInstance()가 자식용
    // 인스턴스를 새로 만든다(공유가 아니라 독립 복사본 - fork() 자식은
    // 자기 자신의 thread_local 상태를 가져야 한다, execImage()와 동일한
    // 이유로 프로세스마다 하나씩).
    proc->hasTlsTemplate = parentProc->hasTlsTemplate;
    proc->tlsTemplateVaddr = parentProc->tlsTemplateVaddr;
    proc->tlsTemplateFilesz = parentProc->tlsTemplateFilesz;
    proc->tlsTemplateMemsz = parentProc->tlsTemplateMemsz;
    proc->tlsTemplateAlign = parentProc->tlsTemplateAlign;
    if (!proc->makeUserTlsInstance(thread)) {
        // [신규, 2026-09-18, SP-76250478, PN-0EB2FABF] 바로 위에서
        // `proc->threads`에 이미 이 thread를 등록해 뒀으므로, 실제
        // 슬랩 반납(UserThread::release()) 전에 그 컨테이너 슬롯부터
        // 지운다(process.h의 threads 문서 주석 - "순서 중요" 참고,
        // 반대 순서면 컨테이너가 이미 반납된 메모리를 가리키는 채로
        // procShared.reset()을 맞는다) - clear()가 이 하나뿐인 슬롯의
        // 청크 메모리까지 반납한다(chunked_list.h 참고).
        proc->threads.clear();
        UserThread::release(thread);
        procShared.reset();
        frame->rax = static_cast<uint64_t>(kInvalidProcessId);
        return;
    }

    // 프로세스 트리 등록 - SpawnProcessHandler 5단계와 동일한 패턴.
    parentProc->children.ensureAllocator(&GenericSlabAllocator::alloc, &GenericSlabAllocator::free);
    if (!parentProc->children.insert(procShared)) {
        // 위와 동일한 이유로 release() 전에 threads부터 정리한다.
        proc->threads.clear();
        UserThread::release(thread);
        procShared.reset();
        frame->rax = static_cast<uint64_t>(kInvalidProcessId);
        return;
    }
    procShared->parent = WeakPtr<Process>(parentProcShared);

    // 자원 그룹 소속 - 부모의 그룹을 그대로 물려받는다(SpawnProcessHandler
    // 와 동일한 기본값), 메모리 사용량 계정도 부모와 동일한 총량으로
    // 맞춘다(주소공간 전체를 그대로 복제했으므로 - execImage()의 ELF
    // 세그먼트 스캔 대신 이 값을 그대로 쓴다).
    procShared->joinResourceGroup(parentProc->group ? parentProc->group : &gRootResourceGroup);
    // [신규, SP-30FCC8AE §1] uid/gid 상속 - SpawnProcessHandler와
    // 동일한 패턴(fork()는 parentProc이 항상 존재하는 경로라 방어적
    // null 분기 불필요).
    procShared->uid = parentProc->uid;
    procShared->gid = parentProc->gid;
    procShared->memoryBytesUsed = parentProc->memoryBytesUsed;
    if (procShared->group) {
        procShared->group->accounting.totalMemoryBytesUsed += procShared->memoryBytesUsed;
    }

    procShared->processId = kAllocateProcessId(procShared);
    Scheduler::enqueue(Scheduler::currentCoreIndex(), thread);

    // 부모의 반환값 - 자식의 ProcessId(POSIX fork()와 동일한 규약,
    // 자식 쪽 반환값 0은 위에서 forkResumeFrame.rax로 이미 처리).
    frame->rax = static_cast<uint64_t>(procShared->processId);
}

void Process::registerSyscallEndpoints() {
    SyscallRegistry::registerHandler(kSyscallEndpointSpawnProcess, &gSpawnProcessHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointWait, &gWaitHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointKill, &gKillHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointSignalAction, &gSignalActionHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointCreateThread, &gCreateThreadHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointJoin, &gJoinHandler);
    SyscallRegistry::registerHandler(kSyscallEndpointDetach, &gDetachHandler);
}

}  // namespace kernel
