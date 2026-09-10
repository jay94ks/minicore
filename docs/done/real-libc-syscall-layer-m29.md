# 완료 보고: real-libc-syscall-layer M29 — 동적 링킹 도입 (부분 완료: 커널 쪽 PT_INTERP 인프라만)

**대상 계획**: [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) §M29
**관련 결정**: [kernel-memory.md](../design/kernel-memory.md) ADR-189, ADR-203
**실행일**: 2026-09-10

## 완료한 것 (커널 쪽 PT_INTERP 적재 인프라)

1. `kernel/arch/x86_64/elf_loader.{hpp,cpp}` — `load_elf()`에
   `load_bias`(기본값 0) 매개변수를 추가했다. `load_bias != 0`이면
   `ET_DYN`(PIE)도 허용하고, 모든 `p_vaddr`/`e_entry`에 그 바이어스를
   더해 호출자가 고른 주소에 적재한다. `load_bias == 0`(기본값,
   기존 모든 호출자)이면 이전과 완전히 동일하게 `ET_EXEC`만
   허용한다 — 회귀 없음.
2. `kernel/arch/x86_64/process_ops.cpp::build_process()` — 새
   `interp_data`/`interp_size` 매개변수를 추가했다. 0이 아니면
   그 ET_DYN 인터프리터를 고정 베이스(`k_interp_base=0x20000000`)에
   추가로 적재하고, 프로세스의 실제 진입점(`out.entry_rip`)을
   인터프리터의 것으로 바꾼다. `linux_abi_stack`(M28)이 함께
   요청되면 auxv에 **진짜** `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`(주
   프로그램의 것, ELF 헤더에서 직접 읽음)+`AT_ENTRY`(주 프로그램의
   진짜 진입점)+`AT_BASE`(인터프리터의 로드 바이어스)를 채운다 —
   M28의 가짜 0값 대신이다.
3. `mc_process_spawn_request`에 `interp_data`/`interp_size` 필드를
   추가했다(0이 기본값) — `process_spawn()` 시그니처도 같이
   확장했다(`interp_data`/`interp_size`에 기본 인자 `nullptr`/`0`을
   둬 기존 15개 호출자는 소스 변경이 필요 없었다).

## 실행하지 않은 것 — musl 자신의 `libc.so`/ld.so 빌드+부트스트랩

[ADR-203](../design/kernel-memory.md)에 전체 판단 근거를 기록했다.
요약: 실제 musl `Makefile`을 확인해 보니 `libc.so`는
`ldso/dlstart.c`+`ldso/dynlink.c`(2439줄)뿐 아니라 **musl 전체를
`-fPIC`로 다시 컴파일해 하나로 합친** 멀티콜 바이너리다. `dlstart.c`
의 `_dlstart_c`는 자기 자신의 `_DYNAMIC` 섹션을 읽어 스스로 재배치를
끝내기 전까지는 전역 데이터 접근이 전혀 안전하지 않은 자기 부트스트랩
코드이고, 이어지는 `dynlink.c`의 GOT/PLT 재배치+심볼 해석까지 이
커널에서 실제로 올바르게 동작시키는 것은 M27/M28이 겪은 것과 질적으로
다른 규모·위험도의 작업이라고 판단했다(재배치 버그는 대개 임의
주소로 점프하는 조용한 크래시로만 드러난다). M29의 계획 문서
자체가 이 정확한 상황("ELF 로더/auxv 확장에서 실제로 크게 막히면
정적 링킹으로 되돌아간다")을 사전에 명시해 둔 대로, 여기서 멈추고
정적 링킹(M28의 `userland/musl-hello`)을 계속 기본 검증 경로로
쓴다.

## 검증

x86_64 전체 재빌드 성공(커널+musl-hello 전부 이전과 동일하게
빌드·링크됨 — `interp_data`/`interp_size`가 항상 0/nullptr인
기존 경로는 손대지 않았다). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0) |

새 QEMU 확인 문자열은 추가하지 않았다 — 이번 라운드가 실제로
증명한 것은 "기존 경로가 안 깨졌다"뿐이고, `interp_data`를 실제로
쓰는 새 시나리오는 아직 없다(M29의 실제 목표 — "동적 링크된
musl-hello가 QEMU에서 실행됨" — 는 달성되지 않았다).

## 남겨 둔 것

real-libc-syscall-layer.md는 **정적 링킹 기반으로 M30부터 계속
진행한다**(ADR-203). M30 착수 시점에는 TLS(M28로 이미 흡수됨)는
빠지고 `SYS_mmap`/`SYS_munmap`(musl 자신의 malloc, mallocng)만
남는다. 이 보고서가 남긴 커널 쪽 PT_INTERP 인프라(`load_bias`,
`interp_data`/`interp_size`, 진짜 auxv 구성)는 향후 musl 동적
링킹을 다시 시도할 라운드가 그대로 재사용할 수 있다.
