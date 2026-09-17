// minicore/pubreg: "프로세스간 공개 인터페이스" Registry 서비스(5번째
// 커널 서비스, SP-B071E628 §1~§4, PN-185406F6 항목1) - devmgr/init과
// 같은 이유로 "libmc를 통해서만 커널에 요청한다"는 모양부터 갖춰 둔다
// (minicore/devmgr/main.cpp/minicore/init/main.cpp와 동일한 관례).
//
// **[정정, 착수 중 코드 감사] 이번 증분은 항목1(서비스 스켈레톤)까지만**
// - SP-B071E628 §3의 "openChannel(name="pubreg"류)로 등록 채널 개설"
// (PN-185406F6 항목3)은 실제로 착수해 보니 `userland/libs/libmc`에
// Channel IPC syscall 래퍼(ConnectChannel/AcceptFromChannel/
// ChannelRead/ChannelWrite 등)가 **아직 하나도 없다**(devmgr/init
// 둘 다 pnp/selfTerminate만 씀 - 코드 감사로 확인, libmc/pnp.h/
// syscall.h 두 헤더뿐). 이 프로젝트에서 Channel IPC는 지금까지 전부
// 커널 내부 TEMP 스캐폴딩(가짜 UserThread가 핸들러 onExec을 직접
// 호출)으로만 검증돼 왔지, 진짜 유저랜드 ELF가 실제 syscall 트랩으로
// Channel을 연 적이 한 번도 없다 - 그 래퍼 자체가 별도의 독립적인
// 작업량(libmc/channel.h 신설)이라 항목3/4으로 분리해 후속 세션에
// 남긴다(RM-23F4B687 §4 - 검증 없이 한 번에 다 만들지 않는다).
#include "libmc/syscall.h"

extern "C" void _start() {
    // TODO(PN-185406F6 항목3/4): libmc/channel.h(신규) 완성 후
    // openChannel(name="pubreg")로 등록 채널을 열고 register/query
    // 메시지를 libjson으로 처리하는 루프가 여기 이어붙는다. 지금은
    // 부팅 매니페스트 스캔(PN-D3C05C0B)이 이 프로세스를 정상적으로
    // 찾아 스폰하는지(ProcessRole::KernelService 포함)만 검증 대상.
    mc::selfTerminate(0);
}
