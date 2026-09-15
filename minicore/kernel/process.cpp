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

// Task::init()의 단일 void* arg 슬롯으로 kEnterRing3에 넘길 값들 -
// v1은 프로세스가 하나뿐이라 전역 인스턴스 하나로 충분하다(여러
// 프로세스를 동시에 exec()하게 되면 UserThread 자신에 이 값을 옮겨
// 담는 확장이 필요하다 - 후속 과제).
struct Ring3EntryParams {
    kernel::uint64_t pml4Phys = 0;
    kernel::uint64_t entryPoint = 0;
    kernel::uint64_t userStackTop = 0;
};

Ring3EntryParams gRing3EntryParams;

// PN-124C105B/PN-16CA347D 6번 - UserThread가 ring3으로 "처음" 진입하는
// 자리. Task::init()의 entry(arg)로 등록되어 kTaskStartTrampoline이
// 평범한 ring0 함수처럼 호출하지만(`call rbx`, context_switch.S), 이
// 함수는 절대 돌아오지 않는다 - iretq가 특권 레벨 자체를 바꿔 버리기
// 때문이다. 이후 이 UserThread가 다시 ring0으로 오는 유일한 경로는
// 트랩/인터럽트뿐이고(아래 RSP0 설정이 그 경로의 스택을 마련해 둔다),
// 스케줄러가 이 Task를 선점했다 재개하는 경우도 그 트랩의 iretq를
// 통해서만 ring3로 되돌아간다 - 기존 Task 컨텍스트 스위칭
// 메커니즘(kContextSwitch)이 이미 일반적으로 지원한다(InterruptFrame의
// iretq가 특권 레벨 전환까지 그대로 복원하므로 이 부분에 별도 코드가
// 필요 없다).
[[noreturn]] void kEnterRing3(void* argPtr) {
    auto* params = reinterpret_cast<Ring3EntryParams*>(argPtr);
    auto* self = kernel::Scheduler::currentTask();

    // RSP0 - 이 UserThread가 ring3에서 트랩할 때마다 하드웨어가 자동
    // 전환할 커널 스택. **v1 가정**: MINICORE_TASK_STACK_GUARD_PAGE가
    // 꺼져 있어(기본값) 커널 스택이 direct map 위에 있다는 전제로
    // 계산한다 - 켜져 있으면 이 계산식이 안 맞는다(가드 페이지 켠
    // 빌드에서 ring3 진입을 함께 쓰는 조합은 아직 검증 대상 밖,
    // 후속 과제).
    const kernel::uint64_t kernelStackTop =
        kernel::kPhysToVirt(self->kernelStackPhys) + self->kernelStackSize;
    kernel::Gdt::setRsp0ForThisCore(kernelStackTop);

    // 아래 인라인 asm은 리터럴 0x1b/0x23을 직접 쓴다(피연산자 제약
    // 안에서 이름 있는 상수를 쓰면 크기 불일치 등으로 더 위험할 수
    // 있어, 리터럴+static_assert 조합을 택했다) - gdt.h의 값이
    // 바뀌면 여기서 빌드 타임에 바로 걸린다.
    static_assert(kernel::kGdtUserDataSelector == 0x1b, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");
    static_assert(kernel::kGdtUserCodeSelector == 0x23, "gdt.h 값이 바뀌면 아래 asm 리터럴도 같이 바꿀 것");

    const kernel::uint64_t pml4Phys = params->pml4Phys;
    const kernel::uint64_t entryPoint = params->entryPoint;
    const kernel::uint64_t userStackTop = params->userStackTop;

    // CR3 전환은 이 Task 자신의(커널 higher-half에 있는) 스택/코드
    // 위에서 실행 중이므로 안전하다 - 모든 프로세스가 커널 상위
    // 절반을 공유한다(PN-58501EAA "중요 발견" 참고). 세그먼트
    // 레지스터는 유저 데이터 셀렉터로 미리 맞춰 두고(SS 자체는 iretq
    // 프레임이 담당), iretq 프레임(SS/RSP/RFLAGS/CS/RIP)을 쌓은 뒤
    // iretq로 실제 특권 레벨 전환을 일으킨다.
    asm volatile(
        "mov %0, %%cr3\n\t"
        "mov $0x1b, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq $0x1b\n\t"
        "pushq %1\n\t"
        "pushq $0x202\n\t"
        "pushq $0x23\n\t"
        "pushq %2\n\t"
        "iretq\n\t"
        :
        : "r"(pml4Phys), "r"(userStackTop), "r"(entryPoint)
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

    gRing3EntryParams.pml4Phys = pml4Phys;
    gRing3EntryParams.entryPoint = image.entryPoint();
    gRing3EntryParams.userStackTop = kUserStackTop;

    thread->process = this;
    thread->isUserLevel = true;
    thread->init(kEnterRing3, &gRing3EntryParams);
    mainThread = thread;
    return thread;
}

}  // namespace kernel
