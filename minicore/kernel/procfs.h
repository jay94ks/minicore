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
// "proc/" 접두사를 뗀 나머지만 받는다("self/status", "<pid>/status" 등).
//
// [확장, 2026-09-19, PN-85FA4992] v1 스코프(SP-5D965B74 §2)는
// `self/status` 하나뿐이었으나, `QU-764C5624`("pid -> Process* 안전
// 조회 수단 없음" 문제)가 `SP-9CB55C5B`의 세대 태그 `ProcessId` 캐패빌리티
// 패턴으로 해결되면서 `<10진수 pid>/status`도 함께 지원한다 -
// `Process::resolveById()`로 매 접근마다 재해석하고(원시 포인터를
// 오래 들고 있지 않음), `kCanViewProcessStatus()`로 열람 권한을
// 확인한다(부모/자신/KernelService만 허용 - `process.h` 문서 주석
// 참고). 그 외 경로는 여전히 전부 NotFound.
//
// [추가, 2026-09-17, PN-0C282BB7] `meminfo`/`uptime` - 특정 프로세스가
// 아니라 커널 전역 상태를 노출하는 두 번째 부류의 파일. 이들은
// `Process*`에 매이지 않으므로 §2의 "self만 허용" 권한 제약과 무관
// (설계 확인 결과 - 특정 프로세스에 종속되지 않는 전역 정보라 접근
// 제어 필요 없음, `PN-0C282BB7` §4 참고) - 호출자가 누구든 항상 읽을
// 수 있다.
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

// [추가, 2026-09-17, PN-0C282BB7] `meminfo`/`uptime` 핸들은 `Process*`를
// 담지 않는 상태 없는 전역 핸들이라, 위 태그 비트만으로는 self/status
// 핸들(실제 `Process*` | kProcFsHandleTagBit)과 구분할 수 없다 -
// `Process*`는 항상 8바이트 정렬이라 태그 비트(bit1)만 OR해서는 bit2가
// 절대 서지 않는다는 사실을 이용해, bit2를 "이 핸들은 프로세스에
// 안 매인 전역 통계 파일"이라는 두 번째 표시로 예약한다(self/status
// 핸들과 절대 충돌 안 함 - 그쪽은 bit2가 항상 0).
constexpr uint64_t kProcFsGlobalHandleBit = 1ULL << 2;

// [신규, 2026-09-19, PN-85FA4992] "이 핸들의 나머지 비트가 `ProcessId`
// (세대 태그 슬롯 인코딩)이지, `Process*` 원시 포인터가 아니다"라는
// 표시. bit0은 `kProcFsHandleTagBit`(bit1)과 마찬가지로 원시 포인터가
// 8바이트 정렬이라 항상 0인 자리라서, 레거시 self 핸들(원시 포인터 |
// `kProcFsHandleTagBit`, bit0=0)과 절대 겹치지 않는다 - 반대로 bit3처럼
// 포인터의 실제 주소 비트가 걸쳐 있는 위치는 값에 따라 1일 수도 있어
// 판별용으로 못 쓴다(포인터 하위 3비트만 정렬 보장, 그 이상은 임의).
// pid 인코딩은 `(pid << 4) | kProcFsHandleTagBit | kProcFsPidHandleBit`,
// 복원은 `handle.value >> 4`.
constexpr uint64_t kProcFsPidHandleBit = 1ULL << 0;

class ProcFs {
public:
    // [확장, 2026-09-19, PN-85FA4992] `self` 뿐 아니라 임의/자식 pid도
    // 대상이 될 수 있다(`kCanViewProcessStatus()` 통과 시) - 대상
    // 프로세스가 `kAllocateProcessId()`로 발급받은 유효한 `processId`를
    // 가지면 그 pid를 재해석 가능한 캐패빌리티로 인코딩하고, (self를
    // 여는 커널 서비스 프로세스처럼) `processId`가 없으면 기존 v1과
    // 동일하게 원시 포인터 핸들로 폴백한다(process.h `ProcessId` 문서
    // 주석 - devmgr/fs/net/tty 등은 애초에 `processId`를 안 받음).
    // PermissionDenied는 제출자를 못 찾을 때(이론상 도달 불가) 또는
    // `kCanViewProcessStatus()`가 거부할 때.
    static OpenResult open(AsyncTask* task, const char* relPath, uint32_t relPathLen, uint32_t flags);

    // pid 인코딩 핸들(`kProcFsPidHandleBit`)은 Read할 때마다
    // `Process::resolveById()`로 다시 해석한다 - 대상이 Open~Read
    // 사이에 죽었으면(세대 불일치) `NotFound`를 돌려줄 뿐 댕글링
    // 역참조가 나지 않는다(`SP-9CB55C5B`가 `Kill`에 적용한 것과 동일한
    // 안전성). 레거시 원시 포인터 핸들(위 open() 문서 주석의 폴백
    // 경로)은 이 안전성이 없다는 기존 v1 한계를 그대로 물려받는다
    // (LiveFs의 named/kernel 핸들과 동일한 처지, SP-2AAD7C8D §9 착수
    // 시 재검토 대상).
    static ReadResult read(FileHandle handle, uint64_t offset, void* buf, uint32_t len);

    static void stat(AsyncTask* task, KernelFsStatArgs* args);

    // [신규, 2026-09-19, PN-770A28FB 항목6] `/sys/live/proc` 나열 -
    // `relPathLen==0`으로 `open()`을 호출하면 이 디렉터리 자신의
    // 핸들(전역 핸들 계열, `kProcFsGlobalHandleBit` 세 번째 인덱스)을
    // 돌려준다. v1 스코프는 이 최상위 고정 이름 3개(`self`/`meminfo`/
    // `uptime`)뿐 - `open`/`read`/`stat`은 PN-85FA4992로 임의 pid까지
    // 열람 가능해졌지만, 그 pid 아래(또는 `self` 아래)를 나열하는 것은
    // 여전히 스코프 밖(그 안이 `status` 파일 하나뿐이라 나열할 실익이
    // 낮음 - RM-23F4B687 §4가 결정할 순수 구현 세부로 남겨둠).
    static void readdir(KernelFsReaddirArgs* args);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PROCFS_H
