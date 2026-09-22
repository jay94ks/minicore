#ifndef MINICORE_LIBVFAT_VFAT_DRIVER_H
#define MINICORE_LIBVFAT_VFAT_DRIVER_H

#include "filesystem_driver.h"
#include "vfat.h"

// SP-A658A124 §4(갱신) - libvfat의 VFS 통합 계층(PN-EBAEA67B). `Fat32Volume`
// (vfat.h, PN-F32F55A8이 이미 구현/검증한 §3 온디스크 포맷 + mount())
// 위에 `kernel::FileSystemDriver`(mount/remount + KernelFsDriver의
// onExec/onFailure/onCancel)를 얹는 어댑터 - `Ext4Driver`
// (minicore/libs/libext4/ext4_driver.h/.cpp, PN-9AE5BFE4)와 완전히
// 같은 관례.
//
// **[범위, 2026-09-22, PN-9AE5BFE4가 먼저 실측 확인한 제약을 그대로
// 적용]** `mount()`/`remount()`만 `Fat32Volume`을 직접 호출한다(진짜
// `kernel::Task` 컨텍스트에서 한 번 동기 호출되는 준비 단계라 안전).
// `onExec`은 I/O가 필요한 Open/Read/Stat/Readdir을 `Fat32Volume`의
// (이미 제거된) 동기 헬퍼로 재사용하지 않고, `onExec` 자신의 코루틴
// 몸체 안에 `co_await kernel::AsyncTaskCoroAwaiter(...)`를 직접 박아
// 넣는 평탄화(flatten) 버전으로 새로 구현한다 - 이유는
// `ext4_driver.cpp` 상단 문서 주석과 동일(코루틴 합성 불가 제약,
// `SP-F682B889` §9.5 항목3 2026-09-22 정정 문단 참고).
//
// FileHandle 인코딩: FAT엔 ext4의 inode 번호 같은 재조회 가능한 단일
// 정수 식별자가 없다(firstCluster만으로는 fileSize/isDir을 못 얻음 -
// vfat.h의 `ResolvedEntry` 참고). 이 드라이버는 별도 open-handle
// 테이블 없이 `FileHandle::value`(uint64_t) 안에 두 값을 직접
// 인코딩한다 - 하위 32비트: firstCluster, 상위 32비트: fileSize
// (FAT32 `DirEntry::fileSize` 필드 자체가 32비트라 정확히 들어맞음).
// isDir은 인코딩하지 않는다 - `Open`이 `OpenResult::isDirectory`로
// 바로 돌려주고, `Read`는 파일 핸들에만, `Readdir`은 디렉터리
// 핸들에만 쓰여 호출부(VFS 계층)가 이미 구분해서 부르는 값이므로
// 핸들 자신이 다시 담을 필요가 없다(디렉터리 핸들의 상위 32비트는
// 항상 0 - `ResolvedEntry::fileSize`가 디렉터리는 항상 0인 스펙
// 그대로).
namespace vfat {

class Fat32Driver : public kernel::FileSystemDriver {
public:
    bool mount(fs::BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;

    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override;
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}

private:
    Fat32Volume volume_;
    bool mounted_ = false;
    bool readOnly_ = true;
    // [신규, 2026-09-23, PN-9D6FE4B6 준비 작업, SP-A658A124 §3.4] free
    // 클러스터 선형 스캔의 시작 힌트 - 매번 클러스터 2부터 스캔하지
    // 않고 마지막으로 할당한 자리 다음부터 이어서 찾는다(libswapfs의
    // allocateSlot과 동일한 방식, §3.4 문서 주석 그대로). Write/Mkdir
    // 구현 자체는 QU-E4E83A9A(FileHandle이 부모 디렉터리 엔트리 위치를
    // 못 담는 구조적 문제) 답변 대기 중이라 이 필드는 아직 안 쓰인다.
    uint32_t nextClusterScanHint_ = 2;
};

}  // namespace vfat

#endif  // MINICORE_LIBVFAT_VFAT_DRIVER_H
