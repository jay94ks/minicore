# QEMU gdb stub 연결 스크립트(PN-1E7798AF) - scripts/debug-qemu.sh로
# 띄운 QEMU(-s -S, TCP 1234)에 붙는다: `gdb -x scripts/kernel.gdb`.
#
# freestanding ELF라 심볼/디버그 정보는 이미 minicore.elf 안에 있다
# (CMake가 기본으로 -g를 켜지 않으면 스택 트레이스가 부실할 수 있음 -
# 그런 경우 CMakeLists.txt에 add_compile_options(-g)를 임시로 추가).
file build/minicore.elf
target remote localhost:1234

# 지금까지(2026-09-16) 관찰된 크래시 패턴은 전부 panic.cpp의 kPanic
# 또는 idt.cpp의 파일 로컬 kPanic(InterruptFrame*)로 귀결되는
# Double Fault류였다(PN-57CF48DB) - 여기서 자동으로 멈춰 그 시점의
# 진짜 레지스터/스택 상태를 살펴본다. Triple Fault(PN-6049A353처럼
# kPanic까지 도달하지 못하는 경우)는 이 브레이크포인트로 못 잡는다 -
# 대신 의심되는 함수(예: kForcedMigrationIsr, kSyncCr3 등)에 직접
# `break <함수명>`을 걸고 `continue`/`stepi`로 좁혀 나간다.
break kPanic

echo \n[kernel.gdb] 연결 완료 - 'continue'로 실행을 재개하세요.\n
echo [kernel.gdb] 특정 함수에서 멈추려면: break <함수명>\n
echo [kernel.gdb] 낮은 재현율 버그(PN-584DB994 ~4-6%)는 재시작 스크립트와 결합해 반복하세요.\n
