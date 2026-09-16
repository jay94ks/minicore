#ifndef MINICORE_KERNEL_PROCFS_H
#define MINICORE_KERNEL_PROCFS_H

#include "async_task.h"
#include "libkenv/types.h"
#include "mount_table.h"

namespace kernel {

// SP-5D965B74 - `/sys/live/proc`의 실제 구현. `LiveFs`(livefs.h)의
// 네 번째 하위 경로("proc/")로 위임되는 순수 헬퍼다 - `LiveFs`가
// 이미 `KernelFsDriver`(AsyncTaskHandler) 진입점을 갖고 있으므로
// `ProcFs` 자신은 별도로 그 계층을 상속하지 않는다. `relPath`는
// "proc/" 접두사를 뗀 나머지만 받는다("self/status" 등).
//
// v1 스코프(SP-5D965B74 §2) - `self/status` 하나뿐, 그 외 경로는
// 전부 NotFound. 임의/자식 pid 열람은 `QU-764C5624`(Kill/DebugAttach가
// 이미 마주친 "pid -> Process* 안전 조회 수단 없음" 문제)의 답변
// 이후로 미룬다.
// [핸들 충돌 방지] `LiveFs`의 `named/`/`kernel/<name>` 핸들은 태그 없는
// 원시 `Channel*` 값을 그대로 `FileHandle::value`로 쓴다(livefs.cpp) -
// `ProcFs`도 원시 `Process*`를 그대로 쓰면 `Read`/`Stat` 디스패치가
// (핸들에 "어느 하위 경로 것인지" 정보가 전혀 없으므로) 두 종류를
// 구분할 수 없어, 우연히 같은 숫자값이면 `Channel*`을 `Process*`로
// 잘못 캐스트해 역참조하는 사고가 날 수 있다. `Process`/`Channel`
// 둘 다 8바이트 이상 정렬된 힙 객체라(포인터/uint64_t 필드를 가진
// C++ 클래스는 항상 최소 8바이트 정렬) 원시 포인터 값의 최하위 3비트는
// 항상 0이다 - 그중 비트1을 `ProcFs` 전용 태그로 예약해도(`kInitrdCpio
// HandleValue`=1, 이진 0b01과도 겹치지 않음 - 그 값은 비트1이 0이다)
// 실제 원시 포인터와 절대 충돌하지 않는다.
constexpr uint64_t kProcFsHandleTagBit = 1ULL << 1;

class ProcFs {
public:
    // 호출자 자신(task->submitterTask.lock()로 얻는 제출자의 process)
    // 만 대상으로 한다 - PermissionDenied는 제출자를 못 찾을 때(이론상
    // 도달 불가, SpawnProcessHandler 등과 동일한 방어적 처리)만 발생.
    static OpenResult open(AsyncTask* task, const char* relPath, uint32_t relPathLen, uint32_t flags);

    // FileHandle::value는 Open 시점에 캐낸 Process*를 그대로 담는다
    // (named/kernel 하위 경로가 Channel*/objectId를 담는 것과 동일한
    // 관례) - 그 프로세스가 Open~Read 사이에 죽으면 댕글링이 될 수
    // 있다는 한계도 동일하게 물려받는다(진짜 fd 테이블이 아직 없어
    // 이 VFS 계층 전체가 공유하는 v1 한계, SP-2AAD7C8D §9 착수 시
    // 재검토 대상 - LiveFs의 named/kernel 핸들과 정확히 같은 처지).
    static ReadResult read(FileHandle handle, uint64_t offset, void* buf, uint32_t len);

    static void stat(AsyncTask* task, KernelFsStatArgs* args);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PROCFS_H
