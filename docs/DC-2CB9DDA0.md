# DC-54D69BEE 방향(B) 적용 후에도 dbgdriver는 9/60(15%) 재현 - 캐스케이딩 gLock 데드락 하드닝 방향 결정 요청

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-2CB9DDA0
  status: review
  updatedAt: 2026-09-27T09:10:47.340Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

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
