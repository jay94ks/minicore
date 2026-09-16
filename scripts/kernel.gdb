# gdb 연결 스크립트(SP-DABFCF9F §3.2, PN-1E7798AF) - scripts/
# run-qemu-gdb.sh 또는 run-grub-gdb.sh로 띄운 QEMU(-s -S, TCP 1234)에
# 붙는다: 프로젝트 루트에서 `gdb -x scripts/kernel.gdb`.
#
# **gdb-multiarch 불필요(2026-09-16 실측 확인, §5-2)** - 이 WSL
# 환경엔 gdb-multiarch가 설치돼 있지 않지만, 호스트/타겟 아키텍처가
# 둘 다 x86_64로 같아 평범한 `gdb`(amd64용)가 이 타겟을 이미 기본
# 지원한다 - gdb-multiarch는 호스트/타겟 아키텍처가 다를 때만
# 필요하다.
target remote localhost:1234
symbol-file build/minicore.elf
set architecture i386:x86-64

# higher-half 커널이라 심볼 주소 자체가 이미 KERNEL_VMA 기준(linker.ld
# 참고) - 별도 오프셋 계산 불필요.

# 기본 브레이크포인트 - 부팅 진입점(전체 흐름을 처음부터 보고 싶을 때).
break kMain

# 크래시 지점(2026-09-16까지 관찰된 패턴은 전부 Double Fault류가
# 도달하는 kPanic이었다 - PN-57CF48DB) - 필요하면 주석을 풀거나
# 직접 `break kPanic`을 입력한다.
# break kPanic

# SMP: `-smp N`으로 띄웠으면 QEMU gdbstub이 vCPU마다 별도 gdb
# 스레드로 노출한다 - `info threads`로 코어별 스레드 목록을,
# `thread N`으로 그 코어의 레지스터/스택으로 전환해 확인한다.

echo \n[kernel.gdb] 연결 완료 - 'continue'로 실행을 재개하세요.\n
echo [kernel.gdb] 특정 함수에서 멈추려면: break <함수명> (조건부: break <함수명> if <조건>)\n
echo [kernel.gdb] SMP 코어 전환: info threads / thread N\n
echo [kernel.gdb] 낮은 재현율 버그(PN-584DB994 ~4-6%)는 §4의 2단계 절차(반복 재현 확인 후 gdb 전환) 참고.\n
