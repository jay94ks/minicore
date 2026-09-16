#include "panic.h"

#include "logger.h"

namespace kernel {

void kPanic(const char* message) {
    Logger::panic("\nminicore: PANIC - %s", message);
    for (;;) {
        asm volatile("cli; hlt");
    }
}

}  // namespace kernel
