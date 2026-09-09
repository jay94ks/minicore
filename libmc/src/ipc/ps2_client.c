#include <mc/ps2_client.h>

#include <mc/syscall.h>
#include <mc/util.h>

mc_ps2_key mc_ps2_read_key(uint32_t ps2_handle) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PS2_OP_READ_KEY;

    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(ps2_handle, &req, &reply);

    mc_ps2_key key;
    key.got_key = (reply.regs[0] != 0) ? 1 : 0;
    key.ascii = (uint8_t)reply.regs[2];
    return key;
}
