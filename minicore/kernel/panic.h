#ifndef MINICORE_KERNEL_PANIC_H
#define MINICORE_KERNEL_PANIC_H

namespace kernel {

// 예외 프레임과 무관한 일반 목적 커널 패닉(SP-EAB162FC §6.4, PN-645CF608
// - "KernelService resurrect limit exceeded" 등) - idt.cpp의 파일 범위
// kPanic(InterruptFrame*)와 달리 트랩 컨텍스트가 없는 평범한 함수 호출
// 지점(예: SelfTerminateHandler::onExec)에서도 부를 수 있도록 메시지
// 문자열 하나만 받는다. 메시지를 로그로 남긴 뒤 영원히 멈추다 - 절대
// 반환하지 않는다.
[[noreturn]] void kPanic(const char* message);

// [신규, 2026-09-17, PN-3081704A] 두 kPanic() 진입점(이 파일의
// kPanic(const char*), idt.cpp 파일 범위의 kPanic(InterruptFrame*))
// 이 공유하는 "최초 1회만" 래치 - 서로 다른 코어가 거의 동시에 각자
// 진짜 패닉(예: 서로 다른 essential 서비스가 비슷한 시점에 죽음)을
// 발견하면, 가장 먼저 이 함수를 부른 코어만 true를 받아 stop-the-world
// NMI 발신 + 진단 로그 출력을 실제로 진행한다. 나머지 코어는 false를
// 받는데, 이 시점까지도 자신의 인터럽트가 켜져 있으면(두 kPanic() 모두
// 맨 마지막 무한루프 직전에야 cli를 건다) 먼저 패닉한 코어가 보낸
// stop-the-world NMI에 자신이 한창 진행 중이던 Serial 출력/NMI 발신
// 도중 끼어들려 로그가 뒤섞이거나(실측 확인, PN-907C5289) 더 나쁘게는
// Smp::startApCores()의 AP 순차 기동 대기 루프 자체가 영구 이탈해
// 버리는 원인이 됐다 - false를 받은 즉시 아무것도 더 하지 않고 바로
// cli부터 걸어(자기 자신을 NMI가 아니어도 최소한 다른 조기 개입으로부터
// 보호) 조용히 영원히 멈추면 된다(어차피 먼저 패닉한 코어가 보낸
// stop-the-world NMI가 곧 도착해 자신을 정지시킨다).
bool kTryClaimFirstPanic();

}  // namespace kernel

#endif  // MINICORE_KERNEL_PANIC_H
