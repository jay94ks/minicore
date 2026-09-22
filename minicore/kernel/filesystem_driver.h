#ifndef MINICORE_KERNEL_FILESYSTEM_DRIVER_H
#define MINICORE_KERNEL_FILESYSTEM_DRIVER_H

#include "mount_table.h"

namespace fs {
class BlockDevice;
}

// SP-2BCE5D60 §3.1 - ext4/FAT류 블록 장치 기반 파일시스템 드라이버의
// 공통 인터페이스. [전면 정정, 2026-09-22, QU-08ACD701 설계자 답변
// ("양쪽 모두를 수정하면서 구현해")] 원안은 독자적인 동기 가상함수
// 9개짜리 인터페이스였으나, `PN-22784AD4`(libext4)가 실제 코드와
// 대조하며 그 인터페이스가 한 번도 코드로 존재한 적이 없다는 걸
// 발견했다 - `fs`가 `SP-43331889`로 순수 커널 `KernelThread`에 흡수된
// 뒤 `LiveFs`/`ProcFs`/`ResourceGroupFs` 전부 이미 `KernelFsDriver`
// (`AsyncTaskHandler` 상속, `KernelFsOpCode` 태그 기반 비동기 op
// 제출) 패턴으로 구현돼 있었다. 그래서 `FileSystemDriver`는 독자
// 인터페이스가 아니라 `KernelFsDriver`를 그대로 확장하는 것으로
// 재정의됐다 - 타입/오퍼레이션(`VfsError`/`FileHandle`/`OpenResult`/
// `ReadResult`/`VfsDirEntry`/`KernelFsOpCode` 9종 Args)은 전부
// `mount_table.h`가 이미 확정해 둔 것을 그대로 재사용한다.
//
// `mount(BlockDevice*, readOnly)`/`remount(writable)`은 블록 장치가
// 필요 없는 `KernelFsDriver`(LiveFs/ProcFs류)와 ext4/FAT류의 유일한
// 차이 - 파일 op 하나하나처럼 매번 `AsyncTask::submit()`으로 제출되는
// 요청이 아니라, `MountTable::mountKernel()` 등록 전/후에 동기적으로
// 한 번 호출되는 준비 단계라 일반 가상함수로 남겨 둔다(`onExec`/
// `onFailure`/`onCancel`은 각 드라이버가 `KernelFsDriver`로부터
// 그대로 구현).
namespace kernel {

class FileSystemDriver : public KernelFsDriver {
public:
    // readOnly: 부팅 초기 임시 읽기전용 마운트 지원(SP-2BCE5D60 §5.1).
    virtual bool mount(fs::BlockDevice* device, bool readOnly) = 0;
    // readOnly=true로 마운트된 대상을 쓰기 가능으로 전환(§5 - init이
    // /sys/etc/mtab을 읽은 뒤 호출) - 이미 writable이면 아무 효과 없이 true.
    virtual bool remount(bool writable) = 0;
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_FILESYSTEM_DRIVER_H
