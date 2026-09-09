#include <mc/vfs_client.h>

#include <mc/syscall.h>
#include <mc/util.h>

uint64_t mc_vfs_open(uint32_t vfs_handle, const char* path, uint64_t identity,
                      uint64_t* out_open_file_id, uint32_t* out_fs_handle) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_FS_OP_OPEN;
    mc_pack_bytes(req.regs, MC_FS_PATH_BUDGET, path, mc_cstr_len(path));
    req.regs[3] = identity;

    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(vfs_handle, &req, &reply);

    if (reply.regs[1] != MC_FS_STATUS_OK) {
        return reply.regs[1];
    }
    if (reply.handle_count != 1) {
        return MC_FS_STATUS_NOT_FOUND;
    }
    *out_open_file_id = reply.regs[0];
    *out_fs_handle = reply.handles[0].src_handle;
    return MC_FS_STATUS_OK;
}
