// servers/drivers/usb/main.cpp — USB(xHCI) 드라이버
// (docs/plan/system-servers-bringup.md §M14, ADR-130 — devmgr의
// "중첩 버스 열거" 아래에 최소 HID 클래스 드라이버를 얹는 계획의
// 첫 단계).
//
// M14 최소 버전 — devmgr에게 xHCI(PCI 클래스 코드 0x0C0330, USB3
// 표준 프로그래밍 인터페이스)를 등록해 BAR0을 위임받고, 그 MMIO를
// sys_map_phys로 매핑해 xHCI 캐패빌리티 레지스터를 읽어 로그로
// 남긴다. 여기서 한 걸음 더 나가 **컨트롤러 리셋(HCRST)**을 실제로
// 시도하고, 리셋이 끝나면 각 포트의 PORTSC(포트 상태/제어) 레지스터
// 값을 읽어 로그로 남긴다 — 실제 USB 장치 열거(SET_ADDRESS/
// GET_DESCRIPTOR)와 HID 클래스 드라이버는 **여기서 멈춘다**(범위
// 밖으로 명시적으로 남김, docs/done/system-servers-bringup-m14.md
// 참고) — xHCI 명령/이벤트 링(TRB 큐)까지 다뤄야 해서 이 자체로
// 별도 마일스톤급 작업이다.
#include <uapi.hpp>

namespace kernsrv::drivers::usb {

namespace {

constexpr uint32_t k_devmgr_handle = 2;  // depends=devmgr(lib/*.ini) — initrun이 물려준 프록시.

constexpr uint32_t k_op_register_driver = 1;
constexpr uint64_t k_match_mode_class_code = 1;
constexpr uint64_t k_status_ok = 0;

// PCI 클래스 코드 (base<<16)|(subclass<<8)|prog_if. xHCI는
// base=0x0C(직렬버스 컨트롤러), subclass=0x03(USB), prog_if=0x30
// (xHCI) — prog_if까지 정확히 맞춰야 EHCI(0x20)/UHCI(0x00)와
// 구분된다.
constexpr uint64_t k_class_xhci = 0x0C0330ull;
constexpr uint64_t k_class_mask_exact = 0xFFFFFFull;

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
void debug_log_hex(const char* prefix, uint64_t value) {
    char buf[96];
    uint64_t i = 0;
    for (; prefix[i] != '\0' && i < 60; ++i) {
        buf[i] = prefix[i];
    }
    buf[i++] = '0';
    buf[i++] = 'x';
    bool leading = true;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0xF);
        if (nibble != 0 || !leading || shift == 0) {
            leading = false;
            buf[i++] = static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
        }
    }
    buf[i++] = '\n';
    do_syscall(uapi::k_syscall_debug_log, reinterpret_cast<uint64_t>(buf), i, 0);
}

uint32_t read_reg32(const void* base, uint32_t offset) {
    return *reinterpret_cast<volatile const uint32_t*>(static_cast<const uint8_t*>(base) + offset);
}
void write_reg32(const void* base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(base)) +
                                           offset) = value;
}

// xHCI 운영 레지스터(Operational Registers, xHCI 1.2 §5.4) 오프셋.
constexpr uint32_t k_op_usbcmd = 0x00;
constexpr uint32_t k_op_usbsts = 0x04;
constexpr uint32_t k_op_portsc_base = 0x400;  // PORTSC[n] = base + (n-1)*0x10.
constexpr uint32_t k_usbcmd_run_stop = 1u << 0;
constexpr uint32_t k_usbcmd_hcrst = 1u << 1;
constexpr uint32_t k_usbsts_cnr = 1u << 11;  // Controller Not Ready.

}  // namespace

extern "C" [[noreturn]] void _start(const void*) {
    uapi::message req{};
    req.label = k_op_register_driver;
    req.regs[0] = k_match_mode_class_code;
    req.regs[1] = k_class_xhci;
    req.regs[2] = k_class_mask_exact;
    uapi::message reply{};
    do_syscall(uapi::k_syscall_ipc_call, k_devmgr_handle, reinterpret_cast<uint64_t>(&req),
               reinterpret_cast<uint64_t>(&reply));

    if (reply.regs[0] != k_status_ok) {
        debug_log("[usb] no xHCI controller registered by devmgr\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }

    uint64_t bar_phys = reply.regs[1];
    uint64_t bar_size = reply.regs[2];
    debug_log_hex("[usb] xhci bar_phys=", bar_phys);
    debug_log_hex("[usb] xhci bar_size=", bar_size);

    uapi::map_phys_request map_req{};
    map_req.phys_addr = bar_phys;
    map_req.size = bar_size;
    uint64_t map_err = do_syscall(uapi::k_syscall_map_phys, reinterpret_cast<uint64_t>(&map_req), 0, 0);
    if (map_err != 0) {
        debug_log("[usb] map_phys failed\n");
        do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    }

    const void* cap_base = reinterpret_cast<const void*>(map_req.out_virt_addr);

    // xHCI 캐패빌리티 레지스터(Capability Registers, xHCI 1.2 §5.3).
    uint8_t cap_length = static_cast<uint8_t>(read_reg32(cap_base, 0x00) & 0xFF);
    uint16_t hci_version = static_cast<uint16_t>((read_reg32(cap_base, 0x00) >> 16) & 0xFFFF);
    uint32_t hcs_params1 = read_reg32(cap_base, 0x04);
    uint32_t max_slots = hcs_params1 & 0xFF;
    uint32_t max_ports = (hcs_params1 >> 24) & 0xFF;

    debug_log_hex("[usb] xhci hci_version=", hci_version);
    debug_log_hex("[usb] xhci max_slots=", max_slots);
    debug_log_hex("[usb] xhci max_ports=", max_ports);

    // 운영 레지스터 베이스 = 캐패빌리티 베이스 + CAPLENGTH.
    void* op_base = reinterpret_cast<uint8_t*>(map_req.out_virt_addr) + cap_length;

    // 컨트롤러 리셋(xHCI 1.2 §4.2) — 먼저 Run/Stop(R/S)을 0으로 내려
    // 정지시키고(이미 정지 상태일 수도 있다 — PVH 직접 부팅이라
    // 펌웨어가 미리 기동해 두지 않았다), HCRST 비트를 세운 뒤
    // 컨트롤러가 스스로 0으로 내릴 때까지 기다린다. 그다음 USBSTS의
    // CNR(Controller Not Ready)이 꺼질 때까지 기다려야 레지스터
    // 접근이 안전해진다.
    write_reg32(op_base, k_op_usbcmd, read_reg32(op_base, k_op_usbcmd) & ~k_usbcmd_run_stop);
    write_reg32(op_base, k_op_usbcmd, read_reg32(op_base, k_op_usbcmd) | k_usbcmd_hcrst);

    bool reset_done = false;
    for (uint32_t i = 0; i < 10'000'000; ++i) {
        if ((read_reg32(op_base, k_op_usbcmd) & k_usbcmd_hcrst) == 0) {
            reset_done = true;
            break;
        }
    }
    debug_log_hex("[usb] xhci hcrst_done=", reset_done ? 1 : 0);

    bool ready = false;
    if (reset_done) {
        for (uint32_t i = 0; i < 10'000'000; ++i) {
            if ((read_reg32(op_base, k_op_usbsts) & k_usbsts_cnr) == 0) {
                ready = true;
                break;
            }
        }
    }
    debug_log_hex("[usb] xhci controller_ready=", ready ? 1 : 0);

    if (ready) {
        uint32_t ports_to_log = max_ports;
        if (ports_to_log > 8) {
            ports_to_log = 8;  // 로그 폭주 방지 — M14 검증에는 몇 개만 있어도 충분하다.
        }
        for (uint32_t port = 1; port <= ports_to_log; ++port) {
            uint32_t portsc = read_reg32(op_base, k_op_portsc_base + (port - 1) * 0x10);
            debug_log_hex("[usb] portsc=", portsc);
        }
    }

    // 실제 USB 장치 열거(디바이스 컨텍스트 배열, 명령/이벤트 링 설정,
    // SET_ADDRESS/GET_DESCRIPTOR)와 HID 클래스 드라이버는 여기서
    // 멈춘다(이 파일 상단 주석 — 별도 마일스톤급 작업).
    debug_log("[usb] xhci reset+port scan done\n");

    do_syscall(uapi::k_syscall_thread_exit, 0, 0, 0);
    for (;;) {
        asm volatile("pause");
    }
}

}  // namespace kernsrv::drivers::usb
