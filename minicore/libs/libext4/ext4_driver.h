#ifndef MINICORE_LIBEXT4_EXT4_DRIVER_H
#define MINICORE_LIBEXT4_EXT4_DRIVER_H

#include "ext4.h"
#include "filesystem_driver.h"

// SP-7A9CED3E §4(갱신) - libext4의 VFS 통합 계층(PN-9AE5BFE4).
// `Ext4Volume`(ext4.h, PN-22784AD4가 이미 구현/검증한 §3 온디스크
// 포맷 + 무상태 읽기 API) 위에 `kernel::FileSystemDriver`(mount/
// remount + KernelFsDriver의 onExec/onFailure/onCancel)를 얹는
// 어댑터 - `LiveFs`/`ProcFs`와 동일한 관례로 args의 `KernelFsOpCode`
// 태그를 분기한다.
//
// **[범위, 2026-09-22, PN-9AE5BFE4 실측 발견]** `mount()`/`remount()`
// 만 `Ext4Volume`을 실제로 호출한다(진짜 `kernel::Task` 컨텍스트에서
// 한 번 동기 호출되는 준비 단계라 안전, 실측 검증 완료) - `onExec`은
// I/O가 필요한 5개 op(Open/Read/Stat/Readdir/Write) 전부에서
// `Ext4Volume`을 호출하지 않고 명시적 실패만 반환한다. 이유는
// ext4_driver.cpp의 `onExec` 문서 주석 참고 - `Ext4Volume`의 I/O가
// Task 레벨 블로킹(`BlockDevice::readBlocks()`)에 의존하는데,
// `onExec`은 코루틴(`AsyncReactor::drainOnce()` 안에서 실행)이라
// 그 안에서 호출하면 실측으로 확인된 무한 대기가 발생한다
// (`QU-FF7044DA`로 설계자 확인 요청 등록, CLAUDE.md 규칙4 - 임의로
// 정하지 않음).
//
// FileHandle 인코딩(설계만 - 위 이유로 아직 실제로 안 씀): ext4는
// inode 번호 자체가 재조회 가능한 단일 정수 식별자라(FAT류와 달리)
// `FileHandle::value`에 inode 번호를 그대로 담을 수 있다 - 별도
// open-handle 테이블이 필요 없다(무상태).
//
// `Write`/`Mkdir`/`Rmdir`/`Unlink` op는 위 코루틴 문제와 별개로도
// `libext4` 1차 증분 자체가 아직 쓰기 경로를 구현하지 않아
// (SP-7A9CED3E §5의 익스텐트 트리 분할 알고리즘 등 미결 사항)
// `LiveFs`의 v1 축소 범위와 동일하게 `VfsError::PermissionDenied`로
// 명시적으로 거부한다.
namespace ext4 {

class Ext4Driver : public kernel::FileSystemDriver {
public:
    bool mount(fs::BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;

    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override;
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}

private:
    Ext4Volume volume_;
    bool mounted_ = false;
    bool readOnly_ = true;
};

}  // namespace ext4

#endif  // MINICORE_LIBEXT4_EXT4_DRIVER_H
