// boot::dump — klog으로 boot_info를 사람이 읽을 수 있게 출력한다.
// arch 독립(ADR-002 HAL 경계) — boot_info.hpp와 klog.hpp만 안다.
#include "boot_info.hpp"
#include "klog.hpp"

namespace boot {

void dump(const char* tag, const boot_info& info, const memory_region* regions) {
    bool magic_ok = info.magic == k_boot_info_magic;

    kern::klog::printf("[boot_info:%s] magic=0x%lx(%s) version=%u cpu_count=%u\n", tag,
                 static_cast<unsigned long>(info.magic), magic_ok ? "ok" : "MISMATCH",
                 info.version, info.cpu_count);
    kern::klog::printf("[boot_info:%s] memory_map_count=%u numa_node_count=%u\n", tag,
                 info.memory_map_count, info.numa_node_count);

    for (uint32_t i = 0; i < info.memory_map_count; ++i) {
        const memory_region& r = regions[i];
        kern::klog::printf("[boot_info:%s]   region[%u] base=0x%lx length=0x%lx type=%u node=%u\n",
                     tag, i, static_cast<unsigned long>(r.base),
                     static_cast<unsigned long>(r.length), r.type, r.node_id);
    }

    kern::klog::printf("[boot_info:%s] initrd_addr=0x%lx initrd_size=0x%lx\n", tag,
                 static_cast<unsigned long>(info.initrd_addr),
                 static_cast<unsigned long>(info.initrd_size));
    kern::klog::printf("[boot_info:%s] cmdline_addr=0x%lx arch_data_addr=0x%lx\n", tag,
                 static_cast<unsigned long>(info.cmdline_addr),
                 static_cast<unsigned long>(info.arch_data_addr));
}

}  // namespace boot
