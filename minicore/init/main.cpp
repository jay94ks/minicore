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
    // 갖춰진 뒤 채운다. 지금은 libmc를 거쳐 커널에 종료를 요청하는
    // 모양만 먼저 잡아 둔다 - selfTerminate()가 아직 스텁이라 실제로는
    // 반환하지 않는다.
    mc::selfTerminate(0);
}
