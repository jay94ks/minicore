# 디버그 콘솔 스펙 (Debug Console)

**관련 결정**: ADR-007, ADR-037
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
    void printf(const char* fmt, ...);       // 최소 포맷터: %d %u %x %p %s %c %% 만 지원
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

## 5. kernel-bootstrap.md M1과의 관계

M1의 완료 기준("QEMU에서 시리얼 포트로 hello from kernel 출력")은 곧
이 스펙 §2~3의 최소 구현 완료 기준과 같다. M1을 구현하는 순서는:

1. §2의 UART 초기화 시퀀스를 arch별로 구현.
2. `klog::putc`/`klog::write` 구현.
3. `klog::init()` 호출 후 고정 문자열 "hello from kernel\n"을 출력.
4. `klog::printf`는 M2(BootInfo 덤프에 필요) 이전까지 구현.

## 아직 정하지 않은 것

- 인터럽트 기반 비동기 로깅 — 현재는 폴링/블로킹만 규정한다. 필요성이
  확인되면 후속 결정으로 추가한다.
- 커널 패닉 시 로그 출력·스택 덤프·시스템 정지 절차는 별도 스펙(추후)
  대상이다.
