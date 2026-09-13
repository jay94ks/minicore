#include "idt.h"
#include "serial.h"

namespace {

// Xen public header(xen/arch-x86/hvm/start_info.h)의
// struct hvm_start_info 첫 필드만 본다 - v1은 매직만 확인한다.
constexpr unsigned int kHvmStartInfoMagic = 0x336ec578;

}  // namespace

// boot.S가 higher-half로 넘어온 뒤 호출한다. rdi = struct
// hvm_start_info의 물리 주소(PVH direct boot ABI, EBX로 전달된 값을
// boot.S가 그대로 넘김). 이 시점에는 커널(ring 0)만 실행 중이다 -
// devmgr 등 "커널 서비스"는 아직 존재하지 않는다(SP-8B6B8D25 §2-A).
extern "C" void kMain(unsigned int startInfoAddr) {
    kernel::Serial::kInit();
    kernel::Serial::kWrite("minicore: booted via Xen PVH (higher-half, long mode)\n");

    const auto* magic = reinterpret_cast<const unsigned int*>(static_cast<unsigned long>(startInfoAddr));
    if (*magic == kHvmStartInfoMagic) {
        kernel::Serial::kWrite("minicore: hvm_start_info magic OK\n");
    } else {
        kernel::Serial::kWrite("minicore: hvm_start_info magic MISMATCH\n");
    }

    // TODO(SP-8B6B8D25 후속): hvm_start_info 전체(rsdp_paddr 등) 파싱

    kernel::Idt::kInit();
    kernel::Serial::kWrite("minicore: IDT ready\n");

    for (;;) {
        asm volatile("hlt");
    }
}
