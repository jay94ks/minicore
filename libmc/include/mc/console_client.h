// libmc/include/mc/console_client.h — servers/drivers/console의
// OP_PRINT 클라이언트(docs/design/foundations.md ADR-170).
#pragma once

#include <stdint.h>

#define MC_CONSOLE_OP_PRINT 1u
#define MC_CONSOLE_MAX_LINE_LEN 24u  // regs[0]=길이, regs[1..3]=최대 24바이트.

void mc_console_print(uint32_t console_handle, const char* text, uint64_t len);
void mc_console_print_str(uint32_t console_handle, const char* s);
void mc_console_print_char(uint32_t console_handle, char c);
