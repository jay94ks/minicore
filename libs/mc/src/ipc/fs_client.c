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

// M54(musl-userland-porting.md §M54, ADR-225) — data는 호출자의
// 임의 버퍼(page-aligned 보장 없음)라, page_descriptor 전송
// (ADR-159/161)이 요구하는 page-aligned 소스 버퍼로 먼저 복사해
// 담는다(procsrv::write_elf_to_vfs()의 g_write_scratch와 같은 이유
// ・같은 패턴).
uint64_t mc_fs_write(uint32_t fs_handle, uint64_t open_file_id, const uint8_t* data,
                      uint64_t count) {
    static uint8_t scratch[MC_PAGE_SIZE] __attribute__((aligned(MC_PAGE_SIZE)));
    uint64_t chunk = count;
    if (chunk > MC_PAGE_SIZE) {
        chunk = MC_PAGE_SIZE;
    }
    for (uint64_t i = 0; i < MC_PAGE_SIZE; ++i) {
        scratch[i] = (i < chunk) ? data[i] : 0;
    }

    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_FS_OP_WRITE;
    req.regs[0] = open_file_id;
    req.regs[1] = chunk;
    req.page_count = 1;
    req.pages[0].vaddr = (uint64_t)(uintptr_t)scratch;
    req.pages[0].length = MC_PAGE_SIZE;
    req.pages[0].mode = MC_TRANSFER_COPY;

    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(fs_handle, &req, &reply);
    if (reply.regs[1] != MC_FS_STATUS_OK) {
        return 0;
    }
    return reply.regs[0];
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
