# DC-54D69BEE 방향(B) 적용 후에도 dbgdriver는 9/60(15%) 재현 - 캐스케이딩 gLock 데드락 하드닝 방향 결정 요청

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-2CB9DDA0
  status: review
  updatedAt: 2026-09-27T11:16:56.673Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## [gdb 확인, 2026-09-27, QU-2CEB2339 답변("gdb로 재확인해 계속 추적")] 잔여 1/60의 정체 확정 - gLock이 아니라 serial.cpp의 gWriteLock, 완전히 다른 메커니즘

free-running gdb 사냥(GRUB SMP4+실제initrd, `-s` 무`-S`, 워치독 로그가
뜨는 순간 attach) 31회차에서 재현 성공. `info threads`/`thread apply
all bt` 결과가 **9/60·기존 gLock 캐스케이딩 케이스와 완전히 다른
그림**을 보였다:

- CPU#3(워치독을 유발한 코어) - `kHandleNmi`/`kPanic`에서 halted(예상됨).
- CPU#0 - `kernel::kDevmgrKernelMain()`을 정상적으로 실행 중(정지 아님).
- CPU#1/CPU#2 - **둘 다 `running`**, `Scheduler::runLoop() →
  kSyncRsp0ForDispatch → currentCoreIndex() → kScanCoreIndexByApicId()
  → Lapic::readRegister()` 정상 idle 경로 실행 중(정지 아님).

**즉 나머지 3코어 중 어느 하나도 락을 쥔 채 멈춰 있지 않다** - 이번
재현은 캐스케이딩 데드락이 아니다. CPU#3 자신의 인터럽트 프레임
(`rip=0xffffffff80110086`, 시리얼 로그 덤프와 gdb의
`kHandleNmi(frame=...)` 둘 다 일치)이 가리키는 지점을 그 코어의
`rdi`(Spinlock::lock()이 `this`를 그대로 보존하는 유일한 레지스터,
루프 안에서 다른 호출이 없어 안전하게 신뢰 가능)로 역추적한 결과 -
`rdi=0xffffffff808ddb10` → `nm -n`으로 이 정확한 빌드에서 해석하면
**`(anonymous namespace)::gWriteLock`(offset=0x0, 완전 일치)** -
`minicore/kernel/serial.cpp:19`, `Serial::write()`가 문자열 전체를
감싸는 바로 그 락이다.

**메커니즘 재구성**: `Serial::write()`는 `gWriteLock`을 잡은 채
`putChar()`로 **문자 하나하나마다 UART LSR THRE 비트를 폴링하는
바쁜 대기**를 한다(`serial.cpp:36`) - 38400 baud 기준 문자당 약
260µs, 로그 한 줄(수십~수백 자)이면 수 ms~수십 ms 동안 이 락을
계속 쥐고 있을 수 있다. `Logger::info/warn/error`가 이 커널
전역에서 매우 광범위하게 호출되므로(부팅 로그, 각종 진단, syscall
경로 곳곳), `gLock`보다 오히려 **경합 빈도 자체는 훨씬 높은 락**
이다 - 다만 임계구역이 "짧고 우연히 재수 없이 선점당하는" 문제가
아니라 **그 자체로 원래 느린(수 ms~수십 ms) 하드웨어 I/O**라는 점이
`gLock`과 근본적으로 다르다.

**독립 교차 확인**: 같은 날 다른 작업(`async_task.cpp:992` 주석,
`kAsyncReactorsPerCore>1` 검증 중 발견)이 이미 **"코어 하나가
Logger의 Spinlock::lock() 안에서 응답 없음"** 이라는 정확히 같은
증상을 낮은/불안정한 빈도로 목격했고 `DC-D8951156`/`PN-6360E6E9`/
`PN-D44504D1` 계열로 잠정 분류해 둔 바 있다 - 이번 gdb 확인이 바로
그 증상의 정체를 처음으로 정확히 규명한 것이다.

**왜 방향(A)(IrqSpinlock)를 gWriteLock에 그대로 적용하면 안 되는가**:
`gLock`의 경우 IrqSpinlock 전환이 정답이었다(임계구역이 원래 짧고,
cli로 감싸도 부작용이 없음) - 그러나 `gWriteLock`은 **임계구역
자체가 수 ms~수십 ms짜리 실제 느린 하드웨어 폴링**이라, 이를 그대로
`IrqSpinlock`으로 감싸면 그 구간 내내 로컬 코어의 인터럽트(타이머
틱 포함)를 전부 막아버려 - 오히려 **매번 확정적으로** 워치독을
유발하는 방향으로 악화될 가능성이 높다. 이 락은 `gLock`과 다른 종류의
해법이 필요하다.

**결정이 필요한 지점(신규)**: 방향을 여쭙는다 -
1. **(E) Serial 출력을 논블로킹/버퍼링 방식으로 재설계** - 문자별
   busy-wait 대신 링버퍼+인터럽트 구동 TX(IRQ 활성화는 이미 `Serial::
   init()`이 해 둠, 현재 미사용)로 전환해 `gWriteLock` 보유 시간을
   "버퍼에 복사"만큼으로 극적으로 줄인다. 가장 근본적이지만 UART
   드라이버 재설계라 범위가 크다.
2. **(F) 워치독이 "느린 I/O로 바쁨"과 "진짜 멈춤"을 구분하게 함** -
   `DC-2CB9DDA0` 최상단(방향 B 후보)이 이미 제시했던 아이디어를
   여기 재적용 - 코어별 "현재 보유 중인 lock/느린 I/O 표시" 플래그.
3. **(G) 그냥 둔다** - 캐스케이딩과 달리 이번엔 다른 코어가 전부
   정상 진행 중이었다(전체 정지 아님, 코어 1개만 영구 손실) - 심각도가
   `gLock` 케이스보다 낮다고 보고, 발생률(1.7%대)을 감내 가능한
   잔존 리스크로 남길지.
4. **(H) 먼저 이 메커니즘을 더 실측** - 어느 Logger:: 호출이 실제로
   문제의 그 순간 락을 쥐고 있었는지(다른 코어 쪽) 추가 재현으로
   더 좁힌 뒤 결정.

## 참고 (추가)
- `minicore/kernel/serial.cpp:19,41-49` - `gWriteLock`/`Serial::write()`
  /`Serial::putChar()` 정의.
- `minicore/kernel/async_task.cpp:992` - 같은 증상의 독립적 이전
  목격(2026-09-27, 다른 작업 도중).

---

## [구현+검증, 2026-09-27] 방향(A) 채택(설계자 답변, QU-05F4B63B) - IrqSpinlock 신설+gLock 적용, commit 8567fe0 - 9/60 -> 1/60로 대폭 개선, 완전 해소는 아님

설계자가 방향 **(A)**(`gLock`을 cli/sti로 감싸는 `IrqSpinlock`으로
교체)를 답변했다.

**구현**: `minicore/libs/libkenv/spinlock.h`에 `IrqSpinlock`/
`IrqSpinlockGuard` 신설 - 기존 `Spinlock`과 같은 TAS 스핀 루프에
`pci.cpp`의 `PciConfigAccessGuard`/`scheduler.cpp`의 `enqueue()`/
`scheduleImmediate()`와 완전히 같은 RFLAGS 저장/복원 기법(`pushfq;
pop; cli` → `push; popfq`)을 결합했다 - 무조건 `sti`가 아니라 진입
시점의 실제 IF 값을 그대로 복원하므로, 이미 cli된 인터럽트 핸들러
도중 이 락을 잡아도 `PN-9326B06F`류의 IF=0 불변조건 파괴가 재발하지
않는다. `minicore/kernel/delayed_exec.cpp`의 `gLock`(및 `init`/
`schedule`/`cancel`/`pump`/`hasPending` 5개 사용처 전부)을 `Spinlock`
→ `IrqSpinlock`으로 교체.

**검증**: 표준 회귀 4종(PVH SMP1/SMP4, GRUB SMP4+실제initrd, GRUB
SMP4+AHCI) 전부 클린. dbgdriver 60회 배치 재검증 - **9/60(15%) →
1/60(1.7%)** - 유의미하게 개선됐으나 **완전히 0은 아니다.**

**잔여 1건(run9) 분석**: 같은 `rip=0xffffffff80110086`, 재빌드된
바이너리로 다시 `nm -n` 해석해도 동일하게 `kernel::Spinlock::lock()
+0x10`을 가리킨다 - **단, `Spinlock::lock()`은 이 커널 전체에서
공유되는 단일 비템플릿 구현이라, 이 심볼만으로는 "어느 `Spinlock`
인스턴스"가 걸렸는지 구분할 수 없다.** `gLock`은 이제 `IrqSpinlock`
이므로 이 잔여 1건은 (a) 같은 캐스케이딩 패턴이 **다른** 전역
`Spinlock` 인스턴스(async reactor/스케줄러 경로에서 도달 가능한 것)
에서 재발한 것이거나, (b) 우연히 같은 지점을 지나가는 완전히
무관한 저확률 타이밍 문제일 수 있다 - gdb 확인 없이는 구분 불가.

**결정이 필요한 지점(추가)**: 91%(9→1) 개선을 "충분한 하드닝"으로
받아들여 이 DC를 종결할지, 아니면 잔여 1/60을 gdb로 재확인해 다른
`Spinlock` 인스턴스까지 계속 추적할지 - 본문 원래의 "결정이 필요한
지점" 후보 중 (D)(먼저 gdb로 재확인)를 이 잔여분에도 다시 적용할지
설계자 판단이 필요하다. **완전 종결(approved 전이)은 이 추가 질의
답변까지 보류한다.**

## 참고 (추가)
- commit `8567fe0` - 방향(A) 실제 구현.
- `PN-6360E6E9`/`PN-D44504D1`(scheduled) - 60회 배치 원 소유 계획,
  9→1 개선 결과를 교차 기록했으나 완전 해소가 아니라 아직 completed
  전이 보류.

---

DC-54D69BEE 방향(B)(commit 76c9bff) 적용 이후, PN-6360E6E9/PN-D44504D1의
원 재현 도구(`minicore/dbgdriver`, GRUB SMP4+실제initrd, TEMP
kmain.cpp 부팅 훅)로 60회 배치 재검증을 수행했다 - **grep 패턴은
PN-6360E6E9가 확정한 정확한 패턴(`'panic\|watchdog'`, 대소문자 무시)
사용**.

## 결과 - 9/60(15%) 재현, 과거 재현율(약 5%)보다 오히려 높음

같은 근본 원인(DC-54D69BEE)의 다른 두 발현(`PN-93C26459`의
sockinherit+sockclient 15/15 무재현, `PN-7562DA62`의 epolltest+
sockclient 45/45 무재현)과 정반대로, **dbgdriver만은 방향(B) 적용
후에도 여전히 상당한 빈도로 재현된다** - 9회(run4/8/10/16/19/22/
53/55/58) 전부 `"NMI - watchdog: this core unresponsive"`.

## 핵심 관찰 - 9건 전부 정확히 같은 rip, DC-54D69BEE가 이미 예견한 캐스케이딩 gLock 메커니즘과 일치

9건 전부 워치독 덤프의 `rip=0xffffffff80110086`로 **완전히 동일**
(cs/rflags도 동일) - 같은 빌드의 `nm -n build/minicore.elf`로 해석한
결과 `_ZN6kernel8Spinlock4lockEv`(`kernel::Spinlock::lock()`,
libkenv/spinlock.h:21) 시작 주소(`0xffffffff80110076`)에서 +0x10
오프셋, 즉 **워치독을 유발한 코어가 아니라(그 코어는 `kHandleNmi`/
`kPanic`에서 정지) 다른 코어가 `Spinlock::lock()`의 스핀 루프 안에서
멈춰 있다가 관측된 것**이다 - `DC-54D69BEE`의 "[보강, 2026-09-27]"
절이 gdb로 직접 확인한 캐스케이딩 `DelayedExecutionQueue::gLock`
데드락과 정확히 같은 시그니처(자매 코어들이 전부 같은 지점에서
`gLock`을 놓고 스핀)로 보인다 - 이번엔 gdb attach까지는 하지 않고
정적 심볼 해석만 했으므로 100% 확정은 아니지만, 오프셋까지 완전히
일치하는 결정론적 재현이라 같은 메커니즘일 개연성이 매우 높다.

## 왜 방향(B)로 해소되지 않는가 - DC-54D69BEE 자신이 이미 남겨 둔 하드닝 과제

`DC-54D69BEE`의 "[보강]" 절은 이미 이렇게 적어 뒀다: "방향(B)만으로도
근본 우선순위 역전 자체는 해소되지만, `gLock`이 여전히 전역 단일
락이라는 사실 자체는 별도 하드닝 과제로 남는다(이 근본 원인이
없어져도, 다른 이유로 `pump()` 임계구역이 길어지는 회귀가 생기면
같은 캐스케이딩 패턴이 재발할 수 있음) - 당장 이 DC의 결정 범위에
넣진 않되, 참고용으로 남긴다." - 이번 재현이 바로 그 예견이 실제로
들어맞은 사례로 보인다.

`delayed_exec.cpp`의 `pump()` 자체의 락 보호 구간은 매우 짧다(만료
항목을 리스트에서 떼어내기만, 콜백은 락 밖에서 실행 - 주석에 명시된
설계 의도). 그런데도 dbgdriver 워크로드(스레드0/1이 각각
`DebugGetRegisters`/`DebugContinue`를 `kMaxPollAttempts=2,000,000`
한도로 폴링하며 `mc::submit()`을 극도로 빠르게 반복 제출 -
`minicore/dbgdriver/main.cpp` `kPollGetRegisters`/`kPollDebugContinue`)
는 AsyncTask 제출량이 socket 기반 재현들보다 훨씬 크다 - 이 정도
부하에서는 `kAsyncDrainConsecutiveRearmLimit=4`의 백오프가 매번
스케줄러 틱에게 열어주는 "탈출 창"이 있어도, 여러 코어가 각자
독립적으로 그 창-닫힘 주기를 반복하는 사이 우연히 gLock을 쥔 코어만
불운하게 계속 창이 닫힌 구간에 걸리는 저확률 사건이 여전히 가능한
것으로 추정된다(추정 - 확정하려면 gdb 사냥으로 재확인 필요, 아래
"결정이 필요한 지점" 참고).

## 결정이 필요한 지점 - 후속 하드닝 방향

이 결함(전역 `Spinlock`이 `cli`를 하지 않아, 락 보유 코어가 인터럽트
우선순위 문제로 오래 선점당하면 다른 모든 코어가 캐스케이딩으로
함께 멈추는 구조)은 `kAsyncDrainConsecutiveRearmLimit`처럼 특정
서브시스템 하나를 고치는 것으로는 근본적으로 막을 수 없다 - 같은
패턴이 미래의 다른 ISR/워크로드에서도 재발할 수 있는 구조적 취약점
이다. 방향을 여쭙는다:

1. **(A) `gLock`(및 유사하게 광범위하게 공유되는 전역 `Spinlock`)을
   인터럽트를 끄는 변형(`cli`/`sti`를 감싸는 새 락 타입, 예:
   `IrqSpinlock`)으로 교체** - 락을 쥔 동안은 그 코어에 어떤
   인터럽트도(우선순위 역전 여지 자체를 원천 차단) 끼어들 수 없게
   한다. 가장 근본적이지만 `libkenv/spinlock.h`가 "SMP AP 기동 0단계
   최소 프리미티브"로 못박혀 있어(설계자 지시, QU-B97FDA44) 새 타입
   추가가 그 원칙과 충돌하지 않는지 확인 필요, 또한 이런 락을 인터럽트
   핸들러 내부에서 잡는 경우(있다면) 이미 인터럽트가 꺼진 컨텍스트에서
   추가로 cli/sti 왕복이 안전한지도 검토 필요.
2. **(B) 워치독 자체를 더 관대하게** - 락을 쥔 코어가 일시적으로
   응답이 늦어도 무조건 NMI로 죽이지 않고, "이 코어가 지금 어떤
   전역 락을 쥐고 있다"는 걸 워치독이 알 수 있게(예: 코어별
   "현재 보유 중인 락 개수/식별자" 플래그) 그런 코어는 죽이지 않고
   더 오래 기다리는 예외를 둔다 - 락 보유 자체를 "정상적으로 바쁨"
   신호로 인정.
3. **(C) `kAsyncDrainConsecutiveRearmLimit`(현재 4)를 더 낮추거나
   `kAsyncDrainBatchLimit`(현재 32)를 낮춰 임시로 완화** - 근본
   구조는 그대로 두고 dbgdriver류 고부하 워크로드에서 재현 빈도만
   낮추는 미봉책. 실측 없이는 얼마나 낮춰야 안전한지 알 수 없다.
4. **(D) 이 재현이 gdb로 재확인됐을 때만 결정** - 지금은 정적 심볼
   해석뿐이라, 먼저 gdb 사냥으로 다른 3코어가 실제로 gLock을 놓고
   스핀 중인지 직접 확인한 뒤 방향을 정한다(가장 보수적).

## 참고
- `DC-54D69BEE`(approved) - 이 발견의 근본 원인 문서, 방향(B) 구현+
  검증 완료, "[보강]" 절이 이 하드닝 과제를 이미 예견해 둠.
- `PN-6360E6E9`/`PN-D44504D1`(둘 다 scheduled) - dbgdriver 재현
  도구의 원 소유 계획, 이번 60회 배치 결과를 교차 기록.
- `PN-93C26459`(completed) - 같은 근본 원인의 첫 발현(sockinherit),
  방향(B)로 완전히 해소됨(대조군).
- `minicore/libs/libkenv/spinlock.h` - `Spinlock`/`SpinlockGuard`
  정의, cli/sti 없음.
- `minicore/kernel/delayed_exec.cpp` - `gLock`/`pump()` 정의.
- `minicore/kernel/async_task.cpp` - `kAsyncDrainConsecutiveRearmLimit`
  /`kAsyncDrainBatchLimit` 정의.
