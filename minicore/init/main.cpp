// minicore/init: initrd가 하드코딩으로 최초 구동하는 유일한 유저
// 프로그램(SP-68182FBD §2.4) - PN-16CA347D 4번 증분, 실구현은
// PN-A60AC09F(설계자 지시: "init은 죽으면 안 되고 자식들을 계속
// 감시해야 한다").
//
// [구현, 2026-09-21, PN-A60AC09F 항목1/3] "커널 서비스 대기" 단계는
// 별도 폴링이 필요 없다 - `kmain.cpp`가 `kSpawnInitProcess()`/
// `kSpawnServiceProcesses()`(pubreg/authmgr)/`kSpawnDevmgrKernelThread()`/
// `kSpawnFsKernelThread()`를 전부 `Scheduler::enterIdleLoop()`(스케줄러
// 디스패치 시작) **이전에 순서대로 동기 호출**한다(kmain.cpp 참고) -
// 즉 init의 유저 코드가 단 한 명령이라도 실행되기 전에 이 서비스들은
// 이미 전부 스폰이 끝나 있음이 부팅 순서 자체로 보장된다. devmgr/fs가
// 이제 pubreg에 등록하는 KernelThread가 아니라서(더 이상 유저
// syscall을 거치지 않음) 그 이상의 "완전히 초기화됐는지"를 확인할
// 공개된 신호가 없다 - 필요해지면 그 신호 자체를 새로 설계해야
// 하므로(CLAUDE.md 규칙4, 지금 임의로 만들지 않음) 여기서는 다루지
// 않는다.
//
// "유저랜드 초기화 단계"(설정 파일 기반 서비스 스폰)는 fs가 아직
// 0% 구현이라 여전히 착수 조건 미충족 - 이 증분의 범위 밖.
//
// 이 증분이 실제로 채우는 것은 "영구 감시 루프"(항목3) - 커널이
// `Process::setOrphanRoot()`로 이미 init을 고아 입양 루트로 등록해
// 두고, 자식이 죽으면 자동으로 이 프로세스의 `children`으로
// reparent한다(process.h `orphanRoot()` 문서 주석 참고) - init은
// `Wait` syscall(process.h `kSyscallEndpointWait`)을 반복 제출해
// 좀비가 된 자식(원래 자식이든 재입양된 고아든 구분 없음, `targetPid
// =-1`이 "아무 자식이나"라는 뜻)을 실제로 회수(reap)한다. `Wait`는
// 지금 블로킹하지 않고 그 순간의 상태만 한 번 보고하는 폴링형
// ABI라(`WaitArgs` 문서 주석 - "블로킹은 실제 소비자가 생기면 그때
// 확장" - 이 소비자가 바로 이 루프다), 좀비가 없으면 짧게 쉬었다가
// 다시 시도한다 - 커널에 아직 sleep/delay류 syscall이 없어 `pause`
// 스핀이 유일하게 쓸 수 있는 방법이다(예전 자리표시자와 동일한
// 기법, 이제 그 사이에 실제로 Wait도 부른다는 점만 다르다).
#include "libmc/process.h"
#include "libmc/syscall.h"

extern "C" void _start() {
    // [유지, 2026-09-18, PN-11B3D2BB] `kSpawnInitProcess()`(kmain.cpp)가
    // `ProcessStartFlags::essential = true`로 스폰하는 이 프로젝트의
    // PID 1이라, 이 함수가 반환하거나 `selfTerminate()`를 부르면
    // 커널이 즉시 패닉한다(scheduler.cpp "PANIC - essential service
    // died") - 아래 루프는 어떤 경우에도 빠져나가지 않는다.
    for (;;) {
        mc::WaitArgs args;
        args.targetPid = mc::kInvalidProcessId;  // 아무 자식이나(POSIX wait(-1, ...))
        mc::SyscallToken token = mc::submit(mc::kSyscallEndpointWait, &args);
        if (token != 0) {
            mc::wait(token);
            // hadZombieChild/hasAnyChild는 지금 당장 로그를 남길 곳이
            // 없어(콘솔/procfs 등 init 전용 진단 경로가 아직 없음)
            // 그냥 소비만 한다 - 이 루프의 목적은 회수(reap) 자체지
            // 보고가 아니다.
        }
        // 좀비가 있었든 없었든 곧바로 다시 스캔하면 sleep 부재 상태에서
        // 이 코어를 계속 붙잡는다 - 짧게 양보한다.
        asm volatile("pause");
    }
}
