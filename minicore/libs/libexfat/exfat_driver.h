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
// FileHandle 인코딩 - exFAT도 FAT류처럼 재조회 가능한 단일 정수
// 식별자(inode 번호 같은)가 없다. `Fat32Driver`가 쓴 하위32비트=
// firstCluster/상위32비트=fileSize 인코딩(vfat_driver.h 문서 주석)을
// 그대로 따르되, exFAT은 추가로 클러스터 체인 순회 방식을 정하는
// `noFatChain` 플래그(§3.2)도 실어야 한다 - `FileHandle::value`(단일
// uint64_t)에 firstCluster(32비트)+noFatChain(1비트)+fileSize를 전부
// 담을 자리가 없어(32+1+64 > 64), **fileSize를 31비트로 줄여
// 최상위 비트(bit 63)를 noFatChain 플래그로 쓴다**(SP-F1987EF8
// §4/PN-09970F05가 착수 세션 재량으로 남겨 둔 정확한 비트 배치를
// 이렇게 결정) - `Fat32Driver`가 32비트 그대로 쓸 수 있었던 건
// FAT32의 fileSize 필드 자체가 32비트라 정확히 들어맞았기 때문이고,
// exFAT의 실제 온디스크 dataLength 필드는 64비트라 근본적으로
// 무손실 인코딩이 불가능하다(96비트 필요) - 그래서 이 v1 인코딩은
// 파일 크기를 **최대 (2^31)-1바이트(약 2GiB)까지만 정확히 표현**하는
// 알려진 제약을 감수한다(그 이상 크기의 파일은 Open()/Stat()에서 실제
// dataLength를 정확히 보고하지만, Read()는 핸들에 인코딩된 잘린 크기
// 기준으로 EOF를 판정해 사실상 접근 불가 - 후속 증분이 필요시 별도
// open-handle 테이블 도입으로 해소할 수 있는 v1 한계, 이번 증분
// 범위 밖). 디렉터리 핸들도 같은 인코딩을 쓴다(exFAT 디렉터리도
// 파일과 동일하게 Stream Extension에 실제 dataLength/NoFatChain을
// 가져 필요 - 단, 항상 EOC까지 순회하는 FAT 체인 디렉터리는 순회에
// fileSize를 쓰지 않아 이 절단의 영향이 없다). 루트 디렉터리(Stream
// Extension이 없음)는 firstCluster=rootFirstCluster()/fileSize=0/
// noFatChain=false로 인코딩한다(§3.2 - 루트는 항상 일반 FAT 체인).
namespace exfat {

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
};

}  // namespace exfat

#endif  // MINICORE_LIBEXFAT_EXFAT_DRIVER_H
