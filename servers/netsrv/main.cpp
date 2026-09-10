// servers/netsrv/main.cpp — 네트워크 서버 최소 구현
// (docs/plan/general-purpose-completion.md §M25).
//
// servers/drivers/virtio-net(순수 하드웨어 계층 — 이더넷 프레임
// 송수신만 안다)에 IPC로 의존해, 이 프로세스가 이더넷/ARP/IP/UDP
// 프로토콜 자체를 구현한다(ADR-002/006/008 — vfs가 경로 라우팅,
// fat32/ext4가 온디스크 포맷을 각각 담당하는 것과 같은 계층
// 분리). 이번 라운드의 검증 목표는 "UDP 패킷 하나를 왕복"뿐이라
// 실제 소켓 API(bind/connect/send/recv)는 만들지 않는다 — 그
// 왕복 자체를 이 프로세스의 부팅 시 자기테스트로 증명한다(다른
// 서버들의 self-test 관례와 동일).
//
// 왕복 대상으로 ARP가 아니라 DHCP(UDP 67/68)를 골랐다 — QEMU의
// usermode 네트워킹(SLIRP, MINICORE_QEMU_NET=1)이 내장 DHCP
// 서버를 항상 갖고 있어 외부 네트워크/인터넷 접근 없이도 결정적으로
// 응답이 오고, DHCPDISCOVER는 이더넷/IP 둘 다 브로드캐스트라 ARP로
// 목적지 MAC을 먼저 구할 필요조차 없다(이 라운드가 ARP를 구현하지
// 않는 이유).
#include <uapi.hpp>

namespace kernsrv::netsrv {

namespace {

constexpr uint32_t k_virtio_net_handle = 2;  // depends=virtio-net(lib/*.ini).

constexpr uint32_t k_op_send_frame = 1;
constexpr uint32_t k_op_recv_frame = 2;
constexpr uint32_t k_op_get_mac = 3;

constexpr uint64_t k_page_size = 4096;
constexpr uint64_t k_max_frame_size = 1514;

alignas(k_page_size) uint8_t g_tx_scratch[k_page_size] = {};

uint64_t do_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 asm("r10") = a3;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "D"(num), "S"(a1), "d"(a2), "r"(r10)
                 : "rcx", "r11", "memory");
    return ret;
}

uint64_t cstr_len(const char* s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}
void debug_log(const char* msg) {
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(msg), cstr_len(msg), 0);
}

void put_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
uint16_t get_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t get_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// 인터넷 체크섬(RFC 1071) — IPv4 헤더 검증에 쓴다. UDP 체크섬은
// 0(비활성, RFC 768이 허용)으로 보내 계산하지 않는다.
uint16_t internet_checksum(const uint8_t* data, uint64_t len) {
    uint32_t sum = 0;
    uint64_t i = 0;
    for (; i + 1 < len; i += 2) {
        sum += get_be16(data + i);
    }
    if (i < len) {
        sum += static_cast<uint32_t>(data[i]) << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

bool send_frame(uint64_t length) {
    uapi::message req{};
    req.label = k_op_send_frame;
    req.regs[0] = length;
    req.page_count = 1;
    req.pages[0].vaddr = reinterpret_cast<uint64_t>(g_tx_scratch);
    req.pages[0].length = k_page_size;
    req.pages[0].mode = uapi::transfer_mode::copy;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_virtio_net_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    return reply.regs[0] == 0;
}

// out_buf는 최소 k_max_frame_size바이트. 반환: 길이(0=이번 시도에
// 아무것도 안 옴).
uint64_t recv_frame(uint8_t* out_buf) {
    uapi::message req{};
    req.label = k_op_recv_frame;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_virtio_net_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    if (reply.regs[0] != 0 || reply.page_count != 1) {
        return 0;
    }
    uint64_t len = reply.regs[1];
    if (len > k_max_frame_size) {
        len = k_max_frame_size;
    }
    const auto* src = reinterpret_cast<const uint8_t*>(reply.pages[0].vaddr);
    for (uint64_t i = 0; i < len; ++i) {
        out_buf[i] = src[i];
    }
    return len;
}

uint64_t get_mac(uint8_t out_mac[6]) {
    uapi::message req{};
    req.label = k_op_get_mac;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_virtio_net_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));
    __builtin_memcpy(out_mac, &reply.regs[0], 6);
    return 0;
}

constexpr uint16_t k_ethertype_ipv4 = 0x0800;
constexpr uint8_t k_ip_proto_udp = 17;
constexpr uint16_t k_dhcp_client_port = 68;
constexpr uint16_t k_dhcp_server_port = 67;
constexpr uint32_t k_dhcp_magic_cookie = 0x63825363;
constexpr uint32_t k_dhcp_xid = 0x4D435231;  // "MCR1" — 이 왕복만의 식별자.

// DHCPDISCOVER 하나를 담은 완전한 이더넷 프레임을 g_tx_scratch에
// 만들고 길이를 반환한다. 이더넷/IP 둘 다 브로드캐스트라 ARP가
// 필요 없다(이 파일 상단 주석).
uint64_t build_dhcp_discover(const uint8_t src_mac[6]) {
    uint8_t* eth = g_tx_scratch;
    for (int i = 0; i < 6; ++i) {
        eth[i] = 0xFF;  // dst=브로드캐스트.
    }
    __builtin_memcpy(eth + 6, src_mac, 6);
    put_be16(eth + 12, k_ethertype_ipv4);

    constexpr uint64_t k_bootp_len = 300;  // 236(고정부)+cookie4+옵션+패딩.
    constexpr uint64_t k_udp_len = 8 + k_bootp_len;
    constexpr uint64_t k_ip_len = 20 + k_udp_len;

    uint8_t* ip = eth + 14;
    ip[0] = 0x45;  // version=4, ihl=5(20바이트, 옵션 없음).
    ip[1] = 0;     // tos.
    put_be16(ip + 2, static_cast<uint16_t>(k_ip_len));
    put_be16(ip + 4, 0);   // identification.
    put_be16(ip + 6, 0);   // flags+fragment offset.
    ip[8] = 64;            // ttl.
    ip[9] = k_ip_proto_udp;
    put_be16(ip + 10, 0);  // checksum(우선 0, 아래서 계산해 채운다).
    put_be32(ip + 12, 0);          // src=0.0.0.0(아직 IP 없음).
    put_be32(ip + 16, 0xFFFFFFFFu);  // dst=255.255.255.255.
    uint16_t ip_csum = internet_checksum(ip, 20);
    put_be16(ip + 10, ip_csum);

    uint8_t* udp = ip + 20;
    put_be16(udp + 0, k_dhcp_client_port);
    put_be16(udp + 2, k_dhcp_server_port);
    put_be16(udp + 4, static_cast<uint16_t>(k_udp_len));
    put_be16(udp + 6, 0);  // checksum=0(비활성, RFC 768).

    uint8_t* bootp = udp + 8;
    for (uint64_t i = 0; i < k_bootp_len; ++i) {
        bootp[i] = 0;
    }
    bootp[0] = 1;  // op=BOOTREQUEST.
    bootp[1] = 1;  // htype=ethernet.
    bootp[2] = 6;  // hlen.
    bootp[3] = 0;  // hops.
    put_be32(bootp + 4, k_dhcp_xid);
    put_be16(bootp + 8, 0);       // secs.
    put_be16(bootp + 10, 0x8000);  // flags: broadcast(아직 유니캐스트로 받을 IP가 없다).
    // ciaddr/yiaddr/siaddr/giaddr(12~27) — 이미 0.
    __builtin_memcpy(bootp + 28, src_mac, 6);  // chaddr 앞 6바이트.
    // sname(44)/file(108~235) — 이미 0.
    put_be32(bootp + 236, k_dhcp_magic_cookie);
    bootp[240] = 53;  // option 53 = DHCP message type.
    bootp[241] = 1;   // length=1.
    bootp[242] = 1;   // DHCPDISCOVER.
    bootp[243] = 255;  // end option.
    // 나머지(244~299)는 이미 0으로 패딩됨.

    return 14 + k_ip_len;
}

// frame이 우리 DHCPDISCOVER(k_dhcp_xid)에 대한 BOOTREPLY(DHCPOFFER류)
// 인지 확인한다 — 엄밀한 DHCP 옵션 파싱은 하지 않는다(이 라운드의
// 검증 목표는 "UDP 패킷 하나가 왕복했는가"뿐이다).
bool is_our_dhcp_reply(const uint8_t* frame, uint64_t len) {
    if (len < 14 + 20 + 8 + 240) {
        return false;
    }
    if (get_be16(frame + 12) != k_ethertype_ipv4) {
        return false;
    }
    const uint8_t* ip = frame + 14;
    if ((ip[0] >> 4) != 4 || ip[9] != k_ip_proto_udp) {
        return false;
    }
    uint64_t ihl_bytes = static_cast<uint64_t>(ip[0] & 0x0F) * 4;
    const uint8_t* udp = ip + ihl_bytes;
    if (get_be16(udp + 0) != k_dhcp_server_port || get_be16(udp + 2) != k_dhcp_client_port) {
        return false;
    }
    const uint8_t* bootp = udp + 8;
    if (bootp[0] != 2 /* BOOTREPLY */) {
        return false;
    }
    return get_be32(bootp + 4) == k_dhcp_xid;
}

void run_dhcp_roundtrip_test() {
    uint8_t mac[6];
    get_mac(mac);
    debug_log("[netsrv] got mac from virtio-net\n");

    uint64_t frame_len = build_dhcp_discover(mac);
    bool sent = send_frame(frame_len);
    debug_log(sent ? "[netsrv] dhcp discover sent=1\n" : "[netsrv] dhcp discover sent=0\n");
    if (!sent) {
        return;
    }

    // 응답이 올 때까지 여러 번 시도한다 — SLIRP의 다른 방송 트래픽
    // (예: 자기 자신의 하우스키핑)이 먼저 잡힐 수 있어 매번 프레임을
    // 검사하고 우리 것이 아니면 계속 기다린다.
    constexpr uint32_t k_max_attempts = 50;
    uint8_t buf[k_max_frame_size];
    bool ok = false;
    for (uint32_t attempt = 0; attempt < k_max_attempts; ++attempt) {
        uint64_t len = recv_frame(buf);
        if (len > 0 && is_our_dhcp_reply(buf, len)) {
            ok = true;
            break;
        }
    }
    debug_log(ok ? "[netsrv] udp roundtrip ok=1\n" : "[netsrv] udp roundtrip ok=0\n");
}

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    run_dhcp_roundtrip_test();
    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace kernsrv::netsrv
