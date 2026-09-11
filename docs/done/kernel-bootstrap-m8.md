# 완료 보고: kernel-bootstrap M8 — initrun 로딩 (최종 마일스톤)

**대상 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) §M8
**관련 스펙**: [boot.md](../spec/boot.md) §4~6
**관련 결정**: ADR-002, 009, 011, 017, 030, 074, 114, 117, 118, 119, 120,
121, 122, 123, 124
**실행일**: 2026-09-08

## 완료 기준 달성 확인

M8, 그리고 kernel-bootstrap.md 계획 전체의 최종 완료 기준은 명시적이다:
"커널이 유저모드 initrun을 실행하고, initrun이 커널에 보낸 IPC Call에
대한 응답을 받는다." QEMU 실행으로 확인했다 — 5회 반복 실행(재빌드
없이) 모두 바이트 단위로 동일한 결과였다.

```
cmake --preset x86_64-clang -S . -B build/x86_64-clang
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => 29개 항목 모두 PASS (M1~M7 25개 + M8 4개)
```

실제 QEMU 출력(뒷부분, 전체는 M1~M7과 동일하게 이어짐):

```
[ipc2] setup ok=1
[initrun] mcpack find_entry ok=1 size=0x14a0
[initrun] load_elf ok=1 entry=0x10000000
[initrun] setup_initrun_process ok=1
[sched] thread A iteration 0
[sched] thread B iteration 0
[ipc2] receiver sys_recv ok=1 page_count=1 content_ok=1 handle_count=1 received_handle_kind=3 (expect notification=3)
[ipc2] notifier sys_notify ok=1
[sched] thread A iteration 1
[sched] thread B iteration 1
[ipc] server sys_recv ok=1 badge=0xcafe (expect 0xcafe) label=0x1234 regs0=41
[ipc] server sys_reply sent
[ipc2] sender sys_call ok=1 ack_label=0xacc0
[ipc2] receiver sys_wait ok=1 bits=0x2 (expect 0x2)
[sched] thread A iteration 2
[sched] thread B iteration 2
[ipc] client sys_call ok=1 reply_label=0x5eed (expect 0x5eed) reply_regs0=42 (expect 42)
[sched] thread A done
[sched] thread B done
[initrun] kernel received boot call ok=1 label=0xb007 (expect 0xb007) - 부팅 성공
```

마지막 줄이 이 milestone의 증거다: **유저모드(CPL=3)에서 실행 중이던
initrun이 SYSCALL 명령으로 커널에 IPC Call을 보냈고, 커널 쪽 수신
스레드가 그 호출을 sys_recv로 받아 label(0xB007, initrun main.cpp와
kernel_main.cpp가 공유하는 관례값)이 정확히 일치함을 확인한 뒤
sys_reply로 응답했다.**

## 이번 milestone에서 새로 설치한 QEMU에 대하여

이 대화 세션 도중 이 개발 머신에서 `qemu-system-x86_64`를 찾을 수
없는 상태였다(과거 세션에서 winget으로 설치했던 흔적 — 캐시된
설치 파일 — 만 남아 있고, 실제 설치는 사라져 있었다). 사용자에게
"윈도우 호스트의 QEMU를 지우고 QEMU 소스를 직접 clone해 x86_64
전용으로 빌드해 쓰는 게 어떤가"라는 제안을 받았으나, 이 프로젝트가
이미 LLVM도 winget으로 설치한 선례([toolchain-setup.md](toolchain-setup.md))가
있고 소스 빌드는 MSYS2/Meson/Ninja 등 상당한 추가 설치·시간이 필요해
`winget install --id SoftwareFreedomConservancy.QEMU`로 재설치하는
쪽을 택했다 — 이 문서가 유일한 기록이다(ADR-031과 같은 정신: 툴체인은
저장소 밖에 둔다).

## 수행한 작업

### 1. MCPACK v1 패키징 도구 (boot.md §5)

[tools/mkinitrd.py](../../tools/mkinitrd.py) — 기존 스텁(`mkinitrd.sh`,
"TODO")을 대체했다. 서드파티 tar/cpio 대신 스펙이 정의한 최소 포맷을
그대로 구현한다. **처음에 겪은 버그**: `mcpack_entry`(`char name[60];
uint64_t offset; uint64_t size;`)를 파이썬 `struct` 모듈로 타이트하게
패킹했더니(76바이트), C++ 쪽의 자연 정렬 구조체(80바이트 — `name[60]`
뒤 4바이트 패딩)와 레이아웃이 어긋나 `offset`/`size` 필드가 4바이트씩
밀려 읽혔다 — `find_entry`가 `truncated`로 계속 실패했다. `struct`
포맷 문자열에 `4x`(패딩 4바이트)를 추가해 C++ 자연 정렬과 바이트
단위로 맞춰 해결했다.

### 2. MCPACK 파서 (kernel/core/initrd, boot.md §5)

[mcpack.hpp](../../kernel/core/initrd/mcpack.hpp)/[.cpp](../../kernel/core/initrd/mcpack.cpp) —
arch 독립(ADR-002). `__builtin_strncmp`를 쓰려다 이 freestanding
빌드에 `strncmp` 구현이 없어(freestanding_mem.cpp는 memcpy/memset류만
제공) 링크 실패를 겪었고, 직접 루프(`name_matches`)로 대체했다.

### 3. 최소 ELF64 로더 (kernel/arch/x86_64, ADR-002)

[elf_loader.hpp](../../kernel/arch/x86_64/elf_loader.hpp)/[.cpp](../../kernel/arch/x86_64/elf_loader.cpp) —
`PT_LOAD` 세그먼트만 처리하는 최소 로더(동적 링킹·재배치 없음,
`ET_EXEC` 고정). 세그먼트별로 페이지를 `mm::alloc_pages`로 확보해
0으로 채운 뒤 파일 범위만 복사하고(`.bss`는 자동으로 0), `p_flags`에서
`page_perm`(write/exec/user)을 파생해 `arch_x86_64::map_page`로 매핑한다.

### 4. GDT 확장 + SYSCALL/SYSRET (M8의 핵심 신규 메커니즘)

- [boot.S](../../kernel/arch/x86_64/boot/boot.S) — `user_data64`/
  `user_code64`(DPL=3) 두 엔트리를 GDT에 추가했다. SYSRET(64비트)의
  잘 알려진 규약(`CS=STAR[63:48]+16`, `SS=STAR[63:48]+8`)에 맞춰
  "data 먼저, 그 바로 다음이 code" 순서로 배치했다.
- [syscall.cpp](../../kernel/arch/x86_64/syscall.cpp) — STAR/LSTAR/FMASK
  MSR 설정 + syscall 번호 디스패처. **이 커널 자체 ABI**를 정했다
  (ADR-122): 번호는 RDI, 4번째 인자는 RCX 대신 R10(SYSCALL이 RCX를
  복귀 RIP로 덮어써 못 쓴다 — 리눅스와 같은 이유).
- [syscall_entry.S](../../kernel/arch/x86_64/syscall_entry.S) — 저수준
  진입/복귀 스텁. 유저 스택에서 커널 스택으로 전환(전역 스크래치
  `g_syscall_kernel_rsp`), RCX/R11 보존, `syscall_dispatch` 호출 후
  SYSRETQ.
- [usermode.S](../../kernel/arch/x86_64/usermode.S) —
  `enter_usermode(rip, rsp, arg0)`: 최초 유저모드 진입은 SYSRET가
  아니라 IRETQ로 한다(SYSRET는 직전 SYSCALL이 남긴 RCX/R11 전제가
  필요해 "한 번도 SYSCALL한 적 없는 새 스레드"의 최초 진입에 안
  맞는다). arg0을 RDI에 남겨 boot.md §6의 "첫 인자 레지스터"
  관례(ADR-030)를 그대로 만족한다.
- [user_thread.cpp](../../kernel/arch/x86_64/user_thread.cpp) —
  `arch_user_thread_trampoline()`: 유저 스레드가 스케줄러에 의해
  "처음" 재개될 때 진입하는 자리(`create_kernel_thread`의 entry
  함수 자리를 유저 스레드는 이걸로 대신한다, `sched::create_user_thread`
  참고).

### 5. 스케줄러 확장 (kernel/core/sched)

- `arch_context_switch`에 3번째 인자(`new_pml4_phys`)를 추가해 CR3
  전환을 지원한다 — 0이면 유지(커널 스레드끼리는 항상 0, 불필요한
  TLB 플러시를 피한다), 0이 아니면 그 물리주소를 적재한다.
- `object::thread`에 유저 스레드 전용 필드(`handles`, `user_entry_rip`,
  `user_rsp`, `user_arg0`)를 추가했다.
- `sched::create_user_thread()` — `create_kernel_thread`와 같은 패턴
  (손으로 짠 초기 컨텍스트)으로 유저 스레드를 만든다.
- **`sched::exit()` 신설, `flush_yield()` 관례 폐기** — 아래 "발견한
  버그" 참고. 모든 데모 스레드가 이제 `sched::exit()` 한 번만 부른다.
- [idle.S](../../kernel/arch/x86_64/idle.S) — `arch_idle_halt()`,
  `exit()`가 아무도 안 남았을 때 부르는 마지막 자리.

### 6. initrun 최소 유저 프로그램

[init/initrun/main.cpp](../../init/initrun/main.cpp) — 콘솔·파일시스템
등 유저모드 I/O가 전혀 없는 이 마일스톤에서, initrun의 유일한 "출력"은
커널에 보내는 IPC Call 자체다(레이블 0xB007) — 커널 쪽 수신 스레드가
그 사실 자체를 klog로 기록하는 것이 관찰 수단이다. 고정 핸들 번호
(1 — `handle_table` 첫 슬롯은 항상 1)로 boot endpoint를 참조하는
M8 한정 관례를 문서화했다. 당시 `kernel/include/uapi.hpp`(M50/
ADR-200 이후 `libs/mc/include/mc/syscall.h`로 흡수+이 헤더 자체는
폐지) — 커널·유저가 공유하는 최소 syscall ABI(`ipc::message`와
바이트 단위로 동일한 레이아웃).

### 7. kernel_main.cpp — initrd 파싱 → ELF 로드 → 유저 스레드 생성 (boot.md §4)

`setup_initrun_process()`가 boot.md §4의 1~4단계를 전부 수행한다:
endpoint 생성(커널 쪽 CAN_RECV 소유 핸들 + initrun 쪽 CAN_SEND
프록시), initrun 전용 `handle_table` 생성, MCPACK에서 "initrun" 검색,
새 주소공간(**ADR-074: `trusted=true`로 무조건 생성** — initrun은
시스템의 유일한 최초 신뢰 루트), ELF 로드, 유저 스택 매핑, boot_info
페이지를 읽기전용으로 매핑 후 그 유저 가상주소를 RDI로 전달(§6) —
이 마일스톤은 아직 진짜 Multiboot2 boot_info가 없어(ADR-114) self-test
값을 싣지만, initrun은 이 내용을 아직 읽지 않는다.

## 실행 중 발견해 고친 버그(중요 — 세 가지, 모두 M8 전에는 한 번도
노출된 적 없던 코드 경로)

### 버그 1: flush_yield()의 근본적 취약성 — 협조적 스케줄러의 구조적 결함

M6/M7이 도입한 "무한 hlt 전에 넉넉히 yield()"라는 `flush_yield()`
관례는 겉보기엔 동작했지만 근본적으로 틀려 있었다: 서로 다른 스레드가
완료까지 필요로 하는 **총 yield 횟수**가 다르면(사전 작업이 없는
스레드 vs 반복 로직이 있는 스레드), 가장 적게 필요한 스레드가 가장
먼저 자기 몫을 다 쓰고 무한 `hlt`로 들어가는 바로 그 순간 자신이
"현재 실행 중"이었다면 — 아직 안 끝난 다른 스레드가 `run_queue`에
아무리 남아 있어도 아무도 그들을 깨울 수 없다(타이머 인터럽트가
없어 `hlt`는 영원히 안 돌아온다). M8에서 유저 스레드(initrun)가
처음 생기며 이 경합이 실제로 재현됐다 — A~I 7개 커널 스레드 중
1개(사전 작업 없는 "I")만 정상 완료하고 나머지는 영원히 멈췄다.
`object::thread` 스택의 지역 변수(반복 카운터)까지 직접 추적해
확진한 뒤, `sched::exit()`(ADR-124, [kernel-scheduler.md](../design/kernel-scheduler.md))로
근본적으로 고쳤다 — "다시 스케줄되지 않음"을 스케줄러 자신이
보장하므로 `run_queue`가 단조롭게 줄어 결국 정확히 빈다.

### 버그 2: 저지대 identity 매핑이 새 주소공간에 없어 GDT 자체가 미매핑

`create_address_space_root()`가 physmap·커널 이미지 엔트리만 공유하고
`pml4[0]`(GDT가 사는 부팅 시점 저지대 항등 매핑)은 공유하지 않아,
CR3가 새 주소공간으로 전환된 뒤 IRETQ가 새 CS/SS 셀렉터 검증을 위해
GDT를 읽으려는 순간 그 자체가 미매핑이라 #PF로 죽었다(`CR2`가 GDT
안의 정확한 엔트리 오프셋을 가리켰다 — QEMU `-d int`로 확진).
`table[0] = pml4[0]`을 추가해 고쳤다(ADR-121).

### 버그 3: 중간 레벨 USER 비트 누락 + .boot.bss 미보장 제로화(연쇄)

버그 2를 고친 뒤 새로 노출된 두 겹의 문제: (a) `pml4[0]`/`low_pdpt[0]`이
부팅 시점에 USER 비트 없이 만들어져 있어(원래 supervisor 전용
용도였다), 리프 자체는 정확히 USER로 매핑됐는데도(ELF 로더가
initrun 코드 페이지를 정확히 `exec+user`로 매핑함을 `query_page`로도
확인했다) x86 페이징의 "모든 레벨이 U/S를 가져야 함" 규칙 때문에
유저모드 명령어 페치가 계속 보호 위반으로 실패했다 — `PAGE_PRESENT_RW`
상수에 USER 비트를 추가해 고쳤다(ADR-121). (b) 그 과정에서 ELF
로더가 처음으로 `low_pd`의 (부팅 시점에 명시적으로 채운 [0,3] 밖)
미기록 엔트리를 실제로 걸어 봤는데, `.boot.bss`가 NOBITS 관례대로
0으로 시작한다는 보장이 이 QEMU PVH 직접 부팅 경로(qboot.rom,
ADR-114)에서는 검증된 적이 없었다는 것도 함께 드러나(초기 링크
주소 0x400000이 저지대 8MiB identity 범위와 겹쳐 `map_page`가 거대
페이지를 다음 레벨 테이블로 오인하는 별개 문제까지 섞여 있었다 —
링크 주소를 0x10000000으로 옮겨 회피, ADR-123), `_start32`에 명시적
`rep stosl` 제로화를 추가했다(ADR-120).

이 세 버그를 순서대로 진단하는 과정에서 QEMU `-d int,cpu_reset`
디버그 로깅, 레지스터 덤프 해석(에러코드 비트 분해), `objdump`
역어셈블·`nm` 심볼 대조, 그리고 임시 klog 계측(스레드별 이름 태깅,
슬랩 할당 격리 테스트, 페이지 테이블 질의)을 광범위하게 사용했다 —
전부 최종 커밋 전에 제거했다.

## 검증 결과 (정직하게 보고)

- **확인함**: MCPACK 파싱(`find_entry`) → ELF 로드(`load_elf`, 세그먼트
  권한 포함) → 새 주소공간·핸들 테이블 생성 → 유저 스레드 생성·등록
  → 스케줄러가 실제로 유저 스레드에게 차례를 줌 → CR3 전환 →
  IRETQ로 ring3 진입(CPL=3, CS/SS 정확히 검증됨, QEMU 레지스터
  덤프로 확인) → initrun이 SYSCALL 실행 → syscall_entry가 커널
  스택으로 전환·디스패치 → `ipc::sys_call`이 커널 쪽 수신 스레드를
  깨움 → 수신 스레드가 label을 정확히 확인하고 `sys_reply` → 5회
  반복 실행 모두 바이트 단위로 동일. 이것으로 "커널이 유저모드
  initrun을 실행하고, initrun이 보낸 IPC Call에 커널이 응답한다"는
  계획 전체의 최종 완료 기준이 실행으로 증명됐다.
- **확인함**: `nm`으로 `GLOBAL__sub_I` 부재 확인(ADR-118 계속 준수).
- **확인함**: x86_64 컴파일러 경고 0개(최종 빌드, clean rebuild).
- **확인하지 못함**: initrun이 syscall 응답(`ack`)을 실제로 수신해서
  뭔가 하는지 — initrun은 syscall 반환 이후 `pause` 무한 루프로
  들어가며, 반환값 자체를 검사·출력하지 않는다(유저모드 출력 경로가
  전혀 없다는 이 마일스톤의 한계, main.cpp 상단 주석 참고). syscall이
  값을 반환한다는 것(RAX에 `ipc_error::ok`)은 `syscall.cpp`의 코드
  경로로만 확인했다 — 유저 프로그램이 그 값을 관찰하는 것까지는
  검증 범위 밖이다.
- **확인하지 못함**: `boot_info` 페이지 전달 자체의 올바름 — 유저
  가상주소로 매핑까지는 확인했지만(읽기전용 매핑 성공), initrun이
  실제로 그 포인터를 역참조해 내용을 읽는 코드는 이 마일스톤에
  없다(위 "알려진 단순화").
- **확인하지 못함**: `map_page()`가 저지대 항등 매핑과 겹치는 가상
  주소를 매핑하려 들 때의 동작 — 링크 주소를 옮겨(ADR-123) 이 경로
  자체를 피했을 뿐, 그 상황에서 조용히 매핑이 깨지는 근본 문제
  자체는 고치지 않았다(코드 리뷰로 재현 메커니즘만 파악).
- **확인하지 못함**: 실제 Multiboot2/GRUB 경로에서의 initrd 모듈
  전달 — ADR-119가 명시하듯 이번 데모의 initrd는 커널 이미지에 직접
  임베딩됐다(ADR-114/117과 동일한 개발 환경 한정 한계의 연장).
- **확인함**: aarch64 `cmake --preset aarch64-clang` 구성 성공(회귀
  없음) — `kernel/arch/aarch64/`는 여전히 빈 스텁이라(M1~M8 범위 밖,
  ADR-009) 실제 컴파일 대상 자체가 없다(M1~M7과 동일한 상태).

## kernel-bootstrap.md 계획 전체 완료

M1~M8이 모두 완료되어, 이 계획 문서가 목표한 "부팅 → 최소 초기화 →
initrun 진입 → IPC 왕복 1회 성공"까지의 **최초 수직 슬라이스**가
끝났다. 다음은 별도 계획 문서(M9 이후, AP 기동·IPI·TLB shootdown 등,
kernel-bootstrap.md "M9 이후" 절 참고)로 넘어간다 — 이 계획 문서
자체는 실행 후에도 수정하지 않고 "실행 전 계획" 그대로 보존한다
(CLAUDE.md 원칙).
