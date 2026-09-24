#ifndef MINICORE_LIBEXFAT_EXFAT_DRIVER_H
#define MINICORE_LIBEXFAT_EXFAT_DRIVER_H

#include "exfat.h"
#include "filesystem_driver.h"

// SP-F1987EF8 §4(갱신) - libexfat의 VFS 통합 계층(PN-09970F05). `ExfatVolume`
// (exfat.h, PN-C93A4E9E가 이미 구현/검증한 §3 온디스크 포맷 + mount())
// 위에 `kernel::FileSystemDriver`(mount/remount + KernelFsDriver의
// onExec/onFailure/onCancel)를 얹는 어댑터 - `Ext4Driver`/`Fat32Driver`
// (PN-9AE5BFE4/PN-EBAEA67B)와 완전히 같은 관례. `mount()`/`remount()`만
// `ExfatVolume`을 직접 호출하고(진짜 kernel::Task 컨텍스트의 준비
// 단계), `onExec`은 I/O가 필요한 Open/Read/Stat/Readdir을 `onExec`
// 자신의 코루틴 몸체 안에 `co_await kernel::AsyncTaskCoroAwaiter(...)`
// 를 직접 박아 넣는 평탄화(flatten) 버전으로 구현한다(코루틴 합성 불가
// 제약, ext4_driver.cpp 상단 문서 주석과 동일한 이유 - 자세한 구현은
// exfat_driver.cpp 참고). `Write`/`Mkdir`/`Rmdir`/`Unlink`만 libexfat
// 1차 증분 자체의 쓰기 경로 미구현으로 여전히 명시적 실패
// (PermissionDenied)를 반환한다.
//
// [갱신, 2026-09-25, PN-06690A0E] FileHandle 인코딩 - exFAT도 FAT류
// 처럼 재조회 가능한 단일 정수 식별자(inode 번호 같은)가 없다.
// **원래는** `Fat32Driver`가 쓴 하위32비트=firstCluster/상위32비트=
// fileSize 인코딩(vfat_driver.h 문서 주석)을 그대로 따르되 exFAT
// 전용 `noFatChain` 플래그(§3.2)까지 얹으려다 fileSize를 31비트로
// 잘라 넣었었다 - exFAT의 실제 온디스크 dataLength 필드가 64비트라
// firstCluster(32)+noFatChain(1)+fileSize(64) = 97비트가
// `FileHandle::value`(단일 uint64_t) 안에 원천적으로 안 들어갔기
// 때문(FAT32는 fileSize 필드 자체가 32비트라 이 문제가 없었음).
// **그 결과 2GiB를 초과하는 파일은 Open() 이후 Read()가 잘린
// fileSize로 EOF를 오판해 뒷부분을 영영 못 읽는 실제 기능 결함이
// 있었다(PN-06690A0E, minicore-3c 세션이 소스로 확인).**
//
// **수정**: `Fat32Driver`가 이미 같은 문제(디렉터리 엔트리 위치까지
// 담아야 해서 더 심각했음, `SP-A658A124` §4.2 `QU-E4E83A9A` 답변)를
// 겪고 확정한 해법을 그대로 따른다 - `FileHandle::value`엔 값을
// 직접 인코딩하지 않고 `openHandles_` 배열의 인덱스만 담는다. 실제
// 상태(firstCluster/fileSize 64비트 그대로/noFatChain/isDir)는 전부
// 그 배열 원소(`OpenHandleEntry`)에 있다. `Open()`이 빈 슬롯에
// 채워 인덱스를 반환하고, `Close()`가 그 슬롯을 반납한다 - 이제
// fileSize를 자를 필요가 전혀 없다(uint64_t 그대로 저장).
namespace exfat {

// Open이 채우고 Read/Readdir/Close가 조회하는 슬롯 - exFAT 자신에겐
// 없는 "재조회 가능한 파일 식별자"를 이 커널 프로세스 생애주기
// 동안만 메모리에 들고 있는 역할(디스크에 반영 안 됨, 재부팅 시
// 당연히 사라짐 - 정상, vfat_driver.h의 OpenHandleEntry와 동일한
// 관례). 쓰기 경로가 없는 v1이라 Fat32Driver의 entryCluster/
// entryByteOffset(부모 디렉터리 엔트리 위치, write 시 그 엔트리를
// 다시 쓰기 위한 정보)는 여기선 불필요.
struct OpenHandleEntry {
    bool inUse = false;
    uint32_t firstCluster = 0;
    uint64_t fileSize = 0;  // 이제 64비트 그대로 - 더 이상 절단 없음
    bool noFatChain = false;
    bool isDir = false;
};

// v1 고정 크기(실측 후 조정 대상, RM-23F4B687 §4 취지, vfat_driver.h
// 와 동일한 관례) - 이 커널 프로세스 하나가 exFAT 볼륨 하나에 동시에
// 열어 둘 수 있는 최대 파일/디렉터리 핸들 수.
constexpr uint32_t kMaxOpenHandles = 64;

class ExfatDriver : public kernel::FileSystemDriver {
public:
    bool mount(fs::BlockDevice* device, bool readOnly) override;
    bool remount(bool writable) override;

    kernel::AsyncExecCoro onExec(kernel::AsyncTask* task, void* argsRaw) override;
    void onFailure(kernel::AsyncTask*) override {}
    void onCancel(kernel::AsyncTask*, void*) override {}

private:
    ExfatVolume volume_;
    bool mounted_ = false;
    bool readOnly_ = true;
    OpenHandleEntry openHandles_[kMaxOpenHandles]{};
};

}  // namespace exfat

#endif  // MINICORE_LIBEXFAT_EXFAT_DRIVER_H
