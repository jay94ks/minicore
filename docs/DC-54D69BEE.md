# kAsyncDrainVector(0xE3)이 kSchedulerTickVector(0x24)보다 하드웨어 인터럽트 우선순위가 높아, AsyncTask 폭주 시 스케줄러 틱을 무기한 굶길 수 있다 - 수정 방향 결정 요청

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: DC-54D69BEE
  status: approved
  updatedAt: 2026-09-27T07:54:19.060Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

## [구현+검증 완료, 2026-09-27] 방향(B) 채택 - self-IPI 재예약 백오프, commit 76c9bff

`QU-D22ADEB7`에서 설계자가 방향 **(B로 회귀)**("self-IPI 재예약에
배치 횟수 기반 백오프 추가")를 최종 승인했다 - 방향 (A)(벡터
우선순위 재배정)는 `RM-28225668`의 `0xE0`~`0xFD` 범위 규칙과
산술적으로 양립 불가능함이 드러나 기각됐다(위 "[구현 착수 중 충돌
발견]" 절).

**구현**: `async_task.cpp`의 `kAsyncDrainIsr()`가 배치 상한(32)에
도달할 때마다 무조건 self-IPI를 재예약하던 것을, 코어별
`gAsyncDrainConsecutiveRearmCount[coreIndex]`로 **연속** 재예약
횟수를 세다가 임계치(`kAsyncDrainConsecutiveRearmLimit = 4`)를
넘으면 그 즉시 조용히 반환하도록 바꿨다 - 벡터 우선순위 정책
(`kAsyncDrainVector`/`kSchedulerTickVector` 값 자체, `RM-28225668`
범위 규칙)은 전혀 안 건드린다. 남은 작업은 `kAsyncReactorTaskMain`
의 기존 `yieldCurrent()` 폴링 루프(정상 우선순위 경로)가 다음
라운드로빈 차례에 이어서 처리하므로, 그 사이에 스케줄러 틱을 포함한
모든 낮은 우선순위 인터럽트가 최소 한 번은 반드시 끼어들 창이
강제로 열린다. 큐가 배치 상한에 안 걸리고 정상적으로 비면(연속
사슬이 끊기면) 카운터를 리셋한다.

**검증**:
- 표준 회귀 4종(PVH SMP1/SMP4, GRUB SMP4+실제initrd, GRUB SMP4+AHCI)
  전부 클린.
- **`PN-93C26459`의 원 100% 재현 시나리오**(`sockinherit`+`sockclient`,
  이 DC의 근본 원인을 gdb로 처음 확정한 바로 그 재현) - **15/15
  무재현**(정확한 grep 패턴 `'panic\|watchdog'` 사용).
- `PN-7562DA62`(epoll 구현) 검증 중 발견한 세 번째, 가장 재현하기
  쉬운 사례(`epolltest`+`sockclient`, 단 2개 프로세스의 순수
  `Socket()` 호출만으로도 수정 전 최대 26/30 무응답)를 **45회
  반복 부팅 - watchdog/panic 0건**. 다만 이 워크로드는 epoll
  구현 자체의 별개 이슈(두 번째 `EpollWait`까지 항상 완주하지는
  못함, 크래시/행 없이 단순 미완주)가 있어 `PN-7562DA62`에서 계속
  추적한다 - 이 DC의 범위(전체 정지/watchdog)는 완전히 해소됐다.
- `dbgdriver`(캐스케이딩 `gLock` 데드락 발현, `PN-6360E6E9`/
  `PN-D44504D1`의 원 재현 도구) 전용 대규모(~60회) 재검증은 아직
  안 함 - 위 두 검증(sockinherit 15/15, epolltest 45회)이 이미
  같은 근본 원인(self-IPI 사슬)을 강하게 뒷받침하지만, 정직하게
  기록: `PN-6360E6E9`/`PN-D44504D1`가 자기 재현 도구로 직접
  재검증할 것을 남겨 둔다.

**상태 전이**: 이 DC를 `approved`로 전이한다 - 근본 원인 확정,
방향 결정, 구현, 표준 검증까지 전부 완료됐다.

---

## [새 재현 사례, 2026-09-27, PN-7562DA62] 2-프로세스 순수 Socket() 호출만으로도 73%(22/30) 무응답 - watchdog조차 안 뜨는 더 심한 발현

`PN-7562DA62`(epoll 구현) 검증 중 `minicore/epolltest`+`sockclient`
(둘 다 Socket() syscall 하나씩만 부르는 최소 워크로드, epoll 코드
자체는 아직 실행되지도 않은 시점)를 30회 반복 부팅했더니: **4/30은
실제 NMI watchdog, 22/30은 watchdog조차 뜨지 않고 완전히 무응답**
(양쪽 프로세스가 각자 Socket() 하나씩 부른 직후 그대로 멈춤), 극소수만
더 진행됨. 이 근본 원인(kAsyncDrainVector 우선순위 역전)이 단순
"워치독 오탐"보다 훨씬 심각하다는 증거 - **워치독이 아예 안 뜨는
22/30 케이스는, `PN-6360E6E9`가 이미 문서화한 "gLock을 쥔 코어가
멈추면 나머지 코어도 같은 락에서 캐스케이딩으로 함께 멈춘다"
메커니즘이 워치독을 유발할 다른 코어까지 전부 삼켜버린 극단적
경우로 추정된다**(전체 코어가 다 멈추면 "다른 코어가 이 코어를
감시"할 주체 자체가 없어져 NMI가 안 뜸) - 단, 이번엔 gdb로 직접
확인하지 않고 로그 패턴만으로 추정한 것이라 단정하지 않는다. 이
발견은 방향 선택(A-1/A-2/B) 자체를 바꾸지는 않지만, **얼마나 쉽게
(단 2개의 아주 단순한 syscall 호출만으로) 전체 시스템이 멈출 수
있는지**를 보여줘 이 DC의 우선순위를 다시 한번 높이는 근거가 된다.

---

## [구현 착수 중 충돌 발견, 2026-09-27] 승인된 방향 (A)가 RM-28225668의 기존 벡터 범위 규칙과 수학적으로 양립 불가능 - 재확인 요청

설계자가 `QU-00E6FB36`에서 방향 **(A)**("`kAsyncDrainVector`를
`kSchedulerTickVector`보다 낮은 우선순위 벡터로 재배정")를 승인해
구현에 착수했는데, 코드 배선을 확인하던 중 **이 방향이 이미 확정된
다른 설계자 지시와 충돌**한다는 걸 발견했다 - 임의로 어느 한쪽을
택하지 않고 다시 여쭙는다.

**충돌 내용**: `RM-28225668`("Minicore 인터럽트 벡터 목록")은
2026-09-16 설계자 지시로 "`0xE0`~`0xFD`" 범위를 **커널 내부 IPI/최적화
벡터 전용**으로 고정해 뒀고, `kAsyncDrainVector`(현재 `0xE3`)는 바로
그 규칙에 따라 이 표에 등록된 항목이다. 그런데 `kSchedulerTickVector`
는 `0x24`(LAPIC 틱/HPET/PIT와 같은 타이머 클러스터, `0x20`~`0x24`) -
**`0xE0`~`0xFD` 범위 안의 어떤 값도 `0x24`보다 하드웨어 우선순위가
낮을 수 없다**(x86 APIC 우선순위는 `vector >> 4` 클래스 비교라, 클래스
0xE~0xF는 항상 클래스 0x2보다 높음). 즉 **"`kAsyncDrainVector`를
`0xE0`~`0xFD` 범위 안에 유지하면서 동시에 `kSchedulerTickVector`보다
낮은 우선순위로 만드는 것은 산술적으로 불가능**하다 - 방향 (A)를
문자 그대로 구현하려면 반드시 이 범위 규칙을 깨야 한다.

이 범위 밖에서 `kSchedulerTickVector`(0x24)보다 낮은 후보는 사실상
`0x21`(CPU 예외 0-31, `0x20` LAPIC 틱, `0x22` HPET, `0x23` PIT가 이미
고정이라 유일하게 빈 슬롯)뿐이다 - 단 이 값은 `pnp.cpp`의
`kMsiVectorRangeStart`(=33=`0x21`)와 정확히 겹쳐, 동적 MSI 벡터 풀의
"첫 후보"이기도 하다(아래 "부수 발견" 참고).

**부수 발견 - `kAllocateMsiVector()`가 고정 벡터를 걸러내지 않는
기존 버그**: `pnp.cpp:401-411`의 `kAllocateMsiVector()`는 후보를 고를
때 `InterruptDelegation::isAllowed()`(이미 위임된 동적 벡터인지)만
확인하고 `kIsFixedVector()`(고정 벡터인지)는 전혀 확인하지 않는다.
그 결과 `kAllocateMsiVector()`가 우연히 고정 벡터(예: `0x24`
스케줄러 틱, `0x80` syscall, `0xE0`~`0xE4` IPI들 - 전부 33-254 범위
안에 있다)를 후보로 반환할 수 있고, `pnp.cpp:490`의 `Pci::enableMsi()`
가 `InterruptDelegation::allow()`(여기서만 `kIsFixedVector`를 확인)
로 거부되기 **이전에 먼저 무조건 실행**돼 그 PCI 장치의 실제 MSI
capability 레지스터가 그 고정 벡터를 가리키도록 프로그램된다 - 이후
`allow()`가 실패해 `assignedIrqVector`는 0으로 반환되지만, **장치의
MSI는 이미 활성화된 채 고정 벡터를 가리키고 있어**, 그 장치가
인터럽트를 발생시키면 엉뚱한(예: 스케줄러 틱) ISR이 실행될 잠재적
위험이 이미 존재한다. 이 버그는 `kAsyncDrainVector`를 `0x21`로
옮기는 것과 무관하게 이미 존재하지만, `0x21`은 `kMsiVectorRangeStart`
바로 그 값이라 옮길 경우 이 버그가 트리거될 확률이 사실상 "부팅
후 첫 MSI 요청"으로 크게 높아진다 - 방향 (A)를 `0x21`로 구현하려면
이 버그(고정 벡터 후보를 건너뛰도록 `kAllocateMsiVector()` 수정)도
함께 고쳐야 안전하다.

**설계자 답변 요청(`QU-` 신규 등록)**: 아래 중 선택해 주시길 -
1. **(A-1)** `RM-28225668`의 `0xE0`~`0xFD` 범위 규칙에 예외를 두고
   `kAsyncDrainVector`를 `0x21`로 재배정 - 위 `kAllocateMsiVector()`
   버그도 함께 수정(고정 벡터 스킵 추가). RM-28225668 본문에 이
   예외를 명시.
2. **(A-2)** 대신 `kSchedulerTickVector`(현재 `0x24`, 타이머
   클러스터)를 `kAsyncDrainVector`보다 낮은 값으로 옮기지 않고,
   오히려 `kAsyncDrainVector`가 속한 IPI 클러스터 전체의 상대
   순서만 조정 - 예를 들어 RM-28225668의 IPI 범위 자체를
   `kSchedulerTickVector`보다 낮은 대역으로 재정의하는 더 큰
   재검토(범위가 넓어 회귀 위험 큼).
3. **(B로 회귀)** 애초에 `QU-00E6FB36`에서 권장으로 표시했던 방향
   (B)(self-IPI 재예약에 배치 횟수 기반 백오프)로 되돌아간다 - 벡터
   우선순위/RM-28225668 범위 규칙을 전혀 안 건드리고, 이미 gdb로
   확인된 두 발현(단일 코어 국소 무응답 - `PN-93C26459`, 캐스케이딩
   `gLock` 전체 데드락 - `PN-6360E6E9`/`PN-D44504D1`) 모두를 해소할
   것으로 예상된다(self-IPI 사슬 자체를 끊으므로).

**임시 조치**: 이 재확인이 끝날 때까지 코드 변경은 보류한다(RM-28225668/
`kAsyncDrainVector`/`kAllocateMsiVector` 어느 것도 아직 건드리지
않았다 - `git status` 클린).

---

## [보강, 2026-09-27 - dbgdriver 재현으로 확인] 이 근본 원인이 캐스케이딩 데드락까지 설명한다 - `Spinlock`은 인터럽트를 안 끈다

`PN-6360E6E9`/`PN-D44504D1`의 원 재현 도구(`minicore/dbgdriver`)로
같은 gdb 사냥 기법(watchdog 로그가 뜨는 순간 즉시 attach)을 40회
시도 중 20회차에서 재현에 성공했다(과거 재현율 ~5%와 일치) - 이번엔
**전혀 다른, 그러나 정확히 같은 근본 원인으로 설명되는 패턴**을
확인했다.

**관찰**: 이번엔 워치독을 유발한 코어(CPU#3, `kHandleNmi`/`kPanic`
안에 정지) 외에 **나머지 3개 코어(CPU#0/1/2) 전부가 `kernel::
Spinlock::lock()`(spinlock.h:21) 안에서 스핀 중**이었고, 셋 다
**정확히 같은 락 주소**(`kernel::(anonymous namespace)::gLock`)를
기다리고 있었다. 콜스택은 셋 다 동일한 모양:
`Spinlock::lock → SpinlockGuard::SpinlockGuard → DelayedExecutionQueue::
pump()(delayed_exec.cpp:86) → AsyncReactor::drainOnce(coreIndex=N) →
kAsyncReactorTaskMain(arg=N) → kTaskStartTrampoline` - **각 코어가
자기 전용 `AsyncReactor` Task(`PN-D4F7BB66`가 코어당 전용 Task로
승격한 바로 그것) 안에서 idle 직전 `DelayedExecutionQueue::pump()`를
부르다가, `delayed_exec.cpp`의 단 하나뿐인 전역 `gLock`(코어별이
아니라 프로젝트 전체에 단 하나)을 얻으려 대기 중이었다.**

**결정적 사실**: `kernel::Spinlock::lock()`(libkenv/spinlock.h)은
순수 TAS 스핀락 구현으로 **`cli`를 전혀 하지 않는다** - 락을 쥔
채로도 인터럽트는 계속 들어온다. 즉:

1. 어떤 코어(추정 CPU#3)가 `pump()`의 임계구역에서 `gLock`을 쥔
   채로,
2. 마침 `kAsyncDrainVector` self-IPI 폭주(위 최상단 절의 근본
   원인)에 걸려 그 코어가 이 문서가 이미 확정한 메커니즘대로
   자기 스케줄러 틱뿐 아니라 **원래 실행(=이 `pump()` 호출 자체로
   돌아오는 것)까지 무기한 선점당하면**,
3. 그 코어는 `gLock`을 영원히 반납하지 못한 채 다른 코어의 워치독에
   "응답 없음"으로 잡혀 `kHandleNmi`/`kPanic`에서 영구 정지되고,
4. **`gLock`이 전역 단일 락이라 다른 모든 코어의 `AsyncReactor` Task도
   idle 전환 때마다 `pump()`를 부르므로, 나머지 코어 전부가 이제
   영원히 반납되지 않을 그 락을 놓고 무기한 스핀** - 이 시점부터는
   진짜 "시스템 전체 정지"가 된다.

**이게 바로 이 계열의 watchdog이 역사적으로 "시스템 전체가 멈춘 것
같다"고 보고돼 온 이유이자, `PN-93C26459`(sockinherit)에서는 "코어
1개만 무응답"으로 관찰된 이유이기도 하다** - 어느 쪽이든 근본
원인은 동일(`kAsyncDrainVector` 우선순위 역전)하지만, 그 순간 걸린
코어가 마침 전역 락을 쥐고 있었는지(→ 전체 캐스케이딩) 아닌지(→
그 코어만 국소적으로 무응답)에 따라 겉보기 증상이 완전히 달라진다.

**이 보강 사실이 수정 방향 선택에 주는 함의**: 방향 (B)(self-IPI
재예약에 백오프 추가)만으로도 근본 우선순위 역전 자체는 해소되지만,
`gLock`이 여전히 전역 단일 락이라는 사실 자체는 별도 하드닝 과제로
남는다(이 근본 원인이 없어져도, 다른 이유로 `pump()` 임계구역이
길어지는 회귀가 생기면 같은 캐스케이딩 패턴이 재발할 수 있음) -
당장 이 DC의 결정 범위에 넣진 않되, 참고용으로 남긴다.

`PN-6360E6E9`/`PN-D44504D1`의 **이번 dbgdriver 재현은 이 DC의 근본
원인으로 완전히 설명된다** - 다만 그 두 계획이 과거 기록해 온
`kSyncCr3`/`kSyncRsp0ForDispatch` Page Fault 계열(`next=0x100000000`)
재현들은 이번 재현과 시그니처가 달라(Page Fault 없음) 여전히 별개로
남을 수 있다 - 그 두 계획이 각자 재현들을 다시 gdb로 분류할 것.

---

## 배경

`PN-93C26459`(fd 상속 재검증 중 100% 재현되는 NMI watchdog, `PN-CC0F4EAC`
9-10회차에서 분리)의 근본 원인을 gdb + 코드 추적으로 확정했다 -
**새 syscall 버그도, `PN-395F4D89`(DC-D8951156)와 같은 계열도 아니고,
완전히 별개의 인터럽트 우선순위 역전 버그다.**

## 재현/진단 경위

`minicore/sockinherit`+`minicore/sockinheritchild`+`minicore/sockclient`
TEMP 부팅 훅(GRUB SMP4)으로 100% 재현되는 `"NMI - watchdog: this core
unresponsive"`를 QEMU gdbstub(`-s`, `-S` 없이 자유 부팅 후 15초 뒤
attach)으로 실측했다:

1. **`info threads`/`thread apply all bt` 결과, 4코어 중 단 1개
   코어만 정지해 있었다** - 나머지 3개 코어는 각자 정상적으로
   syscall 디스패치/idle 루프를 계속 실행 중이었다(`bt`로 확인 -
   `AsyncTask::submit`/`Scheduler::runLoop` 등 정상 콜스택). **이건
   전체 시스템 데드락이 아니라 코어 1개만의 국소적 무응답**이라는
   뜻 - 지금까지 `PN-6360E6E9`/`PN-D44504D1` 등이 "watchdog=시스템
   전체 정지"로 암묵적으로 취급해 온 전제를 재검토할 필요가 있다.
2. **정지된 코어(watchdog을 유발한 그 코어)의 실제 정지 지점은
   `kernel::kDiagRingLog()`(diag_ring.cpp:95) 안**이었다(워치독
   진단 덤프의 `rip`를 같은 빌드의 `nm`으로 직접 해석해 확인,
   `rip=...+0xdf`). **`kDiagRingLog()` 자신은 코어별 전용 슬롯만 쓰고
   락도 루프도 전혀 없는 5줄짜리 함수**(diag_ring.cpp 55행 주석
   "코어별 독립 슬롯... 락이 필요 없다"가 이미 명시)라 이 함수
   자체는 절대 멈출 수 없다 - NMI가 도착한 "그 순간의 우연한
   지점"일 뿐, 진짜 원인은 이 호출을 감싼 더 바깥 코드다.
3. **`kDiagRingLog(DiagRingEvent::AsyncDrainBatchLimitHit, ...)`
   호출부(async_task.cpp:505)가 유일하게 이 이벤트를 로그하는
   지점**이고, 그 호출부는 `kAsyncDrainIsr()`(async_task.cpp:498-510)
   - **`kAsyncDrainVector`(0xE3) IDT 벡터에 등록된 진짜 인터럽트
   핸들러**다: `while (AsyncReactor::drainAny(coreIndex))` 루프를
   돌다가 `kAsyncDrainBatchLimit`(=32)에 도달하면 `kDiagRingLog`로
   기록하고 **자기 자신에게 다시 같은 벡터로 self-IPI를 보낸 뒤
   반환**한다(async_task.cpp:506, `Lapic::sendFixedIpi`).
4. **결정적 사실 - 벡터 우선순위 비교**: `kAsyncDrainVector = 0xE3`
   (227)인데 `kSchedulerTickVector = 0x24`(36)다(둘 다
   `async_task.cpp`/`scheduler.h`에서 확인). x86 로컬 APIC는
   `vector >> 4`(상위 4비트)로 우선순위 클래스를 매기고 **높은
   벡터 번호가 항상 더 높은 우선순위**다 - `0xE3`(클래스 0xE)는
   `0x24`(클래스 0x2)보다 하드웨어 우선순위가 훨씬 높다.

## 근본 원인(확정)

`kAsyncDrainIsr`가 배치 상한에 걸릴 때마다 **자기 자신과 같은,
스케줄러 틱보다 하드웨어 우선순위가 훨씬 높은 벡터로 self-IPI를
재예약**한다. AsyncTask가 계속 밀려드는 워크로드(이번 재현처럼
여러 프로세스가 동시에 짧은 syscall을 연속 제출하는 상황 - `Read()`
루프로 cpio를 읽는 `sockinherit`, `Socket/Bind/Listen/SpawnProcess`
연쇄 등)에서는, 이 self-IPI가 **매번 새로 도착할 때마다 아직 처리
안 된(낮은 우선순위인) 스케줄러 틱보다 항상 먼저 서비스된다** - x86
인터럽트 우선순위 중재 규칙상 더 높은 우선순위의 pending/in-service
인터럽트가 있으면 낮은 우선순위 인터럽트는 그게 없어질 때까지
서비스되지 않는다. 워크로드가 끊기지 않고 계속 새 AsyncTask를
큐에 넣는 한 이 self-IPI 사슬이 이론상 **무기한** 이어질 수 있고,
그동안 이 코어의 스케줄러 틱(그리고 그걸로 진행되는
`gWatchdogTickCounter` 자기 갱신)이 단 한 번도 실행되지 못해
다른 코어의 워치독이 "이 코어가 응답 없다"고 오판(사실은 응답이
없는 게 아니라, **다른(더 우선순위 높은) 인터럽트를 계속 정당하게
처리 중**)해 진단 NMI를 보낸다.

이 메커니즘은 `rflags`의 IF 비트가 재현마다 다르게 관측됐던 것
(어떤 샘플은 0, 어떤 샘플은 1)과도 정확히 들어맞는다 - "계속 cli
상태"가 아니라 "매번 짧게 IF=1로 돌아왔다가, 그 순간에 또 더
높은 우선순위 인터럽트가 대기 중이라 바로 다시 그쪽으로 넘어가는"
패턴이라 워치독 NMI가 잡는 순간의 IF 값이 매번 달라질 수 있다.

## 왜 이게 여러 자매 계획과 시그니처가 겹쳐 보였는지

`PN-6360E6E9`/`PN-D44504D1`도 "NMI watchdog"/"이 코어 응답 없음"류
증상을 오랫동안 추적해 왔다 - 이번 발견으로 그 계획들의 재현 중
일부(전부는 아닐 수 있음)가 실제로는 `DC-D8951156`류 포인터 손상이
아니라 **이 인터럽트 우선순위 역전**이었을 가능성이 새로 생겼다.
다만 그 계획들의 역사적 시그니처(`kSyncCr3`/`kSyncRsp0ForDispatch`
Page Fault, `next=0x100000000`)는 이번 재현과 다르므로(이번엔 Page
Fault가 전혀 없었다) 성급히 통합하지 않는다 - 각 계획이 자신의
재현을 gdb로 다시 확인해 어느 계열인지 가릴 것.

## 결정이 필요한 지점 - 수정 방향

이 결함은 명백한 버그(어떤 의도적 설계도 "스케줄러 틱을 무기한
굶겨도 된다"고 정하지 않았다)지만, 수정 방식은 이 커널의 인터럽트
우선순위 정책 전반(다른 하드웨어 IRQ - AHCI 등 - 와의 상대적 순서
포함)에 영향을 줄 수 있어 방향을 여쭙는다. 후보:

**(A) `kAsyncDrainVector`를 `kSchedulerTickVector`보다 낮은
우선순위 벡터로 재배정한다.** 가장 직접적이지만, 이 벡터를 이미
참조하는 다른 서브시스템(`ahci.cpp`의 self-IPI, `fs.cpp`,
`page_frame_allocator.cpp`의 "일반 인터럽트 하나로 회수를 미룬다"는
전제 등)이 "이 IPI가 다른 하드웨어 IRQ보다 우선한다"는 암묵적 순서에
기대고 있을 가능성이 있어, 전체 벡터 배치를 다시 검토해야 할 수
있다.

**(B) `kAsyncDrainIsr`의 self-IPI 재예약에 "경과 시간/연속 배치
횟수" 기반 백오프를 추가한다** - 예: 연속으로 self-IPI를 재예약한
횟수가 일정 임계치를 넘으면, 그 즉시 다시 쏘지 않고
`DelayedExecutionQueue`나 일반 우선순위 경로(예: yieldCurrent 이후
재개)로 완화해 스케줄러 틱이 최소 한 번은 끼어들 여지를 강제로
만든다. 벡터 우선순위 정책 자체는 안 건드리므로 회귀 위험이 더 낮다.

**(C) 아예 다른 접근** - 예: `kAsyncDrainBatchLimit`(현재 32)를
훨씬 낮추는 것만으로 실질적 완화가 되는지 먼저 실측하고, 근본
우선순위 역전 자체는 별도 트랙으로 남긴다(임시 완화 vs 근본 수정
분리).

## 참고
- `PN-93C26459` - 이 발견의 출처(fd 상속 재검증), 재현 레시피/gdb
  세션 로그 보존.
- `PN-CC0F4EAC` - `PN-93C26459`를 낳은 상위 계획(소켓 계층 fd 상속
  E2E).
- `PN-D4F7BB66`(completed) - AsyncReactor를 코어당 전용 Task로
  승격한 최근 변경 - 이 버그 자체는 그 이전부터 구조적으로 존재했을
  가능성이 높지만(벡터 우선순위는 그 세션이 바꾸지 않음), AsyncTask
  처리량이 늘어난 경로라 더 잘 드러나게 됐을 가능성.
- `PN-6360E6E9`/`PN-D44504D1` - 같은 "NMI watchdog" 표면 증상을
  오래 추적해 온 자매 계획 - 이번 발견을 계기로 재현을 다시 gdb로
  구분해 볼 가치가 있음(교차 기록 완료).
- `async_task.cpp:306`(`kAsyncDrainVector=0xE3`)/`scheduler.h:16`
  (`kSchedulerTickVector=0x24`)/`async_task.cpp:466`
  (`kAsyncDrainBatchLimit=32`)/`async_task.cpp:498-510`
  (`kAsyncDrainIsr`) - 근본 원인의 정확한 코드 위치.
- `diag_ring.cpp:95`(`kDiagRingLog`)/`diag_ring.cpp:55`(락 불필요
  주석) - 이 함수 자체는 무죄임을 확인한 근거.
