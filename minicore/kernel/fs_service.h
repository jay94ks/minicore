#ifndef MINICORE_KERNEL_FS_SERVICE_H
#define MINICORE_KERNEL_FS_SERVICE_H

namespace kernel {

// [신규, 2026-09-20, SP-43331889 §7] fs의 KernelThread entry -
// `kmain.cpp`가 `kSpawnKernelThread(kFsKernelMain, nullptr)`로 직접
// 띄운다(main.cpp 문서 주석 참고 - Process 없는 순수 커널 Task, ELF
// 로드/argc·argv 없음).
void kFsKernelMain(void* arg);

}  // namespace kernel

#endif  // MINICORE_KERNEL_FS_SERVICE_H
