#ifndef MINICORE_KERNEL_FS_SERVICE_H
#define MINICORE_KERNEL_FS_SERVICE_H

namespace kernel {

// [신규, 2026-09-20, SP-43331889 §7] fs의 KernelThread entry -
// `kmain.cpp`가 `kSpawnKernelThread(kFsKernelMain, nullptr)`로 직접
// 띄운다(main.cpp 문서 주석 참고 - Process 없는 순수 커널 Task, ELF
// 로드/argc·argv 없음).
void kFsKernelMain(void* arg);

// [신규, 2026-09-29, QU-1E7DFB8C 답변(A) - SP-7CC5693A §6 5단계 보강]
// 지금까지 `kTryAutoMountBlockDevice()`(fs.cpp)는 ext4/FAT32/FAT16을
// 항상 readOnly=true로만 마운트해 실제 온디스크 쓰기 성공 경로를 검증할
// 방법이 없었다 - `kmain.cpp`가 부팅 cmdline(`kCmdlineHasFlag`)에서
// "--rw-mount" 플래그를 확인해 이 setter로 fs 서비스에 알린다(Lapic::
// setX2ApicDisabled와 동일한 "부팅 극초반 cmdline 파싱 -> 전역 플래그"
// 관례). 이 플래그가 없으면(기본값) 기존과 동일하게 항상 readOnly=true -
// 이 기본 동작을 검증하는 diskmetatest/fatdisktest 회귀 테스트는
// 영향받지 않는다.
void kSetWritableAutoMountRequested(bool requested);

}  // namespace kernel

#endif  // MINICORE_KERNEL_FS_SERVICE_H
