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
// **[갱신, 2026-09-22, PN-9AE5BFE4 완료]** `mount()`/`remount()`만
// `Ext4Volume`을 직접 호출한다(진짜 `kernel::Task` 컨텍스트에서 한
// 번 동기 호출되는 준비 단계라 안전, 실측 검증 완료). `onExec`은
// I/O가 필요한 Open/Read/Stat/Readdir을 `Ext4Volume`의 (이미 제거된)
// 동기 헬퍼로 재사용하지 않고, `co_await kernel::AsyncTaskCoroAwaiter
// (...)`를 `onExec` 자신의 코루틴 몸체 안에 직접 박아 넣는 평탄화
// (flatten)된 버전으로 실제로 I/O까지 수행한다(QEMU 실측 검증
// 완료) - `Ext4Volume`의 동기 래퍼를 그대로 썼다가 실측으로 확인된
// 무한 대기(`QU-FF7044DA`)를 피하기 위함. 자세한 이유/메커니즘은
// ext4_driver.cpp 상단 문서 주석 참고.
//
// FileHandle 인코딩: ext4는 inode 번호 자체가 재조회 가능한 단일
// 정수 식별자라(FAT류와 달리) `FileHandle::value`에 inode 번호를
// 그대로 담는다 - 별도 open-handle 테이블이 필요 없다(무상태).
//
// **[갱신, 2026-09-25, PN-FE718C87]** `Mkdir`은 이제 실제로 구현돼
// 있다(이 커널 최초의 실제 디스크 쓰기 오퍼레이션 - 조립 순서/롤백
// 정책 등은 ext4_driver.cpp의 Mkdir 케이스 문서 주석과 PN-FE718C87
// 계획 본문 참고). `Write`/`Rmdir`/`Unlink`는 여전히 미구현으로
// `VfsError::PermissionDenied`를 반환한다 - 특히 `Write`는 5개
// 이상의 익스텐트가 필요한 파일의 실제 온디스크 트리 확장/분할
// (SP-7A9CED3E §5)이 여전히 미결이라 그 갭이 해소될 때까지 막아
// 둔다.
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
