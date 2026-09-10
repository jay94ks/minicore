# 완료 보고: real-libc-syscall-layer M33 — LAPIC 타이머 보정(PIT/HPET 실측 기반, OPEN-62 해소)

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M33
**관련 결정**: [kernel-scheduler.md](../design/kernel-scheduler.md) ADR-184, ADR-208
**실행일**: 2026-09-10

## 완료한 것

1. **HPET 최소 드라이버(신규)** — `kernel/arch/x86_64/hpet.cpp/.hpp`.
   메인 카운터를 프리러닝으로 켜고 읽기만 한다(자체 인터럽트/
   비교기는 쓰지 않는다). `acpi.cpp`가 HPET ACPI 테이블도 찾도록
   확장했다(`find_and_parse_hpet`, `find_and_parse_mcfg`와 완전히
   같은 RSDP 검색 경로 재사용).
2. **PIT(8254) 채널2 최소 드라이버(신규)** — `kernel/arch/x86_64/pit.cpp/.hpp`.
   모드0+포트 0x61 bit5(OUT2 상태) 폴링으로 인터럽트 없이 카운트다운
   종료를 안다(HPET이 없을 때만 쓰이는 폴백).
3. **`calibrate_lapic_timer(target_time_slice_us)`(신규,
   `lapic.cpp`)** — LVT Timer를 마스크(실제 인터럽트 없이)+최대
   initial_count로 잰 뒤, HPET/PIT로 측정한 고정 10ms 창 동안 LAPIC이
   실제로 감소시킨 틱 수로 주파수를 역산해 목표 마이크로초에
   대응하는 initial_count를 계산한다.
4. **코어마다 1회 보정** — BSP는 `kernel_main.cpp`(부팅 극초반,
   devmgr의 유저랜드 ACPI 열거보다 훨씬 앞선 자리)에서 보정 후
   그 값으로 실제 주기 타이머를 켠다(기존 고정값 0x200000 대체).
   각 AP는 `smp.cpp`의 기동 시퀀스 끝에서 같은 함수를 불러 보정
   결과를 로그로 남기지만, 자기 주기 타이머를 실제로 켜지는
   않는다(M21/ADR-176이 이미 정한 "AP는 아직 run_queue에 참여하지
   않는다"를 그대로 유지 — 활성화는 M34의 몫).
5. **`ticks_for()`(scheduler.cpp) 수정** — `base_time_slice_us`가
   진짜 마이크로초가 되도록, 이제 `budget_us / k_timer_tick_period_us`
   (신규 공개 상수, `scheduler.hpp`, 1000=1ms)로 나눠 "몇 틱을
   기다려야 하는지"를 계산한다. `k_default_time_slice_ticks`(값 20)
   를 `k_default_time_slice_us`(값 20000=20ms)로 재해석했다.

## 계획 대비 범위 조정

ADR-191(M34용 사전 전략)은 "M33이 먼저 `timer_source_interface`
추상화를 만들고 M34가 코어별 인스턴스 배열로 완성한다"고 정해 뒀다.
이 라운드는 그 인터페이스를 만들지 않았다 — `calibrate_lapic_timer()`/
`lapic_start_periodic_timer()`를 호출부가 그대로 직접 부른다.
이유(ADR-208 §결정5): 이 시점엔 소비자가 BSP 하나뿐이고(AP는 아직
자기 주기 타이머를 켜지 않는다) 구현체 하나·호출자 하나짜리
인터페이스는 조기 추상화라고 판단했다 — M34가 실제로 "코어별
인스턴스"가 필요해지는 순간(AP도 자기 주기 타이머를 켜는 순간)에
인터페이스를 만드는 쪽이 그 설계를 실제 요구사항에 맞춰 검증할 수
있다. ADR-191 자신은 수정하지 않는다(그 전략은 M34에도 그대로
유효) — 이 조정은 ADR-208에 별도로 기록했다.

## 실행 중 발견

`ticks_for()`가 `base_time_slice_us`를 그대로 "타이머 틱 수"로
소비하고 있었다(그 필드 옆 기존 주석이 이미 이 사실과 "실제 보정이
필요해지면 이 함수 하나만 고치면 된다"를 정확히 예고해 뒀다) —
`calibrate_lapic_timer()`로 `initial_count`를 아무리 정확히
계산해도, `ticks_for()`가 나눗셈을 하지 않으면 `base_time_slice_us=20`
이 여전히 "20 틱"으로 오독되어 보정 전과 똑같이 부정확한 슬라이스가
나온다는 것을 코드 리뷰 중 발견했다. `ticks_for()` 수정+
`k_default_time_slice_ticks`→`k_default_time_slice_us` 재해석
(위 "완료한 것" 5번)으로 함께 해소했다 — OPEN-62가 지적한 "이름은
마이크로초, 실제로는 틱 수"라는 불일치는 계산 결과(initial_count)
뿐 아니라 그 결과를 실제로 소비하는 쪽(ticks_for)까지 함께 고쳐야
완전히 없어진다는 것을 보여준 사례다.

## 검증

x86_64 전체 재빌드 성공. QEMU에서 확인(BSP, 기본 1코어):
`[acpi] hpet_ok=1 base_phys=0xfed00000 period_fs=10000000` →
`[lapic] calibrate source=hpet elapsed_ticks=648025 window_us=10000
-> initial_count=64802 (target=1000us)` → `[lapic] BSP calibrated
initial_count=64802 (uncalibrated was 2097152)`. `MINICORE_QEMU_SMP=4`
로 4코어 부팅 시 BSP+AP 3개 전부 비슷한 자릿수로 계산됨을 확인
(예: 64226/62675/62596/62616 — 코어 간 편차 ~3% 이내, ADR-185가
요구하는 "코어마다 비슷한 틱 수" 전제를 만족). PIT 폴백 경로는 QEMU가
기본으로 HPET을 제공해 이 라운드에서 실측 검증하지 못했다(정직하게
보고 — 코드는 작성했으나 `-no-hpet` 등으로 HPET을 끈 QEMU 실행은
시도하지 않았다). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) — 4코어 전부 보정 로그 확인 |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

## 남겨 둔 것

real-libc-syscall-layer.md M34(진짜 멀티코어 선점형 스케줄러,
OPEN-63)부터 계속 진행한다 — `timer_source_interface`(ADR-191)를
그 시점에 만든다. PIT 폴백 경로의 실제 QEMU 검증(`-no-hpet`)은
이번 라운드에서 하지 않았다 — 필요해지면 별도로 확인한다.
