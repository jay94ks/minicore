// minicore/devtest - SP-23880DC6(DeviceRegistry, udev 유사 시스템) E2E
// 검증 전용 클라이언트. socknamedtest 등과 동일한 out-of-tree
// add_subdirectory 관례 - 부팅 매니페스트에는 포함하지 않는다(TEMP
// kmain.cpp 훅으로만 자동 실행).
//
// 시나리오:
//  1. "/sys/dev"를 Open(디렉터리)+Readdir로 전부 나열한다(§3.2) - 이
//     단계 자체가 실패하면 안 됨(장치가 0개여도 Readdir은 정상 종료).
//  2. isDirectory==false인 첫 항목을 "루트 소유 평평한 장치"로 채택,
//     Stat()으로 type==Regular/uid==root를 확인한다(§3.6/§3.7).
//  3. 그 장치를 Open() - §4 결정2에 따라 결과는 None(마운트 안 됨,
//     raw 열기 성공) 또는 PermissionDenied(이미 마운트됨) 둘 중
//     하나여야 한다.
//     - None이면: 배타적 Open(§4 항목1)을 확인하기 위해 같은 경로를
//       한 번 더 Open해 반드시 AlreadyExists가 나오는지 검사한 뒤,
//       첫 fd를 Close한다.
//  4. DeviceEventsOpen(§3.3, 그룹2 call4) syscall이 fd를 정상적으로
//     내주는지만 확인하고(핫플러그는 v1에 실제 트리거가 없어 이벤트
//     도착 자체는 검증 범위 밖) 바로 Close한다.
//  5. "/sys/dev" 디렉터리 fd를 Close.
//
// 장치가 하나도 없어도(AHCI 미부착 QEMU 실행) 1/4/5단계만으로 정상
// 종료한다 - 이 클라이언트가 검증하는 핵심은 "/sys/dev" 배선과
// DeviceEventsOpen 등록 자체이지, 특정 하드웨어 구성 의존이 아니다.
//
// exitCode: 0=전체 성공(또는 장치 없음+나머지 정상), 1=/sys/dev Open
// 실패, 2=Readdir 실패, 3=발견한 장치의 Stat 실패/타입 불일치,
// 4=장치 Open 결과가 None/PermissionDenied 둘 다 아님, 5=배타적 Open
// 위반(중복 Open이 성공함), 6=DeviceEventsOpen 실패.
#include "libmc/pnp.h"
#include "libmc/syscall.h"
#include "libmc/vfs.h"

namespace {

constexpr char kDevDirPath[] = "/sys/dev";

[[noreturn]] void kFinish(mc::int32_t code) { mc::selfTerminate(code); }

}  // namespace

extern "C" void _start() {
    mc::OpenArgs dirOpenArgs;
    dirOpenArgs.path = kDevDirPath;
    dirOpenArgs.pathLen = sizeof(kDevDirPath) - 1;
    dirOpenArgs.flags = static_cast<mc::uint32_t>(mc::OpenFlags::Directory);
    mc::SyscallToken token = mc::submit(mc::kSyscallEndpointOpen, &dirOpenArgs);
    if (token == 0 || !mc::wait(token) || dirOpenArgs.error != mc::ChannelError::None) {
        kFinish(1);
    }

    bool deviceFound = false;
    char deviceName[64] = {};
    mc::uint32_t deviceNameLength = 0;

    for (;;) {
        mc::ReaddirArgs readArgs;
        readArgs.fd = dirOpenArgs.fd;
        token = mc::submit(mc::kSyscallEndpointReaddir, &readArgs);
        if (token == 0 || !mc::wait(token) || readArgs.error != mc::ChannelError::None) {
            kFinish(2);
        }
        if (!readArgs.hasMore) {
            break;
        }
        if (!deviceFound && !readArgs.isDirectory) {
            deviceFound = true;
            for (mc::uint32_t i = 0; i < readArgs.nameLength; ++i) {
                deviceName[i] = readArgs.name[i];
            }
            deviceNameLength = readArgs.nameLength;
        }
    }

    mc::CloseArgs dirCloseArgs;
    dirCloseArgs.fd = dirOpenArgs.fd;
    token = mc::submit(mc::kSyscallEndpointClose, &dirCloseArgs);
    if (token != 0) {
        mc::wait(token);
    }

    if (deviceFound) {
        char fullPath[128];
        mc::uint32_t pos = 0;
        for (mc::uint32_t i = 0; i < sizeof(kDevDirPath) - 1; ++i) {
            fullPath[pos++] = kDevDirPath[i];
        }
        fullPath[pos++] = '/';
        for (mc::uint32_t i = 0; i < deviceNameLength; ++i) {
            fullPath[pos++] = deviceName[i];
        }
        const mc::uint32_t fullPathLen = pos;

        mc::StatArgs statArgs;
        statArgs.path = fullPath;
        statArgs.pathLen = fullPathLen;
        token = mc::submit(mc::kSyscallEndpointStat, &statArgs);
        if (token == 0 || !mc::wait(token) || statArgs.error != mc::ChannelError::None ||
            statArgs.type != mc::FileType::Regular || statArgs.uid != 0) {
            kFinish(3);
        }

        mc::OpenArgs firstOpenArgs;
        firstOpenArgs.path = fullPath;
        firstOpenArgs.pathLen = fullPathLen;
        firstOpenArgs.flags = 0;
        token = mc::submit(mc::kSyscallEndpointOpen, &firstOpenArgs);
        if (token == 0 || !mc::wait(token)) {
            kFinish(4);
        }
        if (firstOpenArgs.error != mc::ChannelError::None &&
            firstOpenArgs.error != mc::ChannelError::PermissionDenied) {
            kFinish(4);
        }

        if (firstOpenArgs.error == mc::ChannelError::None) {
            mc::OpenArgs secondOpenArgs;
            secondOpenArgs.path = fullPath;
            secondOpenArgs.pathLen = fullPathLen;
            secondOpenArgs.flags = 0;
            token = mc::submit(mc::kSyscallEndpointOpen, &secondOpenArgs);
            if (token == 0 || !mc::wait(token) || secondOpenArgs.error != mc::ChannelError::AlreadyExists) {
                kFinish(5);
            }

            mc::CloseArgs firstCloseArgs;
            firstCloseArgs.fd = firstOpenArgs.fd;
            token = mc::submit(mc::kSyscallEndpointClose, &firstCloseArgs);
            if (token != 0) {
                mc::wait(token);
            }
        }
    }

    mc::DeviceEventsOpenArgs eventsArgs;
    token = mc::submit(mc::kSyscallEndpointDeviceEventsOpen, &eventsArgs);
    if (token == 0 || !mc::wait(token) || eventsArgs.error != mc::ChannelError::None) {
        kFinish(6);
    }
    mc::CloseArgs eventsCloseArgs;
    eventsCloseArgs.fd = static_cast<mc::int32_t>(eventsArgs.fd);
    token = mc::submit(mc::kSyscallEndpointClose, &eventsCloseArgs);
    if (token != 0) {
        mc::wait(token);
    }

    kFinish(0);
}
