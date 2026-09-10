// 부트 디바이스(virtio-blk) 전용 최소 PCI BAR 배정 (docs/plan/
// system-servers-bringup.md §M12, ADR-131/147). devmgr(M14~M16)의
// "진짜" PCI 버스 열거(ADR-039)와는 완전히 별개다 — 딱 하나의 이미
// 알고 있는 BDF(disk.cfg가 알려 준다)만 다룬다.
//
// 이 필요성 자체가 실측으로 드러났다: 이 프로젝트는 QEMU PVH 직접
// 부팅(ADR-114)이라 SeaBIOS 등 어떤 펌웨어도 PCI 리소스(BAR) 배정을
// 대신해 주지 않는다 — bus0/device4/function0(virtio-blk-pci)의
// BAR0을 실제로 읽어 보면 IO space 비트(bit0=1)만 리셋 상태로 서 있고
// 주소 부분은 0이다. 즉 **커널이 직접** 크기 탐색(0xFFFFFFFF를 써
// 보고 되읽어 마스크 확인) + 주소 배정 + PCI COMMAND 레지스터의 I/O
// space enable 비트까지 켜 줘야 한다.
#pragma once

#include <cstdint>

namespace kern::arch::x86_64 {

struct pci_bar_result {
    bool ok;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t io_port_base;  // BAR0(I/O space)에 배정한 포트 베이스. ok==false면 의미 없음.
};

// ecam_base(find_and_parse_mcfg()의 결과)로 bus/device/function이
// 가리키는 장치의 BAR0을 I/O 공간에 배정하고, PCI COMMAND 레지스터의
// I/O space enable(bit0)을 켠다. BAR0이 이미 I/O 공간이 아니거나
// (bit0==0, 메모리 BAR) vendor_id가 virtio(0x1AF4)가 아니면 실패.
pci_bar_result assign_virtio_blk_bar(uint64_t ecam_base, uint32_t bus, uint32_t device,
                                      uint32_t function);

}  // namespace kern::arch::x86_64
