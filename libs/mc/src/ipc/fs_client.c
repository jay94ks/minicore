#include <mc/fs_client.h>

#include <mc/syscall.h>
#include <mc/util.h>
#include <mc/vfs_client.h>

#define MC_PAGE_SIZE 4096u

uint64_t mc_fs_read_all(uint32_t fs_handle, uint64_t open_file_id, uint8_t* out_buf,
                         uint64_t out_cap) {
    uint64_t total = 0;
    for (;;) {
        mc_message req;
        mc_zero_bytes(&req, sizeof(req));
        req.label = MC_FS_OP_READ;
        req.regs[0] = open_file_id;
        req.regs[1] = MC_PAGE_SIZE;

        mc_message reply;
        mc_zero_bytes(&reply, sizeof(reply));
        mc_ipc_call(fs_handle, &req, &reply);
        if (reply.regs[1] != MC_FS_STATUS_OK || reply.page_count != 1) {
            break;
        }
        uint64_t n = reply.regs[0];
        if (n == 0) {
            break;  // EOF.
        }
        if (total + n > out_cap) {
            n = out_cap - total;
        }
        const uint8_t* src = (const uint8_t*)(uintptr_t)reply.pages[0].vaddr;
        for (uint64_t i = 0; i < n; ++i) {
            out_buf[total + i] = src[i];
        }
        total += n;
        if (n < MC_PAGE_SIZE || total >= out_cap) {
            break;
        }
    }
    return total;
}

uint64_t mc_fs_read(uint32_t fs_handle, uint64_t open_file_id, uint8_t* out_buf, uint64_t count) {
    uint64_t want = count;
    if (want > MC_PAGE_SIZE) {
        want = MC_PAGE_SIZE;
    }
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_FS_OP_READ;
    req.regs[0] = open_file_id;
    req.regs[1] = want;

    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(fs_handle, &req, &reply);
    if (reply.regs[1] != MC_FS_STATUS_OK || reply.page_count != 1) {
        return 0;
    }
    uint64_t n = reply.regs[0];
    const uint8_t* src = (const uint8_t*)(uintptr_t)reply.pages[0].vaddr;
    for (uint64_t i = 0; i < n; ++i) {
        out_buf[i] = src[i];
    }
    return n;
}

uint64_t mc_fs_list(uint32_t fs_handle, uint8_t* out_names_blob, uint64_t out_cap,
                     uint32_t* out_count) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_FS_OP_LIST;

    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(fs_handle, &req, &reply);
    if (reply.regs[0] != MC_FS_STATUS_OK || reply.page_count != 1) {
        return reply.regs[0] != MC_FS_STATUS_OK ? reply.regs[0] : MC_FS_STATUS_NOT_FOUND;
    }
    *out_count = (uint32_t)reply.regs[1];
    uint64_t n = MC_PAGE_SIZE;
    if (n > out_cap) {
        n = out_cap;
    }
    const uint8_t* src = (const uint8_t*)(uintptr_t)reply.pages[0].vaddr;
    for (uint64_t i = 0; i < n; ++i) {
        out_names_blob[i] = src[i];
    }
    return MC_FS_STATUS_OK;
}
