# 완료 보고: smp-fpu-bringup M9 — FPU/SIMD 컨텍스트 스위칭

**대상 계획**: [smp-fpu-bringup.md](../plan/smp-fpu-bringup.md) §M9
**관련 결정**: ADR-127([kernel-scheduler.md](../design/kernel-scheduler.md)),
ADR-134([kernel-memory.md](../design/kernel-memory.md), 이번에 새로 발견·해결한 슬랩 정렬 버그)
**실행일**: 2026-09-09

## 완료 기준 달성 확인

M9의 목표: "데모 스레드 두 개가 서로 다른 상수를 xmm0에 로드한 뒤
여러 차례 yield()로 번갈아 실행하고, 매번 자기 값이 유지됐는지
확인"(계획 §M9). QEMU 실행으로 확인했다.

```
cmake --build build/x86_64-clang --target minicore_kernel_x86_64
tools/smoke-test-x86_64.sh
# => 기존 M1~M8 항목 전부 PASS + 신규 8개(M9) 전부 PASS
```

실제 QEMU 출력(발췌):

```
[fpu] create_kernel_thread a=1 b=1
[fpu] thread A iteration 0 xmm0 preserved=1
[fpu] thread B iteration 0 xmm0 preserved=1
[fpu] thread A iteration 1 xmm0 preserved=1
[fpu] thread B iteration 1 xmm0 preserved=1
[fpu] thread A iteration 2 xmm0 preserved=1
[fpu] thread A done all_preserved=1
[fpu] thread B iteration 2 xmm0 preserved=1
[fpu] thread B done all_preserved=1
```

## 수행한 작업

### 1. `arch_x86_64::init_fpu()` — CR0/CR4 활성화 시퀀스

[kernel/arch/x86_64/fpu.hpp](../../kernel/arch/x86_64/fpu.hpp)/[.cpp](../../kernel/arch/x86_64/fpu.cpp) —
CR0.EM 클리어(에뮬레이션 트랩 없이 x87/SSE 직접 실행)·CR0.MP 설정,
CR4.OSFXSR·OSXMMEXCPT 설정(FXSAVE/FXRSTOR 허용 + SIMD FP 예외를
`#UD`가 아닌 `#XM`으로). `kernel_main()` 극초기(첫 `arch_context_switch`
보다 반드시 먼저)에서 호출한다.

### 2. `object::thread::fxsave_area` + 스레드 생성 시 기본값 patch

[kernel/core/object/kernel_objects.hpp](../../kernel/core/object/kernel_objects.hpp)에
`alignas(16) uint8_t fxsave_area[512] = {};` 필드 추가.
[kernel/core/sched/scheduler.cpp](../../kernel/core/sched/scheduler.cpp)의
`create_kernel_thread`/`create_user_thread`가 공통 헬퍼
`init_fxsave_area()`로 FCW(offset 0)=`0x037F`, MXCSR(offset 24)=`0x1F80`을
patch한다(나머지는 이미 0으로 value-initialize돼 있음, Intel SDM
Vol.1 §13.6 리셋 기본값).

### 3. `arch_context_switch` — 무조건 FXSAVE/FXRSTOR (eager)

[kernel/arch/x86_64/context_switch.S](../../kernel/arch/x86_64/context_switch.S) —
시그니처에 `old_fxsave_area`/`new_fxsave_area`(rcx/r8) 2개 인자를
추가했다. `old_fxsave_area`가 `nullptr`이면 저장을 건너뛴다
(`sched::start()`의 첫 스위치, `sched::exit()`의 소멸 스레드). 4개
호출부(`start`/`yield`/`block`/`exit`, scheduler.cpp) 전부 갱신.

### 4. 검증 데모: `thread_fpu_a_entry`/`thread_fpu_b_entry`

[kernel/arch/x86_64/kernel_main.cpp](../../kernel/arch/x86_64/kernel_main.cpp) —
서로 다른 64비트 패턴을 `movq`로 xmm0에 실어 놓고 `yield()`를 반복,
매번 `movq`로 되읽어 비교한다. 커널 전체가 `-mno-sse`로 빌드되므로
(CMakeLists.txt, "CR4.OSFXSR 미설정 시절의 안전장치") 이 두 함수만
`__attribute__((target("sse2")))`로 개별적으로 SSE 코드생성을 허용했다
— 나머지 커널은 여전히 컴파일러가 암묵적으로 XMM을 쓰지 않는다(이
전역 플래그를 걷어낼지는 이 마일스톤의 범위 밖으로 남겨 둔다, §다음
마일스톤과의 접점).

### 5. 회귀 방지 근거: 음성 대조 실험

계획이 명시한 대로, FXSAVE/FXRSTOR를 일시적으로 비활성화하고 같은
데모를 돌려 **실패**함을 먼저 확인했다(스레드 A가 B에게 값을
빼앗기는 것을 로그로 직접 봄):

```
[fpu] thread A iteration 0 xmm0 preserved=0
[fpu] thread B iteration 0 xmm0 preserved=1
...
[fpu] thread A done all_preserved=0
[fpu] thread B done all_preserved=1
```

(A만 깨지고 B는 우연히 안 깨진 것은 스케줄링 순서 때문 — 매 반복
B가 A보다 나중에 xmm0를 쓰므로 B 자신의 값은 항상 자기가 마지막에
쓴 값과 일치해 보인다. 어느 쪽이든 "값이 서로 오염된다"는 버그
자체는 A의 실패로 이미 증명된다.) 이후 FXSAVE/FXRSTOR를 복원해
위 "완료 기준 달성 확인"의 결과로 되돌아옴을 재확인했다.

## 실행 중 새로 발견한 버그: 슬랩 청크 16바이트 정렬 누락 (ADR-134)

`fxsave_area` 필드를 추가하고 처음 QEMU로 부팅했을 때, 스케줄러의
첫 컨텍스트 스위치(`sched::start()`)에서 원인 불명으로 멈췄다(로그가
`[initrun] setup_initrun_process ok=1` 뒤로 아무것도 안 나옴, 패닉
메시지도 없음 — IDT가 아직 없어 예외가 트리플폴트로 조용히 귀결).

**진단 절차** (이번 세션 앞부분에서 만든 ADR-125/126 디버깅 인프라를
그대로 실전에 썼다):

1. `MINICORE_QEMU_TRACE=1`(ADR-125)으로 재부팅해 QEMU의 `-d
   cpu_reset,guest_errors` 로그를 받았다 — `check_exception old:0xd
   new:0xd` → `#DF` → 재차 `0xd` → `Triple fault`, 폴트 RIP=
   `ffffffff8010fc41`.
2. `llvm-symbolizer`(ADR-126 워크플로)로 그 RIP를
   `context_switch.S:52`로 역추적.
3. `llvm-objdump -d`로 정확한 명령어가 `fxrstor (%r8)`임을 확인 —
   `#GP`(벡터 0xd)는 FXSAVE/FXRSTOR가 정렬 안 된 메모리 피연산자를
   받았을 때의 전형적 원인(Intel SDM)이라는 걸 알고 있었으므로,
   폴트 시점 R8 레지스터 값(`0xffff8000000918b8`, 마지막 자리 8 —
   16의 배수가 아님)을 확인해 가설을 확진했다.
4. `mm::slab_alloc()`(`kernel/core/mm/slab.cpp`) 구현을 읽어
   `slab_header`가 24바이트(16의 배수 아님)라 그 뒤에 오는 모든
   청크가 8바이트만큼 어긋난다는 근본 원인을 찾았다.

**수정**: `slab_header`에 `alignas(16)` 추가(ADR-134) — 24→32바이트로
패딩되어 이후 모든 청크가 다시 16의 배수 오프셋에 놓인다. 수정 후
같은 부팅이 정상적으로 끝까지 진행됨을 확인했다.

이 버그는 **M9 자체의 버그가 아니라 M3(슬랩 힙, `kernel-bootstrap-m3.md`)
때부터 있었던 잠재 버그**다 — M1~M8의 어떤 슬랩 할당 대상도 8바이트
보다 엄격한 정렬을 요구한 적이 없어 드러나지 않았을 뿐이다. M9이
`alignas(16)` 필드를 처음 슬랩에 태우면서 처음으로 실제 증상을
냈다.

## 검증 결과 (정직하게 보고)

- **확인함**: 두 커널 스레드가 각자의 xmm0 값을 여러 차례의 `yield()`에
  걸쳐 정확히 보존함(양성), 그리고 FXSAVE/FXRSTOR 없이는 실제로
  깨짐(음성 대조) — 둘 다 QEMU 실행으로 직접 확인했다.
- **확인함**: `slab_header` 정렬 수정 후 M1~M8 스모크 테스트 전체가
  회귀 없이 그대로 통과함(슬랩을 쓰는 다른 모든 경로 — handle_table,
  endpoint, notification 등 — 가 이 변경으로 깨지지 않았다).
- **확인하지 못함**: 다중 코어 환경에서의 FPU 상태 — M9은 여전히
  논리적으로 1코어(ADR-035)에서만 실행된다. 코어별로 독립적인 FPU
  레지스터 파일을 갖는다는 사실 자체가 실제 다중 코어에서 문제를
  일으키는지는 M10/M11(AP 기동, 다중 코어 검증)에서나 확인 가능하다.
- **확인하지 못함**: 유저 스레드(initrun 같은)의 FPU 상태 보존 —
  데모는 커널 스레드 2개만 썼다. `create_user_thread`도 같은
  `init_fxsave_area()`를 거치므로 코드 경로는 동일하지만, 유저모드
  자체에서 SSE 코드가 실제로 도는 경우(예: 향후 libc가 부동소수점을
  쓸 때)는 아직 실행으로 확인되지 않았다.
- **확인하지 못함**: aarch64 FPSIMD 대응 — ADR-127이 이미 "설계만
  남기고 구현은 별도 계획"으로 명시한 부분, 이번에도 손대지 않았다.

## 다음 마일스톤과의 접점

- M10(IDT 기초 + AP 기동)이 실제 예외 핸들러를 갖추면, 이번에
  `MINICORE_QEMU_TRACE`+`llvm-symbolizer`로 수동 진단했던 트리플폴트
  경로가 커널 자체의 `#GP`/`#DF` 핸들러로 훨씬 빠르게(그리고 QEMU
  트레이스 없이도) 드러나게 된다 — M10 착수 시 이 경험을 반영할
  가치가 있다.
- M11b(lazy FPU 전환 + XSAVE/AVX, ADR-133)는 이 M9의 eager
  FXSAVE/FXRSTOR 경로를 대체한다 — `context_switch.S`의 무조건
  FXSAVE/FXRSTOR 두 줄이 `CR0.TS` 설정 + `#NM` 핸들러로 바뀐다.
  `object::thread::fxsave_area`(512바이트, FXSAVE 전용 레이아웃)도
  XSAVE 가변 영역(최대 1024바이트로 상한, ADR-133) 크기로 커져야
  한다.
- 커널 전체의 `-mno-sse` 계열 플래그(CMakeLists.txt)를 걷어내
    컴파일러가 자유롭게 SSE/XMM을 쓰게 할지는 아직 결정하지 않았다 —
    지금은 FXSAVE/FXRSTOR가 항상 실행되므로 이론적으로는 안전해졌지만,
    커널 전역에 미치는 영향(모든 번역 단위의 코드생성이 바뀜)이 커서
    이 마일스톤에서 독립적으로 결정하지 않았다. 필요성이 확인되면
    별도 ADR 대상.
