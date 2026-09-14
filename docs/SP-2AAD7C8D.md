# mmap 서브시스템 및 Maple Tree 자료구조 — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-2AAD7C8D
  status: review
  updatedAt: 2026-09-14T16:27:49.877Z
  갱신: node scripts/export-cnw-docs.mjs
-->
# mmap 서브시스템 및 Maple Tree 자료구조 — 설계 제안

설계자 지시(2026-09-14, 메시지 3건, 모두 RM-9B8CA541에 대한 후속) -
"가상 주소 관리자는 프로세스별 주소 할당을 담당하는 파트와 커널 영역만을
담당하는 파트로 나눠야 해. 왜냐하면, 커널 영역은 SMP에서 모든 CPU가
공통된 메모리를 봐야 하거든.", "Maple Tree를 구현할 것.
(https://www.minzkn.com/linuxkernel/pages/maple-tree.html)",
"mmap 서브시스템을 지금 설계하도록 해." **이 문서는 RM-9B8CA541("Minicore
가상주소 공간 관리자 위임 사항")이 미뤄 뒀던 mmap 서브시스템 설계를
실제로 수행한 것이다 - RM-9B8CA541은 이 문서로 대체/흡수되어 종료된다
(그 문서의 "이 위임이 끝나는 조건" 항목이 정확히 이 상황이다).**

**[확장, 2026-09-14, 설계자 후속 지시]** - "표준 파일 API들도 모두
설계에 포함시켜." §9에서 open/close/read/write/lseek/stat/readdir/
mkdir/unlink 전체를 다룬다 - VFS 라우팅(SP-7CC5693A §2.2의
`ResolvePathArgs`)과 이 문서의 프로세스 자료구조를 엮는 지점이다.

## 1. 왜 두 파트로 나누는가

설계자 지적대로, 이 프로젝트의 주소공간 구조(`Paging::createAddressSpace`,
SP-68182FBD)는 **PML4 인덱스 256-511(higher half)을 모든 프로세스가
공유**한다 - 인덱스만 복사할 뿐 그 아래 PDPT/PD/PT 물리 페이지 자체는
전부 같은 것을 가리킨다. 즉:

- **커널 영역(higher half) 매핑을 하나 바꾸면 - 새 페이지를 매핑하든,
  권한을 바꾸든 - 그 순간 그 프로세스 뿐 아니라 시스템의 다른 모든
  프로세스, 그리고 이미 각자 다른 CPU에서 실행 중인 다른 코어들도
  전부 그 변경을 보게 된다.** 이건 인덱스 공유의 자연스러운 결과이자
  동시에 위험 요소다 - 여러 코어가 동시에 커널 영역 주소를 할당/해제
  하면 전역적으로 한 곳(자료구조 자체와, 각 코어의 TLB 캐시)에서
  경합이 생긴다.
- **유저 영역(lower half, 인덱스 0-255) 매핑은 프로세스마다 완전히
  독립**이다 - 프로세스 A가 자기 유저 영역에 뭘 매핑하든 프로세스
  B나 다른 코어의 TLB에 전혀 영향이 없다(그 프로세스를 스케줄링한
  코어의 TLB만 관련).

따라서 자료구조는 같아도(§3의 Maple Tree) **인스턴스 개수와 동시성
보호 방식이 근본적으로 다르다** - 이것이 두 파트로 쪼개는 이유다.

## 2. 전체 구조

```
[ProcessAddressSpaceManager]  - Process마다 하나(인스턴스 N개, N=프로세스 수)
        내부: Maple Tree 1개 - 이 프로세스의 유저 영역(lower half) VMA
        동시성: Spinlock 1개(프로세스당) - v1은 프로세스당 스레드 1개
                뿐이라 사실상 경합 없음(SP-04EE2A18 각주 - 멀티스레드
                프로세스는 후속 과제), 락은 미래를 대비한 최소 방어.

[KernelAddressSpaceManager]   - 시스템 전체에 **딱 하나**(싱글턴)
        내부: Maple Tree 1개 - 커널 영역(higher half) 중 "동적 할당
              가능"으로 정한 하위 범위의 VMA(§5 참고 - 부팅 시 고정
              배치된 direct map/커널 이미지/스택 풀 등은 이 관리자
              범위 밖, 건드리지 않음).
        동시성: **전역 Spinlock 1개** - 모든 CPU가 이 자료구조에 대해
                직렬화된다(설계자 지적의 핵심 - "모든 CPU가 공통된
                메모리를 봐야" 하므로 프로세스별 락으로는 안 되고
                시스템에 하나뿐인 락이어야 한다).
```

## 3. Maple Tree 자료구조 설계

Linux 6.1의 Maple Tree(RB-Tree를 대체한 VMA 관리 구조, 참고:
https://www.minzkn.com/linuxkernel/pages/maple-tree.html)를 이
프로젝트의 freestanding 제약에 맞게 이식한다.

### 3.1 노드 레이아웃 (256바이트 고정 - Slab 재사용)

Linux 원본과 동일하게 **노드 하나 = 256바이트**로 고정한다 - 이 크기가
SP-D7013B26(Slab 할당자, 이미 구현 완료)의 **7단계 버킷 중 하나(256B)와
정확히 일치**하므로, 전용 페이지 풀을 새로 만들 필요 없이
`GenericSlabAllocator::alloc(256)`/`free(ptr, 256)`을 그대로 재사용한다
- 이 프로젝트가 아직 갖추지 못한 것(RCU, 캐시라인 정렬 커스텀 슬랩)
없이도 Linux가 노린 "캐시라인 친화적 노드 크기" 이점을 그대로 얻는다.

```cpp
// minicore/libs/libkenv, 가칭 maple_tree.h - freestanding 제약 준수
// (표준 정수 타입 대신 libkenv 관례 타입, <cstring> 없이 reinterpret_cast
// 또는 libkenv/mem.h의 memcpy/memset만 사용).
namespace kernel {

constexpr uint32_t kMapleRangeSlotCount = 16;   // maple_range_64와 동일
constexpr uint32_t kMapleArangeSlotCount = 10;  // maple_arange_64와 동일(gap 배열 자리 확보)

// 일반 노드(내부/리프 공용) - 256B.
struct MapleRangeNode {
    void* parent;
    uint64_t pivot[kMapleRangeSlotCount - 1];  // 15개 경계값
    void* slot[kMapleRangeSlotCount];          // 자식 포인터(내부) 또는 값 포인터(리프), NULL=gap
};
static_assert(sizeof(MapleRangeNode) <= 256, "slab 256B 버킷 초과");

// gap 탐색용 확장 노드 - 256B(슬롯 수가 적은 대신 gap 배열을 갖는다).
struct MapleArangeNode {
    void* parent;
    uint64_t pivot[kMapleArangeSlotCount - 1];  // 9개 경계값
    void* slot[kMapleArangeSlotCount];          // 10개
    uint64_t gap[kMapleArangeSlotCount];        // 각 슬롯 서브트리의 최대 연속 gap 크기
    uint16_t maxGapSlot;                        // 가장 큰 gap을 가진 슬롯 인덱스(meta 역할)
};
static_assert(sizeof(MapleArangeNode) <= 256, "slab 256B 버킷 초과");

}  // namespace kernel
```

**v1 단순화(설계자 재확인 필요 없음, RM-23F4B687 §4 취지 - 숫자/구조
튜닝)**: Linux는 포인터 하위 비트에 노드 타입을 인코딩(256B 정렬로
8비트 여유)하지만, 이 프로젝트는 아직 그런 포인터 태깅 관례가 없다 -
v1은 대신 **모든 노드가 `MapleArangeNode`로 통일**된 단일 타입을
제안한다(항상 gap 배열을 갖되, 리프 노드에서는 그 필드를 안 씀 -
메모리 낭비는 있지만(리프 노드도 256B 전부 소비) 타입 분기/포인터
태깅 복잡도가 아예 없어져 v1 구현이 훨씬 단순해진다). 실측 후 메모리
절약이 실제로 필요하면 `maple_dense`/`maple_leaf_64`류 구분을 추가하는
것을 후속 과제로 남긴다.

### 3.2 RCU 없음 - v1은 전역/프로세스별 락으로 대체 (중요, 설계자 확인 필요)

Linux의 Maple Tree는 **RCU 기반 lock-free 읽기**가 핵심 이점 중
하나다(§ Linux 통합 참고 - `rcu_read_lock`/`call_rcu`로 쓰기가 읽기를
막지 않음). **이 프로젝트는 RCU 인프라 자체가 없다**(quiescent state
추적, grace period, `call_rcu` 콜백 큐 등 - 전부 상당한 규모의 별도
서브시스템) - 처음부터 이걸 같이 설계하는 것은 이번 mmap 서브시스템의
범위를 크게 벗어난다.

**이 문서의 제안**: v1은 Maple Tree의 **자료구조/알고리즘(노드 분할,
gap 탐색)만 그대로 가져오고, 동시성 보호는 RCU 대신 §2의 Spinlock으로
대체**한다 - 읽기든 쓰기든 락을 잡는다(page fault 경로에서 읽기가
잦다는 점에서 RCU 대비 확실히 손해지만, 지금 이 프로젝트엔 페이지
폴트 자체가 아직 없고(§2-B 정책만 있고 구현 전), 프로세스당 스레드도
하나뿐이라 실제 경합이 거의 없다). **RCU 자체를 이 프로젝트에 도입할지
는 별도 설계 질문으로 남긴다**(§6-1 - 성능이 실제로 문제가 되는
시점까지는 락 기반으로 충분하다고 판단했으나, 이건 숫자 튜닝이 아니라
아키텍처 결정이라 확정 짓지 않고 열어 둔다).

### 3.3 핵심 연산

```cpp
namespace kernel {

// 값은 전부 void* - 리프에서는 사용자 데이터(§4의 Vma*), 내부에서는
// 자식 MapleArangeNode*.
class MapleTree {
public:
    void init();

    // [start, end] 범위에 value를 등록한다(겹치면 실패 - 호출부가
    // 먼저 findGap 등으로 빈 공간을 확인했다는 전제).
    bool store(uint64_t start, uint64_t end, void* value);

    // addr을 포함하는 범위를 찾는다(없으면 nullptr 반환 - "gap 안"이라는 뜻).
    void* find(uint64_t addr, uint64_t* outRangeStart, uint64_t* outRangeEnd);

    // size 이상인 연속 gap을 [searchFloor, searchCeil] 범위 안에서 찾는다
    // (§3.1의 gap 배열로 서브트리 단위 스킵 - mmap(hint=0) 구현의 핵심).
    bool findGap(uint64_t searchFloor, uint64_t searchCeil, uint64_t size,
                 uint64_t* outStart);

    // [start, end] 범위를 지운다(부분 겹침 - 범위의 일부만 지우는 것도
    // 지원해야 한다, munmap이 매핑의 중간 일부만 해제할 수 있으므로 -
    // Linux도 동일 요구사항).
    bool erase(uint64_t start, uint64_t end);
};

}  // namespace kernel
```

`store`/`erase`의 노드 분할/병합 알고리즘은 Linux 원본의 절차(§3.1의
`maple_big_node` 임시 버퍼에 기존 항목 + 신규 항목을 모아 분할점을
계산해 재분배)를 그대로 따르는 것을 제안한다 - 이 부분은 RCU가
빠진 것 외에는 알고리즘 자체를 바꿀 이유가 없다.

## 4. VMA 값 구조

```cpp
enum class VmaBacking : uint32_t {
    Anonymous,     // 요구 페이징(§2-B), 초기엔 매핑 없음 - 폴트 시 PageFrameAllocator로 채움
    FixedPhysical, // 물리주소가 이미 정해짐(MMIO/DMA 버퍼) - PnP §3.3/DMA 버퍼 관리자(SP-39F18E30)가 여기 해당
    FileBacked,    // §9의 파일 API로 매핑 - 파일 오프셋 범위를 이 VMA에 연결(§9.5)
};

struct Vma {
    uint64_t start, end;      // MapleTree의 키와 중복되지만 값 쪽에서도 바로 알 수 있게 보관(편의)
    uint32_t prot;            // Read/Write/Exec 비트
    VmaBacking backing;
    uint64_t fixedPhysAddr;   // backing==FixedPhysical일 때만 사용
    int32_t backingFd;        // backing==FileBacked일 때만 사용(§9.2의 fd)
    uint64_t fileOffset;      // backing==FileBacked일 때만 사용
    bool used = false;        // Slab 재사용 시 관례
};
```

`Vma` 자체도 256B 버킷보다 훨씬 작으므로 32B/64B 버킷(SP-D7013B26의
다른 버킷)에서 `GenericSlabAllocator`로 확보하는 것을 제안한다.

## 5. mmap/munmap/brk Syscall API

```cpp
// 유저 프로세스 → 커널: ProcessAddressSpaceManager(호출자 프로세스 것)를 사용.
struct MmapArgs {
    uint64_t hintAddr;    // in: 0이면 커널이 findGap으로 아무 곳이나 고름
    uint64_t length;      // in: 페이지 단위로 올림
    uint32_t prot;        // in
    uint32_t flags;       // in: v1은 Anonymous만 지원(Fixed 힌트 강제는 후속)
    // out
    uint64_t addr;
    ChannelError error;   // OutOfMemory / InvalidArgument
};

struct MunmapArgs {
    uint64_t addr;
    uint64_t length;
    // out
    ChannelError error;   // NotMapped
};

// brk는 프로세스당 "힙 VMA" 하나를 미리 예약해 두고 그 끝점만 움직이는
// 전통적 구현을 제안한다 - Process::init() 시점에 초기 크기 0으로
// 힙 VMA를 store()해 두고, brk() 호출마다 erase+store로 끝점을 재조정.
struct BrkArgs {
    uint64_t newBrk;      // in: 0이면 "현재 brk 조회"
    // out
    uint64_t currentBrk;
    ChannelError error;   // OutOfMemory(확장 실패 - 다음 VMA와 충돌 등)
};
```

`AllocDmaBuffer`(SP-39F18E30)/`RequestIoPermission`(SP-9DD4F3EA §3.3)은
이제 이 문서의 `ProcessAddressSpaceManager`를 내부적으로 사용하도록
갱신된다(§7 참고) - 둘 다 `VmaBacking::FixedPhysical` 종류의 VMA를
등록하는 특수 경우일 뿐이다.

## 6. 아직 열려 있는 설계 영역

1. **RCU 도입 여부** (§3.2) - v1은 락 기반, 필요성이 실제로 나타나면
   (멀티스레드 프로세스 다수 + 페이지 폴트 빈도가 실측으로 문제가
   될 때) 별도 아키텍처 결정으로 재검토 - 설계자 판단 필요할 수
   있는 사안이라 숫자 튜닝이 아니라고 명시해 둔다.
2. **커널 영역 TLB 샷다운 (신규 발견, 중요)**: `KernelAddressSpaceManager`
   가 매핑을 해제/변경하면, 그 변경 전 주소를 캐시하고 있는 **다른
   코어의 TLB**는 그 코어가 `invlpg`를 실행하기 전까진 갱신되지 않는다
   - 지금 이 프로젝트의 `Paging::unmapPage`(`paging.cpp` 91번째 줄
   근처 주석)는 "invlpg는 항상 지금 이 코어가 보고 있는 주소공간
   기준으로만" 동작한다고 스스로 명시하고 있고, **다른 코어에 invlpg를
   전파하는 IPI 기반 샷다운 메커니즘이 이 프로젝트에 아직 전혀 없다**
   (`smp.h`/`lapic.h`에 INIT-SIPI 부팅 시퀀스만 있고 런타임 TLB
   샷다운 IPI는 없음). **`KernelAddressSpaceManager`가 실제로 매핑을
   해제/재배정하는 기능까지 쓰려면 이 IPI 샷다운이 선행돼야 한다** -
   별도 계획으로 등록 필요(이 문서 범위 밖, §7에 선행 조건으로만 기록).
   유저 영역(`ProcessAddressSpaceManager`)은 이 문제가 없다 - 다른
   프로세스가 그 주소공간을 보고 있지 않으므로.
3. **`findGap`의 정확한 시작 지점(§5-A/§5의 이전 논의 상속)**: 코드/
   스택 경계를 피해 어디서부터 유저 영역 mmap을 시작할지 - SP-8B6B8D25
   §5-A가 아직 정하지 않은 힙/mmap 경계를 이 문서가 실질적으로 확정
   짓는다: **코드 공간 위쪽 ~ 스택 하단 사이 전부를 mmap 가능 영역으로
   본다**(v1 제안, 정확한 정렬/여유 공간 상수는 구현 시점 튜닝).
4. ~~**파일 백킹 mmap**~~ - **부분 해결(§9.5)** - `fs` 서비스
   (SP-7CC5693A)가 실제로 구현되기 전까지는 실행 가능한 코드가 없지만,
   `VmaBacking::FileBacked`의 자료구조/폴트 처리 흐름 자체는 §9.5에서
   설계했다.

## 7. 선행 조건

- Slab 할당자(SP-D7013B26, 이미 구현 완료) - 256B/32B 버킷 재사용.
- `Spinlock`(`libkenv/spinlock.h`, 이미 구현 완료, PL-65C20380) -
  `KernelAddressSpaceManager`/`ProcessAddressSpaceManager` 동시성 보호.
- **커널 영역 TLB 샷다운 IPI**(§6-2, 신규 - 아직 없음, 별도 계획 등록
  필요) - `KernelAddressSpaceManager`의 매핑 해제/변경 기능 사용 전
  필수.
- 프로세스 모델(PN-16CA347D) - `ProcessAddressSpaceManager`가 매달릴
  `Process` 구조체 자체.
- `Paging::mapPage`/`unmapPage`(이미 구현 완료) - 실제 페이지 테이블
  조작.
- VFS 커널 서브시스템(SP-7CC5693A) - §9의 파일 API가 경로 해석에
  재사용하는 `ResolvePathArgs`.

## 8. RM-9B8CA541과의 관계

RM-9B8CA541("Minicore 가상주소 공간 관리자 위임 사항")은 "mmap
서브시스템은 지금 안 만든다"는 결정을 기록한 문서였다 - 이번 설계자
지시로 그 결정이 뒤집혔으므로, RM-9B8CA541은 **이 문서로 대체되어
종료**된 것으로 표시한다(그 문서 자체는 "왜 한동안 미뤘었는지"의
역사적 기록으로 남겨 두되, 상태/본문에 이 문서로의 대체를 명시).
SP-39F18E30(DMA 버퍼 관리자) §3.2의 `ProcessVirtualAddressCursor`
(bump 전용 스텁)도 이제 이 문서의 `ProcessAddressSpaceManager`로
흡수된다 - `AllocDmaBuffer`는 이제 자체 커서 대신 `mmap`과 같은
`ProcessAddressSpaceManager::findGap`+`store(VmaBacking::FixedPhysical)`
경로를 재사용한다.

## 9. 표준 파일 API (2026-09-14, 설계자 지시 - "표준 파일 API들도 모두 설계에 포함시켜")

### 9.1 전체 흐름

```
유저 코드 (libmc가 감쌈)
   |  open("/sys/etc/foo.conf", ...)
   v
[Open syscall]
   |  1. ResolvePathArgs(SP-7CC5693A §2.2)로 경로 → (ownerChannelId, relPath)
   |  2. 그 Channel로 "Open(relPath, flags)" IPC 메시지 전송(PL-C8648D4D)
   |  3. fs 서비스가 FileSystemDriver::open() 호출 → 자기 내부 FileHandle 반환
   |  4. 커널이 Process의 파일 디스크립터 테이블에 {ownerChannelId, fsHandle, offset=0} 등록
   v
프로세스별 정수 fd 반환 → 이후 read/write/lseek/close는 전부 이 fd로만 참조
```

이 흐름은 PnP(SP-9DD4F3EA §3.1 "장치 열거 → IO 권한 요청")와 정확히
같은 2단계 패턴("커널이 라우팅 정보만 알려주고, 실제 작업은 그 대상과
직접 IPC")을 재사용한다.

### 9.2 프로세스 파일 디스크립터 테이블

```cpp
// minicore/kernel/process.h에 추가 제안 - DmaBuffer(SP-39F18E30 §4)와
// 동일하게 ChunkedList 재사용.
struct FileDescriptor {
    uint64_t ownerChannelId;  // 이 fd를 처리하는 fs 서비스의 Channel(SP-7CC5693A §2.2)
    uint64_t fsHandle;        // 그 fs 서비스 내부 FileSystemDriver::open()의 반환값
    uint64_t offset;          // POSIX 관례 커서 - read/write가 명시적 offset을 안 줄 때 이 값을 쓰고 자동 전진
    bool isDirectory;
    bool used = false;
};

// Process에 추가
ChunkedList<FileDescriptor, kFileDescriptorChunkCapacity> fileDescriptors;
```

**offset은 커널(fd 테이블)이 갖고, `FileSystemDriver::read/write`
(SP-7CC5693A §3.2)는 매번 명시적 offset을 받는 무상태 오퍼레이션이다**
- 이렇게 나누면 fs 서비스 드라이버 구현이 커서 상태를 신경 쓸 필요가
없고(POSIX `pread`/`pwrite`와 동일한 결), `dup()`류로 fd를 복제해도
어느 쪽이 offset을 소유하는지가 fd 테이블 하나로 명확하다.

### 9.3 Syscall API

```cpp
enum class OpenFlags : uint32_t {
    ReadOnly = 1 << 0,
    WriteOnly = 1 << 1,
    ReadWrite = ReadOnly | WriteOnly,
    Create = 1 << 2,
    Truncate = 1 << 3,
    Append = 1 << 4,
    Directory = 1 << 5,  // 디렉터리로 열기(readdir 전용)
};

struct OpenArgs {
    const char* path;      // in: 절대 경로
    uint32_t pathLen;
    uint32_t flags;        // in: OpenFlags 조합
    // out
    int32_t fd;            // 실패 시 -1
    ChannelError error;    // NotFound / PermissionDenied / AlreadyExists(Create+exclusive류 후속)
};

struct CloseArgs {
    int32_t fd;
    ChannelError error;    // InvalidHandle
};

struct ReadArgs {
    int32_t fd;
    void* buf;
    uint32_t len;
    // out
    uint32_t bytesRead;    // 0이면 EOF
    ChannelError error;
};

struct WriteArgs {
    int32_t fd;
    const void* buf;
    uint32_t len;
    // out
    uint32_t bytesWritten;
    ChannelError error;
};

enum class SeekWhence : uint32_t { Set, Current, End };

struct LseekArgs {
    int32_t fd;
    int64_t offset;
    SeekWhence whence;
    // out
    uint64_t newOffset;
    ChannelError error;
};

struct StatArgs {
    const char* path;      // in (fd 기반 fstat은 후속 변형으로 추가 가능)
    uint32_t pathLen;
    // out
    StatBuf stat;          // SP-7CC5693A §3.2 참고
    ChannelError error;
};

struct ReaddirArgs {
    int32_t fd;             // Directory 플래그로 연 fd
    // out
    DirEntry entry;         // SP-7CC5693A §3.2 참고 - 호출마다 다음 엔트리 하나
    bool hasMore;
    ChannelError error;
};

struct MkdirArgs {
    const char* path;
    uint32_t pathLen;
    ChannelError error;
};

struct UnlinkArgs {
    const char* path;      // 파일이면 unlink, 빈 디렉터리면 rmdir 라우팅(§9.4)
    uint32_t pathLen;
    ChannelError error;
};
```

`Read`/`Write`/`Lseek`/`Close`/`Readdir`는 전부 `fd`를 fd 테이블에서
찾아 `ownerChannelId`로 IPC 메시지를 보내는 동일한 패턴이다 - 명시적
offset이 필요한 `FileSystemDriver::read/write` 호출 시 fd 테이블의
`offset` 필드를 넘기고, 성공하면 `bytesRead`/`bytesWritten`만큼 그
필드를 전진시킨다(Append 플래그면 항상 파일 끝 기준으로 재계산 -
세부는 구현 시점).

### 9.4 `Mkdir`/`Unlink`가 `Stat`과 다른 점 - 디스크립터 없이 경로만으로 동작

`mkdir`/`unlink`/`rmdir`/`stat`은 파일을 "열지" 않고 경로만으로
수행되는 오퍼레이션이라, fd 테이블을 거치지 않고 `ResolvePathArgs`로
얻은 채널에 직접 1회성 메시지를 보내는 것으로 충분하다(§9.1의 4단계
흐름에서 3~4단계, "fd 등록"이 필요 없음) - `FileSystemDriver`(SP-7CC5693A
§3.2)의 `stat`/`mkdir`/`rmdir`/`unlink`가 전부 `FileHandle`이 아니라
`relPath`를 직접 받는 것도 이 때문이다.

### 9.5 파일 백킹 mmap과의 연결 (§4-4, §6-4 참고)

`mmap`(§5)에 `flags`로 "파일 백킹" 옵션이 추가되면(v1 이후, `fs`
서비스가 실제로 동작하는 시점), `Vma::backing = FileBacked`로
`backingFd`/`fileOffset`을 채워 등록한다 - 유저 모드 페이지 폴트
(SP-8B6B8D25 §2-B)가 이 VMA를 만나면, 이미 있는 `FileDescriptor`의
`ownerChannelId`로 그 오프셋 범위를 `Read`해 물리 페이지를 채우는
흐름이 된다(Anonymous VMA의 "PageFrameAllocator로 그냥 0으로 채움"과
갈라지는 지점 - 파일 백킹은 실제 데이터를 읽어와야 한다). 이 흐름의
정확한 캐시/dirty 페이지 되쓰기(writeback) 정책은 `fs` 서비스가
실제로 존재하기 전까지는 확정할 수 없어 후속 과제로 남긴다.

### 9.6 아직 열려 있는 설계 영역 (§9 전용)

1. **`dup`/`dup2`류 fd 복제**: 여러 fd가 같은 `FileDescriptor` 엔트리
   (또는 offset을 공유하는 엔트리)를 가리키는 경우의 참조 카운트 -
   `AtomicU32`(libkenv/spinlock.h, 이미 구현 완료) 재사용 제안, 세부는
   실제 착수 시점.
2. **경로 오프닝의 심볼릭 링크/`..`/`.` 정규화**: `ResolvePathArgs`
   (SP-7CC5693A §2.2)가 지금은 단순 접두사 매칭만 가정 - 심볼릭 링크는
   각 `fs` 드라이버가 있어야 의미가 생기므로 그때 같이 설계.
3. **`Open`의 `Create` 플래그 경합(동시에 두 프로세스가 같은 파일을
   O_CREAT|O_EXCL로 열 때)**: `fs` 서비스 쪽 드라이버의 원자성 보장
   범위 - 각 드라이버(ext4 등) 착수 시점의 구현 세부.
