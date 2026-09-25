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
// FileHandle 인코딩: [갱신, 2026-09-23, PN-9D6FE4B6, QU-E4E83A9A 답변
// - "(A) 별도 open-handle 테이블 도입"] FAT엔 ext4의 inode 번호 같은
// 재조회 가능한 단일 정수 식별자가 없다(firstCluster만으로는
// fileSize/isDir/부모 디렉터리 엔트리 위치를 못 얻음 - 특히 write가
// 파일 끝을 넘거나 unlink가 디렉터리 엔트리 자체를 고쳐 써야 할 때
// "이 엔트리가 어느 디렉터리의 어느 위치에 있는지"가 반드시 필요한데
// 64비트 FileHandle::value 하나로는 담을 여유가 없다). 원래(1차 증분,
// 읽기 전용) 이 값에 firstCluster(하위32)+fileSize(상위32)를 직접
// 인코딩하던 방식을 폐기하고, `openHandles_` 배열의 인덱스 하나를
// 그대로 `FileHandle::value`로 쓴다 - 실제 상태(firstCluster/
// fileSize/isDir/부모 디렉터리 엔트리 위치)는 전부 그 배열 원소
// (`OpenHandleEntry`)에 있다. `Open()`이 빈 슬롯에 채워 인덱스를
// 반환하고, `Close()`가 그 슬롯을 반납한다.
namespace vfat {

// [신규, 2026-09-23, PN-9D6FE4B6] Open()이 채우고 Read/Write/Readdir/
// Close가 조회하는 슬롯 - FAT 자신에겐 없는 "재조회 가능한 파일
// 식별자"를 이 커널 프로세스 생애주기 동안만 메모리에 들고 있는
// 역할(디스크에 반영 안 됨, 재부팅 시 당연히 사라짐 - 정상).
struct OpenHandleEntry {
    bool inUse = false;
    uint32_t firstCluster = 0;  // 0 = 아직 클러스터가 배정 안 된 빈 파일(FAT32 스펙 관례)
    uint64_t fileSize = 0;      // 디렉터리는 항상 0
    bool isDir = false;
    // entryValid=false는 루트 디렉터리(부모 디렉터리 엔트리 자체가
    // 없음) - Write/Unlink류가 이 핸들을 대상으로 하면 그 자리에서
    // 거부한다.
    bool entryValid = false;
    uint32_t entryCluster = 0;
    uint32_t entryByteOffset = 0;
};

// v1 고정 크기(실측 후 조정 대상, RM-23F4B687 §4 취지) - 이 커널
// 프로세스 하나가 FAT32 볼륨 하나에 동시에 열어 둘 수 있는 최대
// 파일/디렉터리 핸들 수.
constexpr uint32_t kMaxOpenHandles = 64;

class Fat32Driver : public kernel::FileSystemDriver {
public:
    bool mount(fs::BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;
    // [신규, 2026-09-23, PN-547EF839, SP-0C7A4F3B의 onUnmount() 훅
    // 첫 실사용처] Shutdown/Reboot(또는 전원 버튼 이벤트) 직전에
    // `MountTable::unmountAllForShutdown()`이 호출 - mount()/remount()
    // 가 dirty로 표시했던 것의 반대로, clean-shutdown 비트를 다시
    // 세운다(§3.4 문서 주석과 동일한 근거로 동기 I/O 안전).
    void onUnmount() override;

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
    // allocateSlot과 동일한 방식, §3.4 문서 주석 그대로).
    uint32_t nextClusterScanHint_ = 2;
    // [신규, 2026-09-25, PN-9C836E41] FSInfo의 freeCount 캐시 - mount()
    // 가 온디스크 FSInfo 섹터에서 읽어 초기화하고(유효하지 않거나
    // "모름"이면 kFsInfoUnknown 그대로 유지), 이후 클러스터를 할당/
    // 해제할 때마다 갱신해 디스크에도 다시 써 준다(정확도가 필수는
    // 아닌 순수 캐시/힌트 필드 - PN-9D6FE4B6가 이미 겪은 대로, 이걸
    // 안 갱신하면 fsck.vfat가 매번 "값 불일치"로 auto-correct하게 됨).
    uint32_t freeClusterCount_ = vfat::kFsInfoUnknown;
    OpenHandleEntry openHandles_[kMaxOpenHandles]{};
};

// [신규, 2026-09-25, PN-5481287C 준비 6단계] Fat16Driver - FAT12/FAT16
// 공용 읽기 전용(v1) 어댑터. `Fat32Driver`와 완전히 같은 관례(mount()만
// `Fat16Volume`을 직접 호출하는 준비 단계, onExec은 평탄화된 코루틴)를
// 따르되 두 가지가 다르다:
// 1) 루트 디렉터리가 클러스터 체인이 아니라 `Fat16Volume`의 고정
//    섹터 범위(rootDirStartSectorValue()/rootDirSectorCountValue())라
//    Open/Stat/Readdir 전부 "지금 스캔 중인 게 루트냐 서브디렉터리냐"
//    분기가 필요하다 - 서브디렉터리는 여전히 평범한 클러스터 체인
//    (FAT32와 동일한 kClusterToSector 재사용).
// 2) FAT 엔트리 폭이 `volume_.entryWidth()`로 런타임에 갈리고(FAT12=
//    12비트 팩, FAT16=16비트 고정), FAT12는 섹터 경계를 걸칠 수 있어
//    엔트리 하나를 볼 때마다 섹터를 2개씩 읽는다(vfat_driver.cpp의
//    kFat16EntryLocation/kReadFat16EntryFromWindow 문서 주석 참고).
// 이번 증분은 읽기 전용(Open/Close/Read/Stat/Readdir)만 다룬다 -
// Write/Mkdir/Rmdir/Unlink는 Fat32Driver의 1차 증분(PN-F32F55A8)이
// 그랬듯 후속 증분(PN-9D6FE4B6과 대응하는 후속 계획)으로 남긴다.
class Fat16Driver : public kernel::FileSystemDriver {
public:
    bool mount(fs::BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;
    // v1은 dirty-shutdown 비트 관리를 하지 않는다(Fat32Driver도 처음엔
    // 그랬다가 PN-547EF839로 후속 추가됐다 - 같은 순서로 이번 증분
    // 범위 밖에 둔다).
    void onUnmount() override {}

    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override;
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}

private:
    Fat16Volume volume_;
    bool mounted_ = false;
    bool readOnly_ = true;
    OpenHandleEntry openHandles_[kMaxOpenHandles]{};
};

}  // namespace vfat

#endif  // MINICORE_LIBVFAT_VFAT_DRIVER_H
