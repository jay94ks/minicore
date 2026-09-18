// minicore/init: initrd가 하드코딩으로 최초 구동하는 유일한 유저
// 프로그램(SP-68182FBD §2.4) - PN-16CA347D 4번 증분. 아직 ring3 진입
// (PN-124C105B)도 실제 syscall 트랩(userland/libs/libmc 참고)도 없어
// "무엇을 할지"보다 "libmc를 통해서만 커널에 요청한다"는 모양 자체를
// 먼저 갖춰 둔다 - 실제로 할 일(fs 서비스 대기 등)은 그 인프라가
// 생기는 다음 증분에서 여기로 이어붙인다.
#include "libmc/syscall.h"

extern "C" void _start() {
    // TODO(PN-124C105B 이후): 실제로 무엇을 할지(fs/devmgr 준비 대기,
    // 첫 유저 프로세스 기동 등)는 ring3 진입과 프로세스 생성/exec이
    // 갖춰진 뒤 채운다.
    //
    // [수정, 2026-09-18, PN-11B3D2BB] `mc::selfTerminate(0)`을 바로
    // 부르던 것을 무한 대기로 교체 - `kSpawnInitProcess()`(kmain.cpp)가
    // `ProcessStartFlags::essential = true`로 스폰하는 이 프로젝트의
    // PID 1이라, essential 프로세스가 실행을 마치면(정상 종료든
    // 크래시든) 커널이 즉시 패닉하는 안전장치(scheduler.cpp "PANIC -
    // essential service died")에 걸린다 - 위 주석이 쓰였던 시점엔
    // `selfTerminate()`가 실제로는 반환하지 않는 스텁이라고 가정했지만
    // (그 가정 자체가 이젠 틀림 - syscall이 이미 완전히 구현돼 진짜로
    // 프로세스를 끝낸다), 실측(GRUB+initrd 부팅)으로 100% 재현되는
    // "PANIC - essential service died: init"을 직접 확인했다. 실제
    // 워크로드(fs 서비스 대기 등)가 이어붙기 전까지는 이 무한 대기가
    // "PID 1은 절대 종료하지 않는다"는 essential 계약을 만족시키는
    // 자리표시자다.
    for (;;) {
        asm volatile("pause");
    }
}
