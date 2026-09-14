#include "process.h"

#include "paging.h"

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

}  // namespace kernel
