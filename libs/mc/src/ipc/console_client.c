#include <mc/console_client.h>

#include <mc/syscall.h>
#include <mc/util.h>

void mc_console_print(uint32_t console_handle, const char* text, uint64_t len) {
    if (len > MC_CONSOLE_MAX_LINE_LEN) {
        len = MC_CONSOLE_MAX_LINE_LEN;
    }
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_CONSOLE_OP_PRINT;
    req.regs[0] = len;
    mc_pack_bytes(&req.regs[1], 3 * sizeof(uint64_t), text, len);

    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(console_handle, &req, &reply);
}

void mc_console_print_str(uint32_t console_handle, const char* s) {
    mc_console_print(console_handle, s, mc_cstr_len(s));
}

void mc_console_print_char(uint32_t console_handle, char c) {
    mc_console_print(console_handle, &c, 1);
}
