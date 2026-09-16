#ifndef MINICORE_KERNEL_LIVEFS_H
#define MINICORE_KERNEL_LIVEFS_H

#include "libkenv/types.h"
#include "mount_table.h"

namespace kernel {

// `/sys/live/kernel/<name>` 예약 테이블(SP-00CA7175 §2.0) - 커널이
// 부팅 극초반(유저 프로세스가 스케줄되기 전) devmgr/fs/net/tty 각각을
// 위해 미리 만들어 두는 Tier A/B 자리. `/sys/live/named/`(named_object.h,
// NamedObjectTable)와 달리 **경쟁적 네임스페이스가 아니다** - reserve는
// syscall로 노출되지 않고 커널 자신의 부팅 코드만 호출하는 내부 API다
// (mountKernel()과 같은 성격). 아래 `LiveFs::open("kernel/<name>")`가
// 이 테이블을 조회하되, 호출자의 ProcessRole이 KernelService이고
// 스폰 이름이 <name>과 정확히 일치할 때만 성공시킨다(이 테이블 자신은
// 그 권한 검사를 하지 않는다).
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

    // 이름으로 조회 - 없으면 nullptr. `LiveFs::open()`이 이 함수로
    // 조회한 뒤 호출자 ProcessRole/스폰 이름 검사를 추가한다(§2.0,
    // 이 함수 자신은 그 권한 검사를 하지 않는다).
    static KernelReservedEntry* find(const char* name, uint32_t nameLen);
};

// initrd.cpio 아카이브 원본 전체를 담는 v1 상한(SP-7CC5693A §2.4,
// PN-71C2B857) - gInitImageBuffer류(kmain.cpp)와 같은 이유로 개별
// 엔트리가 아니라 아카이브 원본 바이트 전체를 보존해야 한다(하나의
// 불투명한 파일로 노출하므로). init+devmgr+fs+net+tty 5개 이미지
// 각각의 1MiB 상한(kMaxInitImageSize)을 다 합친 것보다 여유 있게.
constexpr uint64_t kMaxLiveFsCpioSize = 8UL * 1024UL * 1024UL;  // 8MiB v1 상한(실측 후 조정)

// `/sys/live`에 마운트되는 커널 자체 구현 드라이버(SP-7CC5693A §2.4,
// PN-71C2B857) - 세 하위 경로를 하나의 마운트 아래 통합한다:
//   - `named/<name>`  -> NamedObjectTable에 위임(SP-1FBC0EEB)
//   - `initrd.cpio`   -> captureCpioArchive()가 보존해 둔 원본 바이트
//                        전체를 하나의 불투명한 파일로 노출
//   - `kernel/<name>` -> KernelReservedTable 조회 + 호출자 신원 검사
//                        (SP-00CA7175 §2.0)
// [수정, 2026-09-17, PN-BC04D3DC] `KernelFsDriver`가 이제
// `AsyncTaskHandler`를 상속하므로(mount_table.h 문서 주석 참고)
// open/close/read 가상함수 대신 `onExec()` 하나로 9개 op 전부를
// 받는다 - 여전히 이 드라이버를 실제로 호출하는 syscall 경로(Open/
// Read 등, SP-2AAD7C8D §9)는 없다(`MountTable::mountKernel()`로
// 마운트+`AsyncCallbackRegistry` 등록까지는 걸리지만, §9 착수 이후에야
// 진짜 유저 syscall이 `AsyncTask::submit(subjectCode(), ...)`로 이
// onExec을 부르게 된다) - 이번 증분은 인터페이스 변환 자체가 범위.
class LiveFs : public KernelFsDriver {
public:
    // 이 프로세스 전체에서 딱 하나만 존재하는 인스턴스 - `MountTable::
    // mountKernel()`에 넘길 `KernelFsDriver*`가 여기서 나온다.
    static LiveFs& instance();

    // 부팅 시(kmain.cpp의 kLogBootInfo, PageFrameAllocator::init()보다
    // 먼저) initrd CPIO 모듈을 발견하면 한 번 호출 - 그 원본 바이트
    // 전체를 커널 BSS 안의 전용 버퍼로 복사해 보존한다(QU-A7D8E49B와
    // 같은 이유 - 부트 모듈의 물리 프레임은 PageFrameAllocator가
    // 예약 목록에 넣지 않아 나중에 재활용될 수 있음). 이미 한 번
    // 성공했으면(첫 CPIO 모듈만 채택) 이후 호출은 무시. archiveSize가
    // `kMaxLiveFsCpioSize`를 넘으면 실패.
    static bool captureCpioArchive(const void* archive, uint64_t archiveSize);

    AsyncExecCoro onExec(AsyncTask* task, void* argsRaw) override;
    void onFailure(AsyncTask*) override {}
    void onCancel(AsyncTask*, void*) override {}
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_LIVEFS_H
