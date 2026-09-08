# 디버그 콘솔 스펙 (Debug Console)

**관련 결정**: ADR-007, ADR-037, ADR-125, ADR-126
**관련 계획**: [kernel-bootstrap.md](../plan/kernel-bootstrap.md) M1

## 1. 목적

부팅 극초기(유저 프로세스는커녕 IPC조차 없는 시점)부터 커널이 관찰
가능한 로그를 남길 수 있는 최소 경로를 정의한다. 이 콘솔은 이후
유저 프로세스로 구현될 실제 터미널/콘솔 드라이버(ADR-007)와 **완전히
별개**이며, 오직 커널 자신의 디버깅 용도다. 유저 프로세스에게 이 경로를
직접 노출하지 않는다(IPC를 통한 로그 조회 API는 이 스펙의 범위 밖).

## 2. 하드웨어 인터페이스

### 2.1 x86_64 — 16550 호환 UART (COM1)

- 포트 베이스: `0x3F8` (I/O 포트, QEMU `-serial stdio` 기본 매핑).
- 초기화: DLAB 비트 설정 후 분주비(baud divisor) 설정(115200 baud
  기준 divisor=1), 8N1(8비트, 패리티 없음, 정지비트 1), FIFO는
  비활성 상태로 시작(폴링 모드) — 인터럽트 기반은 이 스펙의 범위 밖.
- 송신: Line Status Register(포트+5)의 THR Empty 비트가 설 때까지
  폴링한 뒤 Data 레지스터(포트+0)에 1바이트 기록.

### 2.2 aarch64 — PL011 UART (QEMU virt)

- MMIO 베이스: `0x09000000`.
- 레지스터 오프셋: `DR=0x00`(데이터), `FR=0x18`(플래그), `IBRD=0x24`,
  `FBRD=0x28`(정수/소수 분주), `LCR_H=0x2C`(라인 제어), `CR=0x30`(제어).
- 초기화: 분주자 설정, 8N1, UART 활성화(`CR`의 UARTEN/TXE 비트).
- 송신: `FR`의 TXFF(송신 FIFO full) 비트가 꺼질 때까지 폴링한 뒤
  `DR`에 1바이트 기록.

## 3. 커널 내부 API

```cpp
namespace klog {
    void init();                            // arch별 UART 초기화
    void putc(char c);                       // 1바이트 즉시 송신 (폴링, 블로킹)
    void write(const char* s, size_t len);   // putc 반복
    void printf(const char* fmt, ...);       // 최소 포맷터: %d %u %x %p %s %c %% + l 수식어(%ld %lu %lx, 64비트) 지원
}
```

- 예외/RTTI 없이 구현하며(ADR-010), 가변인자는 freestanding 표준 헤더
  `<cstdarg>`로 처리한다. 동적 할당 없는 고정 버퍼 기반 포맷터로 작성한다.
- **락**: 여러 코어가 동시에 로그를 남길 수 있으므로(ADR-033) 출력
  자체는 전역 스핀락 1개로 직렬화한다 — 로그 경로는 성능이 중요하지
  않으므로 ADR-033의 "필요한 곳만 최소 락" 원칙의 명시적 예외다(ADR-037).

## 4. 로그 형식 (관례, 강제 아님)

`[LEVEL] module: message\n` 형식을 권장한다. `LEVEL`은
`DEBUG`/`INFO`/`WARN`/`ERROR`/`PANIC` 등 자유롭게 정의하며, 이 스펙은
형식을 강제하지 않고 §3의 원시 출력 API만 규정한다.

## 5. 패닉 시 스택 백트레이스 (ADR-126)

`libk_detail::panic_hook`(§3의 `LIBK_PANIC` 매크로가 부르는 훅,
`kernel/core/panic.cpp`)은 `[PANIC] file:line: msg`를 찍은 뒤 스택
백트레이스를 함께 남긴다:

```
[PANIC] kernel/core/mm/page_allocator.cpp:273: mm::alloc_pages called before mm::init
[PANIC] backtrace:
  #0 0xffffffff8010adf4
  #1 0xffffffff80110f72
  #2 0xffffffff801102b9
  ...
```

- rbp(x86_64)/x29(aarch64) 프레임포인터 체인을 최대 16단까지 걸어
  return address만 raw hex로 출력한다. **함수명은 커널 안에서
  해석하지 않는다** — 빌드 호스트에서
  `llvm-addr2line -e <kernel.elf> <addr>` 또는
  `llvm-symbolizer -e <kernel.elf>`로 사후 변환한다. `<kernel.elf>`는
  `build/<arch>-clang/kernel/arch/<arch>/minicore_kernel_<arch>.elf`
  (ADR-125로 항상 `-g` 포함, strip 안 함).
- 이 백트레이스가 유효하려면 프레임포인터가 보존돼야 한다 —
  `toolchain/common.cmake`가 모든 컴파일 단위에 `-fno-omit-frame-pointer`를
  상시 적용한다(ADR-125).
- CPU 레지스터 전체 덤프(GPR)는 아직 없다 — IDT/예외 핸들러가 생기는
  시점(M9 이후)에 별도로 다룬다(ADR-126 근거 참고).
- 스택 자체가 손상된 경우(더블 폴트 등) 체인이 중간에 끊기거나
  무의미할 수 있다 — 이 경우는 §6의 GDB 원격 디버깅으로 직접
  들여다본다.

## 6. GDB 원격 디버깅 (ADR-125)

`tools/run-qemu.sh`에 `MINICORE_QEMU_GDB=1`을 주면 QEMU가 CPU를 즉시
정지시킨 채(`-S`) `tcp::1234`에 GDB 스텁을 연다. 기본(미설정)은 기존과
동일하게 즉시 부팅하므로, `tools/smoke-test-x86_64.sh` 같은 고정
타임아웃 자동화 경로와는 이 변수를 같이 쓰지 않는다.

```
# 터미널 1
MINICORE_QEMU_GDB=1 tools/run-qemu.sh x86_64

# 터미널 2
tools/debug-gdb.sh x86_64
```

`debug-gdb.sh`는 같은 커널 ELF를 심볼과 함께(ADR-125로 항상 `-g`)
그대로 읽어 `target remote :1234`로 붙는다 — 소스 라인 브레이크포인트,
`step`/`next`, 지역변수 조회가 gdb가 지원하는 그대로 된다. 또한
`MINICORE_QEMU_TRACE=1`을 주면 트리플폴트/CPU 리셋/인터럽트 이벤트를
`<빌드 디렉토리>/qemu-trace.log`에 남긴다 — 원인 불명의 hang을
살펴볼 때 GDB 붙이기 전에 먼저 확인하기 좋다.

## 7. kernel-bootstrap.md M1과의 관계

M1의 완료 기준("QEMU에서 시리얼 포트로 hello from kernel 출력")은 곧
이 스펙 §2~3의 최소 구현 완료 기준과 같다. M1을 구현하는 순서는:

1. §2의 UART 초기화 시퀀스를 arch별로 구현.
2. `klog::putc`/`klog::write` 구현.
3. `klog::init()` 호출 후 고정 문자열 "hello from kernel\n"을 출력.
4. `klog::printf`는 M2(BootInfo 덤프에 필요) 이전까지 구현.

## 아직 정하지 않은 것

- 인터럽트 기반 비동기 로깅 — 현재는 폴링/블로킹만 규정한다. 필요성이
  확인되면 후속 결정으로 추가한다.
- 커널 이미지에 심볼 테이블을 직접 임베딩해 패닉 시점에 함수명까지
  바로 보여주는 것(§5는 raw 주소만 출력하고 `llvm-symbolizer` 사후
  변환에 의존한다, ADR-126) — 필요성이 확인되면 별도 ADR 대상이다.
- 예외/인터럽트 핸들러(IDT) 도입 시점의 CPU 레지스터 전체 덤프(GPR) —
  M9 이후 범위(ADR-126 근거 참고).
