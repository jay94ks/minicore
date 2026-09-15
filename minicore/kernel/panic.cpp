#include "panic.h"

#include "serial.h"

namespace kernel {

void kPanic(const char* message) {
    Serial::write("\nminicore: PANIC - ");
    Serial::write(message);
    Serial::write("\n");
    for (;;) {
        asm volatile("cli; hlt");
    }
}

}  // namespace kernel
