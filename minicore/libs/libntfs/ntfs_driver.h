#ifndef MINICORE_LIBNTFS_NTFS_DRIVER_H
#define MINICORE_LIBNTFS_NTFS_DRIVER_H

#include "ntfs.h"
#include "filesystem_driver.h"

// SP-AA6DF406 §4(갱신) - libntfs의 VFS 통합 계층(PN-52C577F3). `NtfsVolume`
// (ntfs.h, PN-49F24FD7이 이미 구현/검증한 §3 온디스크 포맷 + mount())
// 위에 `kernel::FileSystemDriver`(mount/remount + KernelFsDriver의
// onExec/onFailure/onCancel)를 얹는 어댑터 - `Ext4Driver`/`Fat32Driver`/
// `ExfatDriver`와 완전히 같은 관례. `mount()`/`remount()`만
// `NtfsVolume`을 직접 호출하고(진짜 kernel::Task 컨텍스트의 준비
// 단계), `onExec`은 I/O가 필요한 Open/Read/Stat/Readdir을 `onExec`
// 자신의 코루틴 몸체 안에 `co_await kernel::AsyncTaskCoroAwaiter(...)`
// 를 직접 박아 넣는 평탄화(flatten) 버전으로 구현한다(코루틴 합성
// 불가 제약, ext4_driver.cpp 상단 문서 주석과 동일한 이유).
//
// `libntfs` 1차 증분은 기본적으로 읽기 전용 - Write/Mkdir/Rmdir/Unlink는
// `readOnly_`와 무관하게 항상 거부한다(SP-AA6DF406 §1이 든 위험 - MFT
// 비트맵 할당/속성 상주-비상주 확장/B+ 트리 재조정은 여전히 범위 밖).
// **[예외, 2026-09-29, QU-9F8AD7A8 답변(A)]** Chmod만은 그 위험 중
// 어디에도 해당하지 않는 "이미 존재하는 상주 속성($STANDARD_INFORMATION,
// 크기 불변)의 제자리 갱신"이라 `readOnly_` 게이트를 통과하면 실제로
// 디스크에 쓴다(exFAT의 Chmod와 대칭, PN-2A0981B7 항목2) - `mount()`가
// 이제 `readOnly` 인자를 실제로 반영하고 `remount()`도 전환을 허용한다.
//
// FileHandle 인코딩: NTFS는 MFT 레코드 번호 자체가 재조회 가능한
// 단일 정수 식별자다(ext4의 inode 번호와 동일한 성격 - 파일 크기는
// `$DATA` 속성을 다시 찾아 얻으면 되므로 FAT/exFAT처럼 firstCluster+
// fileSize를 핸들에 합성할 필요가 없다) - `FileHandle::value`에 MFT
// 레코드 번호를 그대로 담는다.
//
// 대용량 디렉터리(`$INDEX_ALLOCATION` 필요, 하위 노드가 있는 인덱스
// 엔트리)를 만나면 `Open`/`Readdir` 모두 명시적 실패(NotFound/
// InvalidHandle)로 거부한다 - 크래시가 아니라 "이번 증분이 다루지
// 않는 형태"로 정직하게 실패한다(SP-AA6DF406 §1이 이미 범위 밖으로
// 명시).
namespace ntfs {

class NtfsDriver : public kernel::FileSystemDriver {
public:
    bool mount(fs::BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;

    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override;
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}

private:
    NtfsVolume volume_;
    bool mounted_ = false;
    bool readOnly_ = true;
};

}  // namespace ntfs

#endif  // MINICORE_LIBNTFS_NTFS_DRIVER_H
