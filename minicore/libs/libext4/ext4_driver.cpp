#include "ext4_driver.h"

namespace ext4 {

bool Ext4Driver::mount(fs::BlockDevice* device, bool readOnly) {
    if (!volume_.mount(device)) {
        return false;
    }
    mounted_ = true;
    readOnly_ = readOnly;
    return true;
}

bool Ext4Driver::remount(bool writable) {
    // libext4 1차 증분은 쓰기 경로 자체가 없어(§5) writable=true로
    // 전환해도 실질적 의미는 없다 - 계약대로("이미 writable이면 아무
    // 효과 없이 true") 요청은 받아들이지만, 실제 쓰기 오퍼레이션은
    // 여전히 onExec에서 PermissionDenied로 거부된다.
    readOnly_ = !writable;
    return true;
}

// [발견, 2026-09-22, PN-9AE5BFE4 실측] `Ext4Volume`의 모든 I/O
// 메서드(resolvePath/readInode/statInode/readdirAt)는 결국
// `fs::BlockDevice::readBlocks()`(동기 래퍼, `AsyncTaskWaitGroup::
// waitAll()` 사용)를 호출한다 - 이 대기 방식은 **Task 레벨 블로킹**
// 이라 `kFsKernelMain()` 같은 진짜 `kernel::Task` 컨텍스트에서
// 호출하면 정상 동작하지만(PN-22784AD4/PN-6D9A5DAE 검증이 바로 이
// 경로), `AsyncTaskHandler::onExec()`은 `AsyncReactor::drainOnce()`
// 안에서 실행되는 **코루틴**이다. 실측으로 확인된 실제 무한 대기
// 경로: `AsyncTaskWaitGroup::waitAll()`이 내부에서 다시
// `AsyncReactor::drainOnce()`를 호출하는데, `drainOnce()`는 "이미 이
// 코어에서 드레인이 진행 중이면 즉시 false 반환"하는 재진입 방지
// 가드를 갖고 있다(mount_table.h가 아니라 async_task.h 문서 주석
// 참고) - onExec 자신이 이미 drainOnce() 호출 스택 안에 있으므로
// 안쪽 waitAll()의 drainOnce() 호출은 항상 즉시 false를 반환하고,
// 그 결과 waitAll()이 `Scheduler::yieldCurrent()`로 넘어가지만 이
// 역시 코루틴 컨텍스트에선 중첩 AsyncTask(디바이스 I/O 완료용)를
// 진행시켜 주지 못해 영원히 끝나지 않는다(TEMP 하네스로 실제 QEMU
// 무한 대기 재현 후 원복). `AsyncTaskAwaiter`(코루틴 전용,
// async_task.h)가 이런 상황을 위한 것이지만, `Ext4Volume`을 그
// 경로를 쓰도록 다시 쓰려면 모든 I/O 지점을 코루틴화해야 하고,
// 그러면 반대로 `mount()`(진짜 Task 컨텍스트에서 한 번 동기 호출되는
// 준비 단계, SP-2BCE5D60 §3.1)에서는 못 쓰게 된다 - 두 컨텍스트
// 모두를 만족하는 설계는 이 세션이 임의로 정하지 않는다(CLAUDE.md
// 규칙4, `QU-FF7044DA`로 확인 요청 등록).
//
// 그래서 이번 증분은 `mount()`/`remount()`만 구현하고(Task 컨텍스트
// 호출이라 안전, 실측 검증 완료), I/O가 필요한 5개 op(Open/Read/
// Stat/Readdir 그리고 원래도 미구현이던 Write)는 `Ext4Volume`을
// 아예 호출하지 않고 명시적으로 실패를 반환한다 - "구현했지만 버그"
// 가 아니라 "이 설계 공백이 풀리기 전까진 안전하게 아무것도 안
// 한다"는 정직한 상태.
kernel::AsyncExecCoro Ext4Driver::onExec(kernel::AsyncTask*, void* argsRaw) {
    const auto op = *static_cast<const kernel::KernelFsOpCode*>(argsRaw);
    switch (op) {
        case kernel::KernelFsOpCode::Open: {
            auto* args = static_cast<kernel::KernelFsOpenArgs*>(argsRaw);
            args->result = kernel::OpenResult{kernel::FileHandle{}, false, kernel::VfsError::InvalidHandle};
            break;
        }
        case kernel::KernelFsOpCode::Close: {
            break;
        }
        case kernel::KernelFsOpCode::Read: {
            auto* args = static_cast<kernel::KernelFsReadArgs*>(argsRaw);
            args->result = kernel::ReadResult{0, kernel::VfsError::InvalidHandle};
            break;
        }
        case kernel::KernelFsOpCode::Write: {
            // libext4 1차 증분 자체도 쓰기 경로가 없다(SP-7A9CED3E §5) -
            // 위 코루틴 문제와 무관하게 어차피 거부돼야 하는 op.
            auto* args = static_cast<kernel::KernelFsWriteArgs*>(argsRaw);
            args->bytesWritten = 0;
            args->error = kernel::VfsError::PermissionDenied;
            break;
        }
        case kernel::KernelFsOpCode::Stat: {
            static_cast<kernel::KernelFsStatArgs*>(argsRaw)->error = kernel::VfsError::InvalidHandle;
            break;
        }
        case kernel::KernelFsOpCode::Mkdir: {
            static_cast<kernel::KernelFsMkdirArgs*>(argsRaw)->error = kernel::VfsError::PermissionDenied;
            break;
        }
        case kernel::KernelFsOpCode::Rmdir: {
            static_cast<kernel::KernelFsRmdirArgs*>(argsRaw)->error = kernel::VfsError::PermissionDenied;
            break;
        }
        case kernel::KernelFsOpCode::Unlink: {
            static_cast<kernel::KernelFsUnlinkArgs*>(argsRaw)->error = kernel::VfsError::PermissionDenied;
            break;
        }
        case kernel::KernelFsOpCode::Readdir: {
            auto* args = static_cast<kernel::KernelFsReaddirArgs*>(argsRaw);
            args->hasMore = false;
            args->error = kernel::VfsError::InvalidHandle;
            break;
        }
    }
    co_return;
}

}  // namespace ext4
