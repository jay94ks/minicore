#include <mc/lifecycle_client.h>

#include <mc/syscall.h>
#include <mc/util.h>

void mc_signal_ready(void) {
    mc_message req;
    mc_zero_bytes(&req, sizeof(req));
    req.label = MC_SERVICE_READY_LABEL;
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_call(MC_SERVICE_READY_OWN_HANDLE, &req, &reply);
}

void mc_wait_ready(uint32_t endpoint_proxy_handle) {
    mc_message ask;
    mc_zero_bytes(&ask, sizeof(ask));
    mc_ipc_recv(endpoint_proxy_handle, &ask);
    mc_message reply;
    mc_zero_bytes(&reply, sizeof(reply));
    mc_ipc_reply(&reply);
}
