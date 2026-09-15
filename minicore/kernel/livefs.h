#ifndef MINICORE_KERNEL_LIVEFS_H
#define MINICORE_KERNEL_LIVEFS_H

#include "libkenv/types.h"

namespace kernel {

// `/sys/live/kernel/<name>` 예약 테이블(SP-00CA7175 §2.0) - 커널이
// 부팅 극초반(유저 프로세스가 스케줄되기 전) devmgr/fs/net/tty 각각을
// 위해 미리 만들어 두는 Tier A/B 자리. `/sys/live/named/`(named_object.h,
// NamedObjectTable)와 달리 **경쟁적 네임스페이스가 아니다** - reserve는
// syscall로 노출되지 않고 커널 자신의 부팅 코드만 호출하는 내부 API다
// (mountKernel()과 같은 성격). livefs(`LiveFs : KernelFsDriver`,
// SP-7CC5693A §2.4, 아직 미구현 - PN-71C2B857)가 실제로 존재하게 되면
// `LiveFs::open("kernel/<name>")`가 이 테이블을 조회하되, 호출자의
// ProcessRole이 KernelService이고 스폰 이름이 <name>과 정확히 일치할
// 때만 성공시켜야 한다(그 전까지는 이 테이블 자체만 존재 - 아직 어떤
// VFS 경로도 이 테이블을 실제로 조회하지 않는다).
constexpr uint32_t kMaxKernelReservedNameLen = 32;
constexpr uint32_t kMaxKernelReservedEntries = 8;  // v1 상한(devmgr/fs/net/tty 4개 + 여유)

struct KernelReservedEntry {
    char name[kMaxKernelReservedNameLen] = {};
    uint32_t nameLen = 0;
    void* tierA = nullptr;           // KernelServiceSharedRingBuffer* - 없으면 nullptr(할당 실패 시)
    uint64_t tierBChannelId = 0;     // Tier B Channel의 ChannelId - 0이면 미배정
    bool used = false;
};

class KernelReservedTable {
public:
    // 부팅 시 한 번(BSP) - 테이블을 빈 상태로 리셋한다.
    static void init();

    // 커널 부팅 코드 전용(kmain.cpp의 kSpawnServiceProcesses 직후,
    // 실제로 스폰된 서비스마다 한 번씩 호출) - Tier A 링버퍼 +
    // Tier B Channel(exclusivePreemptive=true)을 새로 만들어 이 표에
    // 등록한다. syscall 아님 - 유저랜드에서 호출할 방법이 없다.
    static bool reserveForKernelService(const char* name, uint32_t nameLen);

    // 이름으로 조회 - 없으면 nullptr. livefs의 LiveFs::open() 구현이
    // 착수되면 이 함수로 조회한 뒤 호출자 ProcessRole/스폰 이름 검사를
    // 추가한다(§2.0, 이 함수 자신은 그 권한 검사를 하지 않는다).
    static KernelReservedEntry* find(const char* name, uint32_t nameLen);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LIVEFS_H
