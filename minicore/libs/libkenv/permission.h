#ifndef MINICORE_LIBS_LIBKENV_PERMISSION_H
#define MINICORE_LIBS_LIBKENV_PERMISSION_H

#include "types.h"

namespace kernel {

// [SP-30FCC8AE §1/§2, PN-617F4E52, PN-88E62419] Minicore 사용자/권한
// 체계 - Linux 유사 uid/gid + POSIX mode_t 하위 9비트(rwxrwxrwx) +
// 특수 비트 S 하나. uid/gid는 SpawnProcess/fork() 시 부모로부터
// 상속되며(process.cpp) 승격 경로(kSetuid)는 아직 없다(§1-A,
// PN-B6DB692C 후속 대상) - 지금은 모든 프로세스가 최초 프로세스(init)
// 로부터 이어받은 값을 그대로 쓴다. **최초 프로세스 자체의 기본값은
// root(0)** - 부팅 초기에는 다른 신원 축이 전혀 없어 root 외의
// 선택지가 없다(Linux의 PID 1이 root로 시작하는 것과 동일한 관례,
// RM-23F4B687 §4 취지상 이 선택 자체는 사소한 구현 세부).
using Uid = uint32_t;
using Gid = uint32_t;
constexpr Uid kRootUid = 0;
constexpr Gid kRootGid = 0;

// Process 전용이 아니라 앞으로 다른 자원(procfs 엔트리, ResourceGroup
// 등)에도 재사용 가능하도록 커널/유저 공용 위치(libkenv)에 둔다
// (RM-7C249618 - 새 라이브러리가 아니라 기존 libkenv에 파일만 추가).
using Permission = uint16_t;

constexpr Permission kPermOwnerRead  = 1u << 8;
constexpr Permission kPermOwnerWrite = 1u << 7;
constexpr Permission kPermOwnerExec  = 1u << 6;
constexpr Permission kPermGroupRead  = 1u << 5;
constexpr Permission kPermGroupWrite = 1u << 4;
constexpr Permission kPermGroupExec  = 1u << 3;
constexpr Permission kPermOtherRead  = 1u << 2;
constexpr Permission kPermOtherWrite = 1u << 1;
constexpr Permission kPermOtherExec  = 1u << 0;
constexpr Permission kPermSpecialS   = 1u << 9;  // 의미 미정 - SP-30FCC8AE §7(sudo/su, setuid류)

// [SP-30FCC8AE §3] 범용 권한 판정 - resourceUid/resourceGid/mode로
// 표현된 자원에 callerUid/callerGid가 requested 접근(카테고리 무관 -
// owner/group/other 중 어느 비트를 볼지는 uid/gid 비교로 스스로
// 고른다)을 할 수 있는지. 판정 순서(§3이 확정한 그대로, 앞 단계가
// 통과하면 뒤 단계는 안 본다):
//   1. callerUid == kRootUid - 항상 허용(root 특권).
//   2. callerUid == resourceUid면 mode의 owner 비트, callerGid ==
//      resourceGid면 group 비트, 둘 다 아니면 other 비트 - Linux
//      access()와 동일한 3분기 규칙.
// **이 함수 밖에서 먼저 걸러야 하는 것**: 커널/ProcessRole::
// KernelService 예외(§3 순서상 1번, role 개념 자체가 이 함수의
// uid/gid 파라미터로는 표현 안 됨)와 조상-자손 관계 예외(§3 순서상
// 3번, Process 트리 구조가 필요해 이 범용 함수의 관심사가 아님) -
// 둘 다 자원별 소비자(예: process.cpp의 kCanSendSignal)가 이 함수를
// 부르기 전에 먼저 확인한다.
inline bool kCheckPermission(Uid callerUid, Gid callerGid, Uid resourceUid, Gid resourceGid, Permission mode,
                              Permission requested) {
    if (callerUid == kRootUid) {
        return true;
    }
    Permission categoryBits;
    if (callerUid == resourceUid) {
        categoryBits = static_cast<Permission>((mode >> 6) & 0x7u);
    } else if (callerGid == resourceGid) {
        categoryBits = static_cast<Permission>((mode >> 3) & 0x7u);
    } else {
        categoryBits = static_cast<Permission>(mode & 0x7u);
    }
    // `requested`는 항상 kPermOwner*(비트 6~8) 상수로 전달된다는 관례 -
    // "어느 카테고리(owner/group/other)인지"가 아니라 "어떤 동작
    // (r/w/x)인지"만 그 비트 위치로 표현하므로, 항상 owner 슬롯 기준
    // 3비트로 뽑는다(카테고리는 위에서 uid/gid 비교로 이미 결정됨).
    const Permission requestedAction = static_cast<Permission>((requested >> 6) & 0x7u);
    return (categoryBits & requestedAction) == requestedAction;
}

}  // namespace kernel

#endif  // MINICORE_LIBS_LIBKENV_PERMISSION_H
