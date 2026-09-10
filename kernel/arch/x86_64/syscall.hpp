// SYSCALL/SYSRET 진입 설정 (docs/plan/kernel-bootstrap.md M8). 유저모드
// 진입을 시작하기 전에 한 번 호출해야 한다 — syscall_entry.S/syscall.cpp
// 참고.
#pragma once

namespace kern::arch::x86_64 {

void install_syscall_entry();

}  // namespace kern::arch::x86_64
