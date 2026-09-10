# 실행 계획: 실제 musl syscall 계층 포팅 + 동적 링킹·멀티코어 선점·pthread·signal·SDK (M27~M39)

**관련 결정**: [foundations.md](../design/foundations.md) ADR-182(M26 범위
좁힘), ADR-183(syscall 번역 전략), ADR-188(locale 범위),
[procsrv.md](../spec/procsrv.md),
[kernel-memory.md](../design/kernel-memory.md) ADR-180(sys_brk),
ADR-189(동적 링킹 앞당김),
[kernel-scheduler.md](../design/kernel-scheduler.md) ADR-178
(sys_process_kill), ADR-184(LAPIC 타이머 보정), ADR-185(멀티코어
선점), ADR-186(signal 전달), ADR-187(pthread), ADR-191(타이머
추상화), [boot-and-drivers.md](../design/boot-and-drivers.md)
ADR-192(별도 데몬 폐기, procsrv 재부모화), ADR-193(준비완료 신호
통일), [security-model.md](../design/security-model.md) ADR-194
(위임 명령 범위), [build-system.md](../design/build-system.md)
ADR-190(SDK 내보내기), ADR-195(와이어 프로토콜 마크업+추출 도구)

**선행 완료 전제**: [general-purpose-completion.md](general-purpose-completion.md)
(M21~M26) 전부 완료돼 있어야 한다 — 특히 M23(sys_fork의 handle_table
복제)과 M24(sys_brk)가 이 계획의 최소 인프라를 이미 제공한다.

## 배경

M26(ADR-182)이 정직하게 진단한 그대로다 — musl의 문자열 함수만으로는
"실제로 컴파일되고 잘 동작하는 커널"이라는 목표에 한 걸음도 다가가지
못한다. 문자열 함수는 애초에 이 커널의 어떤 syscall/IPC 모델과도
마찰이 없는 부분집합이었기 때문이다. 진짜 목표(사용자가 명시한
"OPEN 항목들을 검토하고 해결할 계획을 세우고, musl을 포팅하여 실제로
컴파일이 가능하며 잘 동작하는 커널로 개선")를 이루려면 **OPEN-66이
지적한 진짜 syscall 계층**을 실제로 만들어야 한다.

### 왜 지금까지 안 됐는가 — 근본 문제 재확인

musl은 다른 libc(newlib 등)와 달리 "커스텀 커널을 위한 HAL"이 애초에
없다. `arch/x86_64/syscall_arch.h`가 `__syscall0`~`__syscall6`을 raw
`syscall` x86_64 명령(Linux syscall ABI, RAX=번호/RDI,RSI,RDX,R10,R8,R9=
인자/RAX=반환값, 음수=−errno)으로 직접 구현하고, `src/*.c` 전체가 이
매크로들을 통해서만 커널과 통신한다. 이 커널은 이런 syscall이 전혀
없다 — 열거된 14개 syscall(`kernel/include/uapi.hpp`)은 전부 IPC
프리미티브(`sys_ipc_call`/`sys_ipc_recv`/`sys_ipc_reply`)이거나
극소수의 프로세스/메모리 관리 syscall(`sys_fork`/`sys_exec`/`sys_brk`
등)뿐이고, open/read/write는 procsrv/VFS/FS 서버와의 IPC 왕복으로만
구현된다.

### 전략 결정 — ADR-183

두 가지 길이 있었다:

1. **커널에 Linux 호환 syscall ABI 진입점을 새로 추가**한다 (별도
   SYSCALL 벡터로 Linux syscall 번호를 그대로 받아 커널 내부에서
   해석) — 커널이 "다른 커널을 흉내 내는 코드"를 영구히 떠안는다.
2. **musl 자신의 syscall 진입점(`syscall_arch.h`)을 패치**해, 모든
   syscall 호출이 반드시 거치는 이 단일 지점에서 유저랜드 C 함수
   (`libc/sysdeps/minicore/syscall_shim.c`)로 우회시킨다 — 커널은
   전혀 모른다. 우회된 C 함수가 Linux syscall 번호를 보고 필요한
   최소 집합만 기존 `libmc`(VFS/FS/procsrv 클라이언트) 호출이나
   기존 14개 syscall로 번역한다.

ADR-183은 **2번**을 택한다 — ADR-006/007("커널은 최소한만 직접
구현, 나머지는 유저랜드")과 정확히 같은 논리다: "Linux syscall 번호를
minicore IPC로 번역하는 것"은 순전히 유저랜드 문제이고, 커널이 새로
알아야 할 것이 없다. `third_party/patches/musl/`(ADR-022가 예약해
뒀지만 M26까지는 한 번도 쓰이지 않은 자리)가 정확히 이런 용도다.
`tools/apply-patches.sh`(여전히 TODO 스텁)를 이 계획의 첫 마일스톤에서
드디어 실제로 구현한다.

**이 계획 전체를 관통하는 방향성(ADR-183 §결정4)**: musl을 포팅하는
과정에서 새 Linux syscall이 필요해질 때마다, 그 실제 동작은 항상
**`libmc`를 먼저 확장해서**(없는 함수를 추가) 채우고 `syscall_shim.c`
는 그 `libmc` 함수 하나를 호출하는 것으로 끝낸다 — `syscall_shim.c`
자신에 IPC/프로토콜 로직을 직접 쓰는 일은 없다. 즉 이 계획은 musl을
포팅하는 동시에 **`libmc`를 확장하며, 최종적으로 musl(정적
`libc.a`든 동적 `libc.so`든)은 그렇게 확장된 `libmc`와 링크해
빌드된다** — ADR-132(M12)가 원래 정한 "0단 syscall 1:1 래퍼+서버별
프로토콜 클라이언트는 전부 `libmc` 한 곳" 원칙을 musl 포팅의 매
단계에 예외 없이 적용하는 것이다. 각 마일스톤의 "구현" 절에 어떤
`libmc` 함수를 새로 추가하는지 구체적으로 적어 둔다.

**정적 링킹이 출발점이지만, M29부터는 동적 링킹이 기본 검증 경로가
된다** — ADR-189(아래 "전략 결정 — ADR-189" 참고)가 최초 초안(동적
링킹은 맨 뒤에 스트레치로만) 대비 방향을 바꿔, 계획 초반에 동적
링킹을 확보해 이후 모든 마일스톤이 그 위에서 검증되도록 앞당겼다.

### 전략 결정 — ADR-189: 동적 링킹을 계획 초반(M29)으로 앞당김

처음에는 동적 링커(`ld.so`)를 이 계획의 맨 마지막(스트레치, "사전
빌드된 `.so` 하나만 `dlopen`하는 최소 증명, 실패해도 무방")으로
남겨 뒀다. 사용자가 "동적 링킹을 계획에서 바로 활용할 수 있도록
미리 도입하라"고 방향을 바꿔, ADR-189를 다음과 같이 갱신했다:

- 동적 링킹 도입을 **M29(전체 세 번째 마일스톤)**로 앞당긴다.
- 대상은 "아무 `.so`나 하나"가 아니라 **musl 자신**이다 — musl의
  실제 배포 관례(`ld-musl-x86_64.so.1`이 동적 링커와 libc.so를
  겸하는 "멀티콜" 바이너리)를 그대로 채택한다. M28의 정적 테스트
  프로그램을 같은 소스로 다시 링크해 동적 실행파일로도 만들어
  검증한다.
- **M30(TLS/malloc)부터 M37(pthread)까지, 그리고 M39(셸/coreutils)의
  검증 프로그램은 기본적으로 동적 링크로 빌드**한다 — 여러 프로세스가
  같은 musl 코드를 공유하는 현실적인 형태로 나머지 계획 전체가
  검증된다. 정적 링킹은 폐기되지 않는다 — 계속 빌드 옵션으로
  지원하고, M29가 실제로 크게 막히면 이후 마일스톤은 정적 링킹으로
  되돌아가 계속 진행한다(ADR-049과 같은 판단 기준, 실패도 유효한
  결과로 기록).

## OPEN 항목 검토 — 이 계획과의 관계

| OPEN | 내용 | 이 계획과의 관계 |
|---|---|---|
| OPEN-54 | procsrv 실제 와이어 프로토콜(바이트 레이아웃) 미확정 | **M27이 해소한다** — 실제 프로세스 테이블을 만들려면 어차피 `proc_op` 오퍼레이션들의 정확한 `regs[]`/`pages[]` 배치를 확정해야 한다. **방법론은 ADR-195(build-system.md)로 먼저 확정됨** — 각 서버 헤더에 `@wire-op` 마크업을 달고 `tools/gen-wire-docs.py`로 추출·검증한 뒤에야 구현한다 |
| OPEN-64 | procsrv가 임의의 다른 프로세스를 pid로 wait/kill하는 프로토콜, `dup_for_new_client` 완전한 fd 진실 공급원 프로토콜 모두 미구현 | **M27이 절반만 해소한다** — 임의 pid에 대한 `OP_WAIT`/`OP_KILL`은 M27이 실제로 만든다. `dup_for_new_client`(신원이 바뀌는 exec/su·sudo 경로에서만 필요, fork는 신원 불변이라 M23의 handle_table 복제로 이미 충분하다, procsrv.md §3)는 **이 계획에서도 계속 범위 밖**으로 남긴다 — su/sudo 경유 coreutils 실행이 실제로 필요해지는 시점(M39 이후)에 재검토 |
| OPEN-65 | IPC 대기열에 갇힌 스레드는 kill이 즉시 폐기하지 못함 | **M36이 해소한다** — [ADR-186](../design/kernel-scheduler.md) §결정6("POSIX SIGKILL이 인터럽터블 슬립도 즉시 깨우는 것"과 같은 동작)이 `SIGKILL` 한정으로 대기열에서 즉시 `unlink`한다. `SIGKILL` 이외의 일반 시그널/`kill_requested`는 여전히 "다음 실행 시점"에만 전달된다(그 부분은 계속 열어 둔다) |
| OPEN-63 | AP 코어가 협조적 스케줄러에 참여하지 않음(진짜 멀티코어 선점형 스케줄러 없음) | **M34가 해소한다** — 코어별 `g_current`+독립 타이머+AP의 유저 run_queue 참여(ADR-185), **[ADR-191](../design/kernel-scheduler.md)의 `timer_source_interface`로 추상화**해 코어별 LAPIC을 정밀 제어한다. M37(pthread)의 진짜 병렬성이 이 위에서 성립한다 |
| OPEN-62 | LAPIC 타이머 보정 없음(마이크로초 정확도 없음) | **M33이 해소한다** — PIT/HPET 실측 기반 코어별 보정(ADR-184), M34보다 먼저 둔다. **원칙(ADR-184 §결정4): 모든 시스템 타이머는 이 시스템의 어떤 코어에서도 첫 유저모드 진입이 일어나기 전에 보정이 끝나 있어야 한다** |
| OPEN-32 | result/optional의 `[[nodiscard]]` 강제 여부 | 무관, 이 계획에서 건드리지 않음 |
| OPEN-42, 51, 52 | (각각 위임 세부범위, 서비스 관리자 데몬, 준비완료 신호) | **이 계획과는 무관하지만 이 라운드에서 별도로 해소됐다** — OPEN-42(명령 단위 범위)는 [ADR-194](../design/security-model.md), OPEN-52(준비완료 신호를 기존 spawn 시점 전용 endpoint의 Call/Reply로 통일)는 [ADR-193](../design/boot-and-drivers.md) 참고. OPEN-51(유저 서비스 관리자 데몬의 정체성·책임 범위, "커널 서버"와 역할 분리)은 [ADR-192](../design/boot-and-drivers.md) — 이 데몬 자체는 여전히 이 계획의 범위 밖이라 M27은 재부모화 **메커니즘**만 만들고 대상은 잠정적으로 `parent_pid=0`으로 둔다(위 M27 참고) |
| OPEN-60 | cfgsrv 레지스트리 기반 I/O 활성화 권한의 동적 부여/회수(ADR-154) | 무관, 이 계획에서 건드리지 않음 — 여전히 미해결(2026-09-10 사용자가 확인: OPEN-60은 진짜로 cfgsrv IO 권한 얘기이며, `docs/reply.md`에 같은 번호로 태그됐던 fork 변형 답변은 이 항목과 무관한 오기였다 — 아래 M32/M37에 반영한 fork/clone 범위 좁힘은 OPEN-60과 별개로 유효한 확인사항으로 남긴다) |
| **OPEN-66** | 진짜 syscall 계층/동적 링커/pthread/locale/stdio 전부 없음 | **이 계획의 본체.** M28·M30~M32가 진짜 syscall 계층을, M29가 동적 링킹을, M35가 locale을, M36이 signal을, M37이 pthread를 다룬다 — 이 계획은 OPEN-66이 나열한 **전부**를 한 번씩은 다룬다. 다만 각 항목의 범위가 의도적으로 좁다(futex 기초 연산만, 표준 시그널만, 단일 공유 libc만 등) — 완료 시점에 open-items.md를 "핵심은 해소, 세부 확장은 남음"으로 갱신하고, 남는 세부(다중 `.so` 일반화, 실시간 시그널, job control 등)를 새 OPEN 번호로 분리한다 |

## M27. procsrv 실제 프로세스 테이블 + 범용 OP_WAIT/OP_KILL

- **구현**:
  0. **[ADR-195](../design/build-system.md)의 마크업 컨벤션+
     `tools/gen-wire-docs.py`를 이 마일스톤의 첫 단계로 먼저
     만든다** — procsrv 프로토콜 헤더 자체가 이 도구로 처음
     검증받는 사례가 된다("헤더 먼저 작성→추출 도구로 확인→그제서야
     구현"이라는 ADR-195 §결정4의 순서를 그대로 따른다).
  1. [procsrv.md](../spec/procsrv.md) §2/§6이 정의한 `process_entry`
     테이블(pid 발급, parent/children 트리, `process_state`)을 실제로
     만든다. M22(ADR-178)는 procsrv가 자기 자신을 대상으로 한
     자기테스트로만 wait/kill 메커니즘을 증명했다 — 이번엔 **어느
     프로세스든 자신이 낳은 자식의 pid로 `OP_WAIT`를 부를 수 있고,
     자신이 kill 권한이 있는 대상 pid로 `OP_KILL`을 부를 수 있는**
     실제 프로토콜을 만든다. `proc_op::wait`/`signal`의 정확한
     `regs[]`/`pages[]` 배치를 이 마일스톤에서 확정한다(OPEN-54 해소).
  2. 이 프로토콜의 **클라이언트 쪽 구현(호출 조립)은 procsrv 자신의
     코드에 두지 않고 `libmc`(procsrv 프로토콜 클라이언트 자리,
     ADR-132)에 둔다** — M32가 `SYS_wait4`/`SYS_getpid`를 옮길 때
     이 `libmc` 함수를 그대로 재사용한다.
  3. **[ADR-192](../design/boot-and-drivers.md) §결정3의 재부모화
     메커니즘(대상은 매개변수)** — initrun이 사라질 때 그 자식들의
     `parent_pid`를 새 대상으로 갈아치우는 일반 메커니즘을 만든다.
     ADR-192의 최종 재부모화 대상은 "유저 서비스 관리자 데몬"이지만,
     그 데몬 자체는 이 계획의 범위 밖이라 **아직 존재하지 않는다** —
     그래서 이 마일스톤은 데몬이 생기기 전 잠정 동작으로 고아들을
     `parent_pid = 0`(부모 없음)으로 둔다. 데몬이 실제로 생기는
     시점에 이 잠정 처리를 교체하기만 하면 되도록, 재부모화 대상을
     하드코딩하지 않고 매개변수로 받는 형태로 구현한다.
  4. **[ADR-193](../design/boot-and-drivers.md)의 준비완료 신호** —
     procsrv를 포함해 핵심 서비스 spawn 전부가 spawn 시점 전용
     endpoint에서 `k_service_ready_label`로 첫 Call을 받는(그리고
     `mc_wait_ready()`로 그것을 기다리는) 관례를 실제로 적용한다 —
     M27이 이 관례를 처음 실전에 쓰는 자리가 된다(procsrv.md의
     wait 프로토콜과 정확히 같은 모양이므로 구현 비용이 낮다).
- **목표**: 서로 다른 두 유저 프로세스 A(부모)·B(자식)를 procsrv가
  spawn한다. B가 종료되면 A가 `OP_WAIT(B의 pid)`로 exit code를
  회수해 일치를 확인한다. 별도 프로세스 C에 대해 A가 `OP_KILL(C의
  pid)`을 보내 C가 강제 종료됨을 확인한다 — M22와 달리 procsrv
  자신이 아니라 **평범한 유저 프로세스끼리**의 왕복이다. 별도로,
  initrun 종료 후 남은 서비스의 `parent_pid`가 (아직 유저 서비스
  관리자 데몬이 없으므로) `0`으로 바뀌었음을 `/sys/proc`(또는
  디버그 로그)로 확인한다.

## M28. musl syscall 번역 계층 착수 — 최초의 실제 musl 프로그램 (정적)

- **구현**: ADR-183의 전략을 실제로 구현한다.
  1. `tools/apply-patches.sh`를 실제로 구현(더 이상 TODO 스텁이 아님) —
     `third_party/patches/<project>/*.patch`를 `third_party/musl`
     체크아웃 위에 순서대로 적용해 `build/_patched/musl`류 작업
     트리를 만든다.
  2. `third_party/patches/musl/0001-syscall-shim.patch` — musl의
     `arch/x86_64/syscall_arch.h`를 재작성해 `__syscall0`~`__syscall6`이
     `long __minicore_syscall_dispatch(long n, long a, long b, long c, long d, long e, long f)`
     (새 `libc/sysdeps/minicore/syscall_shim.c`, `extern "C"`)를
     호출하게 만든다. 취소 지점(`__syscall_cp_c` 등, pthread
     취소용)은 이 마일스톤 시점엔 단일 스레드만 다루므로(pthread는
     M37) 평범한 syscall과 동일하게 처리한다(취소 없음) — M37
     착수 시점에 실제 취소 지점으로 다시 확장한다.
  3. `syscall_shim.c`가 처음 구현하는 것: `SYS_write`(fd==1/2만),
     `SYS_exit`, `SYS_exit_group`. 그 외 번호는 전부 `-ENOSYS`.
     **`syscall_shim.c` 자신은 번호별 `switch`+`libmc` 호출 한 줄만
     담는다** — `SYS_write`가 실제로 쓸 콘솔/tty 출력 경로가 `libmc`
     에 아직 없으면(M17이 만든 콘솔 클라이언트를 그대로 재사용할 수
     있는지 먼저 확인) `libmc/src/ipc/console.c`(또는 해당 자리)에
     `mc_console_write()` 같은 함수를 **먼저 추가**한 뒤
     `syscall_shim.c`가 그것만 호출한다(ADR-183 §결정4 — 새 IPC/
     프로토콜 로직을 `syscall_shim.c`에 직접 두지 않는다).
  4. `libc/CMakeLists.txt`가 musl의 **실제 시작 경로**
     (`crt/crt1.c`, `src/env/__libc_start_main.c`, `__init_tls` 등)를
     빌드에 포함하도록 확장한다 — M26은 이 경로를 전혀 쓰지 않았다
     (셸이 직접 문자열 함수만 호출).
- **목표**: musl로 **정적** 링크된 새 테스트 프로그램
  (`userland/musl-hello/` 같은 최소 새 타깃, 기존 셸과 무관)이 musl의
  **진짜 시작 경로**를 거쳐 `main()`에 진입하고,
  `write(1, "hello from real musl\n", ...)`+`_exit(0)`을 호출해
  콘솔에 그 문자열이 실제로 찍히는 것을 QEMU로 확인한다.

## M29. 동적 링킹 도입 — `PT_INTERP`/auxv + musl 자신의 공유 `libc.so`

- **구현**: [ADR-189](../design/kernel-memory.md)의 전략을 실제로
  구현한다.
  1. 커널의 ELF 로더(`process_ops.cpp::load_elf`류, ADR-140~142)가
     `PT_INTERP`를 인식하면, 지정된 인터프리터(`/lib/ld-musl-x86_64.so.1`)
     를 VFS에서 읽어 별도 베이스 주소에 매핑하고, 원래 프로그램은
     매핑만 해 둔 채 인터프리터의 진입점으로 점프한다.
  2. 유저 스택에 최소 auxv(`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_ENTRY`/
     `AT_BASE`/`AT_NULL`)를 구성한다 — 지금까지 전혀 없었다.
  3. `libc/CMakeLists.txt`에 musl의 `ldso/dynlink.c`를 포함하는 **동적
     libc.so 빌드 타깃**을 새로 추가한다(기존 정적 `minicore_libc`
     타깃은 그대로 유지 — 어느 쪽도 폐기하지 않는다).
  4. M28의 테스트 프로그램을 **같은 소스로 다시 링크**해 동적
     실행파일로도 만든다.
- **주의(사전 진단)**: 이 마일스톤이 ELF 로더/auxv 확장에서 실제로
  크게 막히면(ADR-049과 같은 판단 기준), **정적 링킹으로 되돌아가
  M30부터 계속 진행한다** — 이 경우 M39("여러 바이너리가 libc를
  공유")는 달성되지 않지만 나머지 계획(syscall 계층/시그널/pthread
  등)은 정적 링킹으로도 전부 독립적으로 유효하다. 어느 경로를 탔는지
  `docs/done/`에 정직하게 기록한다.
- **목표**: M28의 테스트 프로그램을 동적 링크로 다시 빌드해 QEMU로
  실행하고, `/lib/ld-musl-x86_64.so.1`이 실제로 매핑·실행돼 같은
  "hello from real musl" 출력이 나오는 것을 확인한다 — 이후 M30부터
  이 동적 버전이 기본 검증 경로가 된다.

## M30. TLS(`arch_prctl`) + 실제 musl 힙 할당자 (동적 링크 기본)

- **구현**: `SYS_arch_prctl`(`ARCH_SET_FS`)을 구현해 musl의 TLS(스레드별
  errno 등)가 동작하게 한다 — **스레드 단위**로 설계한다(M37 pthread가
  이 전제에 의존, ADR-187 §결정 2). `SYS_mmap`(익명,
  `MAP_PRIVATE|MAP_ANONYMOUS`만)과 `SYS_munmap`을 구현해 musl 자신의
  실제 할당자(mallocng)가 동작하게 한다 — M24의 `sys_brk` 고정 1MiB
  슬롯(ADR-160 슬롯 5)으로 충분한지, 별도의 새 슬롯/새 커널 syscall이
  필요한지는 착수 시점에 확정한다. **두 syscall 모두 새 `libmc` 함수
  (`mc_arch_prctl`/`mc_mmap`/`mc_munmap`)로 먼저 감싼 뒤
  `syscall_shim.c`가 그 함수만 호출한다**(ADR-183 §결정4) — 새
  커널 syscall이 필요해지면 그것도 `libmc`의 1:1 래퍼 목록에
  추가한다(ADR-132 §결정1의 "0단"). M26의 `mem_shim.c`
  (`malloc`/`free`→`mc_malloc`/`mc_free`)는 이 마일스톤부터 더 이상
  쓰이지 않는다 — musl 자신의 malloc으로 완전히 대체한다.
- **목표**: M29의 **동적 링크된** 테스트 프로그램이 `errno`를 실제로
  읽고 쓰는 코드 경로와, 진짜 musl `malloc()`으로 버퍼를 할당·채움·
  `free()`하는 왕복을 QEMU로 확인한다.

## M31. 파일 I/O syscall — VFS/FS IPC 연결 + 실제 musl stdio

- **구현**: `SYS_open`/`SYS_openat`/`SYS_read`/`SYS_close`/`SYS_lseek`/
  `SYS_fstat`/`SYS_writev`/`SYS_ioctl`(스텁, `isatty` 판별용)을
  구현한다 — `syscall_shim.c`가 **이미 존재하는 `libmc`의 VFS/FS
  프로토콜 클라이언트 함수를 그대로 호출**한다(M13~M20이 이미 만들어
  둔 것, 셸이 쓰던 것과 동일 — 여기서는 새로 만들 필요 없이 재사용이
  대부분이다). POSIX `open()` flags/mode를 기존 fs-protocol v4
  오퍼레이션에 맞게 번역하는 코드만 `syscall_shim.c`에 남고, 그
  변환된 값으로 실제 IPC를 하는 코드는 여전히 `libmc` 쪽이다(ADR-183
  §결정4) — 기존 `libmc` 함수의 시그니처가 이 변환 결과를 못 받으면
  그 함수를 확장한다.
- **목표**: 테스트 프로그램(동적 링크)이 musl의 **진짜 stdio**
  (`fopen`/`fread`/`fclose`, 내부 버퍼링 포함)로 VFS의 실제 파일을
  열어 읽고, `printf`로 그 내용을 stdout에 찍어 기대값과 일치함을
  QEMU로 확인한다 — 재구현이 아니라 musl 소스 자체가 만든 stdio다.

## M32. 프로세스 syscall — fork/execve/wait4 실왕복

- **구현**: `SYS_fork`(또는 musl이 실제로 쓰는 `SYS_clone` 경로 —
  단일 스레드 fork만 다루므로 `flags==SIGCHLD`뿐인 단순 케이스만
  지원, 그 외 `-ENOSYS`)를 기존 `sys_fork`(ADR-179, 이미 handle_table
  전체 복제)로, `SYS_execve`를 기존 `sys_exec`으로, `SYS_wait4`를
  M27이 만든 범용 `OP_WAIT`로, `SYS_getpid`를 procsrv 조회로 번역한다.
  이 네 커널/IPC 호출 각각에 대응하는 `mc_fork`/`mc_exec`/`mc_wait`/
  `mc_getpid` 같은 `libmc` 함수가 아직 없으면 먼저 추가하고
  `syscall_shim.c`는 그것만 부른다(ADR-183 §결정4) — M27이 새로
  만드는 `OP_WAIT`/`OP_KILL` 와이어 프로토콜의 클라이언트 쪽 구현도
  `libmc`(procsrv 클라이언트 자리)에 두는 것이 맞다. **musl의
  `SYS_clone`이 지원하는 모든 flags 조합을 흉내 낼 필요는 없다 —
  fork의 특수 변형(예: `vfork`, `CLONE_VM` 없는 다른 조합)은
  minicore 전용 변형(위 "단순 케이스만 지원, 그 외 `-ENOSYS`")으로
  한정해도 된다고 사용자가 명시적으로 확인했다**(2026-09-10 11:04
  답변 — 당시 OPEN-60으로 태그돼 있었으나, 2026-09-10에 사용자가
  OPEN-60은 별개로 cfgsrv IO 권한 얘기가 맞다고 확인해 태그 오기로
  판명됐다. 이 확인 자체는 fork/clone 범위 좁힘에 대한 것으로
  유효하게 반영한다 — 위 "OPEN 항목 검토" 표의 OPEN-60 행 참고).
- **목표**: 테스트 프로그램이 musl의 **진짜 `fork()`+`execve()`+
  `wait()`**(라이브러리 함수, syscall 매크로가 아니라)로 자식을
  만들어 다른 실행 이미지(동적 링크된 별도 바이너리)를 실행시키고,
  부모가 그 종료 코드를 회수함을 QEMU로 확인한다 — `execve()`가
  자식 안에서 다시 `PT_INTERP` 로더 경로를 타는 것도 함께 확인한다
  (M29가 최초 프로세스 하나만이 아니라 fork+exec 경로에서도 실제로
  버티는지 검증하는 기회).

## M33. LAPIC 타이머 보정 — PIT/HPET 실측 기반 (OPEN-62 해소)

- **구현**: [ADR-184](../design/kernel-scheduler.md)의 전략을 실제로
  구현한다 — 부팅 초기 ACPI 파싱(M10, MADT용)을 HPET 테이블도
  찾도록 확장하고, `kernel/arch/x86_64/lapic.cpp`에
  `calibrate_lapic_timer()`를 추가한다. HPET가 있으면 그것을, 없으면
  PIT(8254) 채널2 폴링을 기준시계로 삼아 각 코어(BSP+각 AP, AP
  기동 시퀀스 끝에서 스스로 수행)가 자신의 LAPIC 타이머 실측
  주파수를 구하고, `base_time_slice_us`에 정확히 대응하는
  `initial_count`를 다시 써 넣는다.
- **목표**: 코어별로 실측한 `initial_count`가 (터보/클록 차이가 있는
  QEMU vCPU 설정에서도) 목표 마이크로초에 근접함을 QEMU 로그로
  확인한다 — 보정 전/후 값을 함께 로그로 남겨 대조한다. 기존
  smoke/SMP/NUMA/AVX 4개 스위트 전부 회귀 없음을 재확인한다(타이머
  경로를 건드리므로 특히 꼼꼼히).

## M34. 진짜 멀티코어 선점형 스케줄러 — 코어별 `g_current`+AP의 유저 run_queue 참여 (OPEN-63 해소)

- **구현**: [ADR-185](../design/kernel-scheduler.md)의 전략을 실제로
  구현한다 — `g_current`를 코어별로 분리하고, AP 코어가 유저 밴드
  run_queue에서 직접 스레드를 뽑아 실행하게 하며, 각 코어가 자신의
  로컬 LAPIC 타이머(M33의 보정값)로 독립적으로 선점한다. 워크
  스틸링(ADR-053/137)의 기존 락 순서 표(ADR-136)를 이 변경에 맞춰
  갱신한다.
- **목표**: `init/preempt_demo/`(M21이 만든 busy/counter 데모)를
  코어 수만큼 늘려 `MINICORE_QEMU_SMP=4`(예시)로 띄우고, **BSP뿐
  아니라 AP에서도** 유저 스레드가 실제로 도는 것을 QEMU 로그로
  확인한다(코어별 로그 태그로 "어느 코어가 어느 스레드를 실행
  중인지" 구분). smoke/SMP/NUMA/AVX 4개 스위트 전부 회귀 없음을
  특히 꼼꼼히 재확인한다.

## M35. musl locale — "C" 고정 검증 (범위 좁힘)

- **구현**: [ADR-188](../design/foundations.md)이 정한 범위 그대로 —
  별도 커널/syscall 작업 없이, musl이 기본 제공하는 C 로케일
  경로가 이 커널 위에서도 그대로 동작하는지 확인만 한다.
- **목표**: 테스트 프로그램에 `setlocale(LC_ALL, "")`와
  `setlocale(LC_ALL, "ko_KR.UTF-8")`(실패해야 함, `NULL` 반환 확인)를
  둘 다 추가해 QEMU로 확인한다.

## M36. 완전한 signal 계층 — `pending_signals`+return-to-user 트램폴린

- **구현**: [ADR-186](../design/kernel-scheduler.md)의 전략을 실제로
  구현한다 — `object::thread`에 `pending_signals`/`signal_mask` 추가,
  `k_right_can_kill`을 `k_right_can_signal`로 일반화, 새 커널 syscall
  `sys_signal_send`/`sys_rt_sigreturn`, SYSRET/IRETQ 직전 확인 경로
  추가. **새 커널 syscall 둘은 각각 `libmc`의 `mc_signal_send`/
  `mc_sigreturn`으로 먼저 감싼다**(ADR-183 §결정4) —
  `syscall_shim.c`는 그 함수만 부른다. `sys_rt_sigaction`(핸들러
  등록)은 커널을 부르지 않는 **순수 유저공간 상태**(등록된 핸들러
  주소/마스크 테이블)라 예외적으로 `syscall_shim.c` 자신이 그 표를
  들고 있어도 된다 — IPC/커널 syscall이 아니므로 ADR-183 §결정4의
  대상이 아니다. procsrv가 M27의 `process_entry` 상태 전이 시점에
  `SIGCHLD`를 실제로 전달한다(이 경로도 procsrv→대상 프로세스
  전달이므로 `mc_signal_send`를 그대로 재사용).
- **목표**: 테스트 프로그램이 `sigaction(SIGUSR1, ...)`으로 핸들러를
  등록하고, 다른 프로세스가 `kill(pid, SIGUSR1)`을 보내면 그
  핸들러가 실제로 호출돼 전역 카운터를 증가시킴을 확인한다. 별도로,
  부모가 `wait4` 없이도(비차단) 자식 종료 시 `SIGCHLD` 핸들러가
  호출됨을 확인한다. `SIGKILL`은 핸들러 등록을 시도해도 여전히
  즉시 강제 종료됨을 확인한다(ADR-178 경로 유지 확인).

## M37. pthread 최소 구현 — `sys_thread_create`+`sys_futex`

- **구현**: [ADR-187](../design/kernel-scheduler.md)의 전략을 실제로
  구현한다 — 새 syscall `sys_thread_create`(기존
  `create_user_thread`를 새 address_space 없이 재사용)+`sys_futex`
  (`FUTEX_WAIT`/`FUTEX_WAKE`만). **각각 `libmc`의 `mc_thread_create`/
  `mc_futex_wait`/`mc_futex_wake`로 먼저 감싸고**(ADR-183 §결정4),
  `syscall_shim.c`가 musl의 `SYS_clone(CLONE_VM|...)`/`SYS_futex`를
  이 `libmc` 함수로 번역한다. M30의 `sys_arch_prctl`(및 그
  `mc_arch_prctl` 래퍼)을 스레드 단위로 확장(이미 M30 착수 시점에
  이렇게 설계돼 있어야 한다, ADR-187 §결정 2). `kernel-memory.md`
  ADR-180이 미리 지적해 둔 `sys_brk`의 동시성 보호(spinlock)를 이
  마일스톤의 선행 작업으로 추가한다.
- **목표**: 테스트 프로그램이 musl의 진짜 `pthread_create()`로 새
  스레드 둘을 만들어(M34 덕분에 서로 다른 코어에서 동시에 실행될
  수 있음을 코어별 로그 태그로 확인), `pthread_mutex_t`(futex 기반)
  로 보호된 공유 카운터를 병렬로 증가시킨 뒤 `pthread_join()`으로
  합류해 최종 값이 기대한 그대로임을 확인한다(경쟁 조건 없이).

## M38. minicore 타깃 크로스 툴체인/SDK 내보내기

- **구현**: [ADR-190](../design/build-system.md)의 전략을 실제로
  구현한다.
  1. `tools/export-sdk.py` — 빌드된 minicore 트리에서 musl의 패치된
     공개 헤더, `libc.a`(정적)와 `libc.so`/`ld-musl-x86_64.so.1`(동적,
     M29~), `libmc.a`+헤더, musl의 crt 객체, 새 공용 `link.ld`를
     뽑아 `<out>/sdk/x86_64-minicore/`에 모은다.
  2. 컴파일러 래퍼 `x86_64-minicore-clang`(이미 설치된 크로스
     clang에 `--target=x86_64-linux-musl --sysroot=<sdk>`를 자동으로
     얹음)와 최소 CMake 툴체인 파일(`sdk/x86_64-minicore.cmake`)을
     만든다.
- **목표**: minicore 저장소 **바깥의** 새 디렉터리에 musl `printf`+
  `malloc`만 쓰는 "hello world"를 하나 작성하고, minicore 소스를
  전혀 참조하지 않은 채 이 SDK만으로 컴파일한다. 결과 ELF를 기존
  `tools/mkbootdisk.py`(필요시 최소 확장)로 부트 디스크에 넣어
  QEMU로 실행해 출력이 맞는지 확인한다 — "minicore 소스 트리 없이도
  minicore용 프로그램을 만들 수 있는가"가 핵심 검증이다.

## M39. 실제 서드파티 셸/coreutils 재포팅 시도 (스트레치)

- **구현**: M27~M37이 실전에서 버틴다면, M20/M26이 두 번 미룬 원래
  목표("실제 포팅된 셸/coreutils로 M20의 완료 기준 재달성")를 이번엔
  진짜로 시도한다 — 가벼운 셸(`mksh`/`dash`류)과 최소 coreutils
  (`ls`/`cat`/`echo` 등)를 `third_party/`에 새 submodule로 추가해
  musl+syscall_shim 위에 **동적 링크**한다(M38의 SDK를 그대로 써도
  되고, 기존 in-tree vendoring 패턴(ADR-022)을 써도 된다 — 둘 다
  결과 ELF는 같아야 한다). 이 마일스톤에서 처음으로 "여러 바이너리가
  같은 `libc.so`를 공유"하는 M29의 원래 목표가 실현된다.
- **주의(사전 진단)**: M17~M26이 반복해 온 패턴대로, 착수 시점에
  실제로 필요한 기능이 이 계획이 다루지 않은 것들(job control —
  프로세스 그룹/`tcsetpgrp`, 파이프/`dup2`를 이용한 리다이렉션,
  `waitpid` 상태 코드 세부)로 드러날 가능성이 높다 — 그 시점에 다시
  범위를 좁히고 새 ADR로 기록한다(ADR-170/182와 같은 정직한 패턴).
  **이 마일스톤이 실패해도(부분 구현으로 끝나도) M27~M38의 성과는
  독립적으로 유효하다** — 그래서 스트레치로 맨 뒤에 뒀다(M36의
  signal이 이미 갖춰져 있어야 셸의 Ctrl-C/`SIGINT` 처리가 최소한
  성립하므로, 순서상으로도 여기가 맞다).
- **목표**: 로그인 후 셸이 뜨고, 그 셸이 minicore 네이티브 구현
  (ADR-170)이 아니라 **포팅된 서드파티 바이너리**이며, `ls`/`cat`
  같은 명령이 빌트인이 아니라 **별도 실행파일**로 fork+exec됨을
  QEMU로 확인한다.

## 포함하지 않는 것 (이 계획 이후로 명시적으로 미룸)

- **동적 링킹의 일반화** — M29~M39는 musl 자신의 공유 `libc.so` 하나만
  다룬다. PLT lazy binding, 심볼 버전, 서드파티 `.so` 여러 개가 서로
  의존하는 일반적인 경우는 이 계획 이후로 미룬다.
- **실시간 시그널(`SIGRTMIN`~`SIGRTMAX`, `sigqueue` payload)과 job
  control**(프로세스 그룹, `setpgid`/`tcsetpgrp`) — M36은 표준
  시그널 1~32와 `SIGCHLD`/사용자 핸들러까지만 다룬다.
- **`FUTEX_CMP_REQUEUE` 등 고급 futex 연산, 실시간 스케줄링 정책
  (`SCHED_FIFO`/`SCHED_RR`)** — M37의 futex는 `WAIT`/`WAKE`만.
- **`dup_for_new_client` 완전한 fd 진실 공급원 프로토콜**(OPEN-64
  나머지) — 위 "OPEN 항목 검토" 표 참고, su/sudo 경유 실행이 실제로
  필요해지는 시점까지 미룬다.
- **`SIGKILL` 이외 시그널의 즉시 대기열 unlink** — OPEN-65는 `SIGKILL`
  한정으로 M36이 해소한다(위 "OPEN 항목 검토" 표 참고). 일반
  시그널이 대기 중인 스레드에 "다음 실행 시점"보다 더 빠르게
  전달돼야 하는 경우는 여전히 범위 밖이다.
- **위임의 시간대 단위 제한, 마운트 계획의 boot-파라미터화, 유저
  서비스 관리자 데몬 자체(구현체) 및 유저 서비스 등록 프로토콜** —
  ADR-194/192가 방향은 정했지만 실제 코드 변경은 이 musl 계획의
  어떤 마일스톤에도 넣지 않는다(각각 별도 라운드 대상, 위 "OPEN
  항목 검토" 표 참고). 단 M27이 procsrv 프로세스 테이블을 실제로
  만드는 시점이므로, ADR-192 §결정3의 **재부모화 메커니즘**(대상은
  매개변수, 데몬이 없는 지금은 잠정적으로 `parent_pid=0`)만큼은
  **M27 착수 시점에 함께 구현하는 것이 자연스럽다** — M27의
  "구현" 절에 반영했다.
- **SDK의 버전 고정/배포 정책**(M38) — "매번 새로 export한다"는
  전제로만 다루고, 실제 외부 사용자가 생기는 시점의 후속 결정으로
  남긴다.
- aarch64 이식 — 여전히 이 계획 이후로 미뤄 둔 별도 방향.

## 검증 방법

기존 계획들과 같은 방식 — QEMU 부팅 로그로 확인 가능한 마일스톤별
완료 기준을 두고, `tools/smoke-test-x86_64.sh`에 확인 문자열을
마일스톤마다 추가한다. M28부터는 기존 셸/서버와 무관한 **새
테스트 프로그램**(`userland/musl-hello/` 또는 유사한 이름, 착수
시점에 확정)을 하나 두고 M28~M32, M35~M37에 걸쳐 그 프로그램만
점진적으로 키워 간다 — 매 마일스톤이 "지금까지 쌓은 것 위에 딱 한
단계"만 추가하는 이 프로젝트의 기존 패턴을 그대로 따른다.
`syscall_shim.c`가 아직 구현하지 않은 syscall 번호를 만나면 반드시
-ENOSYS를 반환하고 그 사실을 debug_log로 남긴다 — 조용히 무시하거나
잘못된 값을 돌려주지 않는다(추후 디버깅을 위해). M33/M34(스케줄러
변경)은 musl 테스트 프로그램과 무관하게 기존 `init/preempt_demo/`
(M21)를 확장해 검증한다 — 스케줄러 변경이 musl 경로보다 먼저
회귀를 일으키기 쉬우므로 독립적으로 확인한다.

## 완료 후

각 마일스톤(또는 몇 개씩 묶어) 완료 시 `docs/done/`에 결과를
기록한다. 이 계획 문서 자체는 실행 후에도 수정하지 않고 "실행 전
계획" 그대로 보존한다(기존 계획들과 동일한 문서 체계 원칙).
