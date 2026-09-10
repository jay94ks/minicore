#include <mc/cfgsrv_client.h>

#include <mc/syscall.h>
#include <mc/util.h>

// servers/procsrv/main.cpp의 자기테스트가 같은 프로토콜을 인라인할
// 때 쓴 것과 같은 자리 — 페이지 전체를 넘기되 실제 길이는 별도
// regs로 알려준다(경로/키/값 각각 최대 한 페이지).
#define MC_CFG_PAGE_SIZE 4096u

// 페이지 정렬 필수(kernel/core/ipc/endpoint.cpp가 page_descriptor의
// vaddr을 4096바이트 경계로 강제한다 — 이 정렬 없이 M41 실행 중
// 실제로 커널 패닉("page_descriptor not page-aligned")을 겪었다,
// servers/procsrv/main.cpp의 같은 용도 버퍼들이 이미 alignas를
// 쓰고 있던 이유가 바로 이거였다).
_Alignas(MC_CFG_PAGE_SIZE) static uint8_t g_cfg_path_buf[MC_CFG_PAGE_SIZE];
_Alignas(MC_CFG_PAGE_SIZE) static uint8_t g_cfg_key_buf[MC_CFG_PAGE_SIZE];
_Alignas(MC_CFG_PAGE_SIZE) static uint8_t g_cfg_value_buf[MC_CFG_PAGE_SIZE];

static void fill_page_buf(uint8_t* buf, const char* text) {
    mc_pack_bytes(buf, MC_CFG_PAGE_SIZE, text, mc_cstr_len(text));
}

uint64_t mc_reg_open_or_create(uint32_t cfgsrv_handle, uint32_t op, uint32_t caller_uid,
                                const char* caller_username, const char* path,
                                uint64_t* out_table_id) {
    fill_page_buf(g_cfg_path_buf, path);
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = op;
    req.regs[0] = caller_uid;
    mc_pack_bytes(&req.regs[1], sizeof(uint64_t), caller_username, mc_cstr_len(caller_username));
    req.page_count = 1;
    req.pages[0].vaddr = (uint64_t)(uintptr_t)g_cfg_path_buf;
    req.pages[0].length = MC_CFG_PAGE_SIZE;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(cfgsrv_handle, &req, &reply);
    if (out_table_id != 0) {
        *out_table_id = reply.regs[1];
    }
    return reply.regs[0];
}

uint64_t mc_reg_get_binary(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                            const char* key, void* out_buf, uint64_t out_cap,
                            uint64_t* out_len) {
    fill_page_buf(g_cfg_key_buf, key);
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_REG_OP_GET_VALUE;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = (uint64_t)(uintptr_t)g_cfg_key_buf;
    req.pages[0].length = MC_CFG_PAGE_SIZE;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(cfgsrv_handle, &req, &reply);
    if (reply.regs[0] != MC_REG_ERR_OK) {
        return reply.regs[0];
    }
    uint64_t len = reply.regs[2];
    if (len > out_cap) {
        len = out_cap;
    }
    const uint8_t* src = (const uint8_t*)(uintptr_t)reply.pages[0].vaddr;
    for (uint64_t i = 0; i < len; ++i) {
        ((uint8_t*)out_buf)[i] = src[i];
    }
    if (out_len != 0) {
        *out_len = len;
    }
    return MC_REG_ERR_OK;
}

uint64_t mc_reg_set_binary(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                            const char* key, const void* value, uint64_t value_len) {
    fill_page_buf(g_cfg_key_buf, key);
    uint64_t copy_len = value_len > MC_CFG_PAGE_SIZE ? MC_CFG_PAGE_SIZE : value_len;
    mc_pack_bytes(g_cfg_value_buf, MC_CFG_PAGE_SIZE, (const char*)value, copy_len);
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_REG_OP_SET_VALUE;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.regs[2] = MC_REG_TYPE_BINARY;
    req.regs[3] = copy_len;
    req.page_count = 2;
    req.pages[0].vaddr = (uint64_t)(uintptr_t)g_cfg_key_buf;
    req.pages[0].length = MC_CFG_PAGE_SIZE;
    req.pages[0].mode = MC_TRANSFER_COPY;
    req.pages[1].vaddr = (uint64_t)(uintptr_t)g_cfg_value_buf;
    req.pages[1].length = MC_CFG_PAGE_SIZE;
    req.pages[1].mode = MC_TRANSFER_COPY;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(cfgsrv_handle, &req, &reply);
    return reply.regs[0];
}

uint64_t mc_reg_delete_value(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                              const char* key) {
    fill_page_buf(g_cfg_key_buf, key);
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_REG_OP_DELETE_VALUE;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    req.page_count = 1;
    req.pages[0].vaddr = (uint64_t)(uintptr_t)g_cfg_key_buf;
    req.pages[0].length = MC_CFG_PAGE_SIZE;
    req.pages[0].mode = MC_TRANSFER_COPY;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(cfgsrv_handle, &req, &reply);
    return reply.regs[0];
}

uint64_t mc_reg_list_values(uint32_t cfgsrv_handle, uint32_t caller_uid, uint64_t table_id,
                             char* out_names_blob, uint64_t out_cap, uint64_t* out_count) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_REG_OP_LIST_VALUES;
    req.regs[0] = caller_uid;
    req.regs[1] = table_id;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(cfgsrv_handle, &req, &reply);
    if (reply.regs[0] != MC_REG_ERR_OK) {
        return reply.regs[0];
    }
    if (out_count != 0) {
        *out_count = reply.regs[1];
    }
    if (out_names_blob != 0 && out_cap > 0) {
        const uint8_t* src = (const uint8_t*)(uintptr_t)reply.pages[0].vaddr;
        for (uint64_t i = 0; i < out_cap; ++i) {
            out_names_blob[i] = (char)src[i];
        }
    }
    return MC_REG_ERR_OK;
}
