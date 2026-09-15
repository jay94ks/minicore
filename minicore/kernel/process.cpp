#include "process.h"

#include "gdt.h"
#include "libelf/elf.h"
#include "page_frame_allocator.h"
#include "paging.h"
#include "scheduler.h"
#include "syscall.h"

namespace {

// §5-A "스택 공간은 유저 공간의 끝점에서부터 맵핑" - 128TiB 유저
// 공간의 상한 바로 아래(4KiB 정렬)를 스택 top으로 고정한다. v1은
// 고정 크기(64KiB)/고정 주소이고 가드 페이지가 없다 - 커널 스택
// 가드 페이지(task.cpp, MINICORE_TASK_STACK_GUARD_PAGE)와 같은
// 패턴으로 후속 추가할 수 있는 자리만 남겨 둔다(새 DC 불필요 수준).
constexpr kernel::uint64_t kUserStackTop = 0x00007FFFFFFFF000UL;
constexpr kernel::uint64_t kUserStackSize = 16UL * 4096UL;  // 64KiB

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
// 덮어써지는 결함이 있었다) - CR3를 `self->userPml4Phys`에서 읽는
// 것과 완전히 같은 패턴.
[[noreturn]] void kEnterRing3(void*) {
    auto* self = kernel::Scheduler::currentTask();

    // RSP0은 이 함수에 도달하기 전에 이미 스케줄러가 맞춰 둔다
    // (Scheduler::runLoop()의 idle->Task 디스패치, PN-AEA74E1B) - GDT
    // TSS 구조체 쓰기는 지금 어떤 스택 위에서 실행 중이든 안전해서
    // (메모리 매핑과 무관한 순수 데이터 쓰기) runLoop() 자신이 아직
    // 이 Task 고유의 스택으로 넘어오기 전(idle 컨텍스트)에 해도 된다.
    //
    // **CR3는 다르다(PN-63BCFE45 실측으로 발견)**: `mov cr3`은 그
    // 자리에서 즉시 전체 TLB를 무효화하고 이후의 모든 메모리 접근을
    // 새 주소공간 기준으로 해석하게 만든다 - PN-58501EAA "중요
    // 발견"이 이미 경고했듯, BSP의 kMain()/AP의 kApMain()이 진입할
    // 때부터 쓰는 최초 부트 스택은 저지대 identity map에 있어(커널
    // 자신의 higher-half 이미지 안이 아님) 어떤 프로세스의 PML4에도
    // 안 들어있다 - 그 스택 위에서(=아직 이 Task 자신의 스택으로
    // 넘어오기 전인 idle 컨텍스트에서) CR3를 바꾸면 다음 명령(스택
    // 접근)에서 즉시 폴트/트리플 폴트가 난다. 그래서 CR3는 **반드시
    // 이 Task 자신의(커널 higher-half에 있는, 모든 프로세스가
    // 공유하는) 스택으로 이미 넘어온 뒤에만** 설정해야 한다 - 이
    // 함수(kTaskStartTrampoline이 이 Task 자신의 스택 위에서 부른
    // entry 콜백) 안이 바로 그 지점이다. 이후 이 Task가 다시
    // 디스패치될 때는 `Scheduler::onTick()`이 같은 이유로 안전한
    // 지점(트랩으로 들어와 이미 이 Task 자신의 스택 위)에서
    // `next->userPml4Phys`를 CR3에 다시 싣는다 - `Scheduler::
    // runLoop()`의 idle->Task 경로는 CR3를 건드리지 않는다(idle
    // 컨텍스트 자체가 안전하지 않은 스택이므로).
    asm volatile("mov %0, %%cr3" : : "r"(self->userPml4Phys) : "memory");

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

bool Process::init() {
    pml4Phys = Paging::createAddressSpace();
    if (!pml4Phys) {
        return false;
    }
    mainThread = nullptr;
    lastFault = FaultInfo{};
    return true;
}

void Process::destroy() {
    if (pml4Phys) {
        Paging::destroyAddressSpace(pml4Phys);
        pml4Phys = 0;
    }
}

UserThread* Process::execImage(const elf::Image& image, UserThread* thread) {
    if (!elf::loadIntoAddressSpace(image, pml4Phys)) {
        return nullptr;
    }

    for (uint64_t off = 0; off < kUserStackSize; off += 4096UL) {
        const uint64_t phys = PageFrameAllocator::allocPage();
        if (!phys) {
            // 이미 매핑한 세그먼트/스택 일부의 롤백은 Process::destroy()
            // 호출부 책임(elf::loadIntoAddressSpace와 동일한 관례).
            return nullptr;
        }
        Paging::mapPage(kUserStackTop - kUserStackSize + off, phys, PAGE_WRITABLE | PAGE_USER, pml4Phys);
    }

    thread->process = this;
    thread->isUserLevel = true;
    thread->userPml4Phys = pml4Phys;
    thread->ring3EntryPoint = image.entryPoint();
    thread->ring3UserStackTop = kUserStackTop;
    thread->init(kEnterRing3, nullptr);
    mainThread = thread;
    return thread;
}

}  // namespace kernel
