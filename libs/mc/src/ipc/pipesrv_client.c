#include <mc/pipesrv_client.h>

#include <mc/pipesrv_protocol.h>
#include <mc/syscall.h>
#include <mc/util.h>

// kernel/core/ipc/endpoint.cpp가 page_descriptor의 vaddr/length를
// 4096바이트 경계로 강제한다(M41이 이미 겪은 것과 같은 이유) — 이
// 파일 전용 정적 페이지 버퍼.
_Alignas(MC_PIPE_CAPACITY) static uint8_t g_pipe_io_buf[MC_PIPE_CAPACITY];

uint8_t mc_pipe_create(uint32_t pipesrv_handle, uint64_t* out_read_id, uint64_t* out_write_id) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PIPE_OP_CREATE;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(pipesrv_handle, &req, &reply);
    if (reply.regs[0] != MC_PIPE_STATUS_OK) {
        return 0;
    }
    if (out_read_id != 0) {
        *out_read_id = reply.regs[1];
    }
    if (out_write_id != 0) {
        *out_write_id = reply.regs[2];
    }
    return 1;
}

uint32_t mc_pipe_read(uint32_t pipesrv_handle, uint64_t id, uint64_t requested_len, void* out_buf,
                       uint64_t* out_len) {
    if (requested_len > MC_PIPE_CAPACITY) {
        requested_len = MC_PIPE_CAPACITY;
    }
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PIPE_OP_READ;
    req.regs[0] = id;
    req.regs[1] = requested_len;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(pipesrv_handle, &req, &reply);
    if (reply.regs[0] != MC_PIPE_STATUS_OK) {
        return (uint32_t)reply.regs[0];
    }
    uint64_t len = reply.regs[1];
    if (len > requested_len) {
        len = requested_len;
    }
    const uint8_t* src = (const uint8_t*)(uintptr_t)reply.pages[0].vaddr;
    for (uint64_t i = 0; i < len; ++i) {
        ((uint8_t*)out_buf)[i] = src[i];
    }
    if (out_len != 0) {
        *out_len = len;
    }
    return MC_PIPE_STATUS_OK;
}

uint32_t mc_pipe_write(uint32_t pipesrv_handle, uint64_t id, const void* buf, uint64_t len,
                       uint64_t* out_written) {
    uint64_t copy_len = len > MC_PIPE_CAPACITY ? MC_PIPE_CAPACITY : len;
    mc_zero_bytes(g_pipe_io_buf, sizeof(g_pipe_io_buf));
    mc_pack_bytes(g_pipe_io_buf, sizeof(g_pipe_io_buf), (const char*)buf, copy_len);

    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PIPE_OP_WRITE;
    req.regs[0] = id;
    req.regs[1] = copy_len;
    req.page_count = 1;
    req.pages[0].vaddr = (uint64_t)(uintptr_t)g_pipe_io_buf;
    req.pages[0].length = MC_PIPE_CAPACITY;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(pipesrv_handle, &req, &reply);
    if (reply.regs[0] != MC_PIPE_STATUS_OK) {
        return (uint32_t)reply.regs[0];
    }
    if (out_written != 0) {
        *out_written = reply.regs[1];
    }
    return MC_PIPE_STATUS_OK;
}

uint32_t mc_pipe_close(uint32_t pipesrv_handle, uint64_t id) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PIPE_OP_CLOSE;
    req.regs[0] = id;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(pipesrv_handle, &req, &reply);
    return (uint32_t)reply.regs[0];
}

uint32_t mc_pipe_dup(uint32_t pipesrv_handle, uint64_t id) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_PIPE_OP_DUP;
    req.regs[0] = id;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(pipesrv_handle, &req, &reply);
    return (uint32_t)reply.regs[0];
}
