# 완료 보고: libs-restructure M50 — `uapi.hpp` 폐지 + `mc` 커널/유저 통합

**대상 계획**: [libs-restructure.md](../plan/libs-restructure.md) §M50
**관련 결정**: [foundations.md](../design/foundations.md) ADR-200(ADR-132 보강)
**실행일**: 2026-09-10

## 완료한 것

1. `libs/mc/include/mc/syscall.h`를 커널·유저 공용 syscall ABI의
   단일 출처로 재작성했다 — `kernel/include/uapi.hpp`가 손으로
   복제해 온 message 레이아웃(`mc_message`), syscall 번호
   (`MC_SYSCALL_*`), 요청/응답 구조체(`mc_process_spawn_request`,
   `mc_exec_request`, `mc_map_phys_request`, `mc_brk_request` 등),
   권한 비트(`MC_RIGHT_*`), 전송 모드(`MC_TRANSFER_*`)를 전부
   순수 C 구조체/`#define`으로 옮겼다.
2. `MC_LAND_KERNEL` 매크로 가드를 도입했다 — 이 매크로가 정의된
   상태로 include하면(커널) 구조체/enum/상수만 노출되고, syscall
   트램폴린 함수(`mc_raw_syscall`/`mc_ipc_call`/`mc_ipc_recv`/
   `mc_ipc_reply`/`mc_debug_log`/`mc_thread_exit`/`mc_brk`)의 정의는
   `#ifndef MC_LAND_KERNEL`로 숨긴다. 매크로가 없으면(기존 유저랜드
   소비자) 지금까지와 동일하게 전부 노출된다.
3. `kernel/CMakeLists.txt`의 `minicore_kernel` 인터페이스 타깃에
   `target_compile_definitions(... INTERFACE MC_LAND_KERNEL)`와
   `target_include_directories(... INTERFACE libs/mc/include)`를
   추가해, `minicore_kernel_core`(PUBLIC 링크)→
   `minicore_kernel_x86_64`(PRIVATE 링크)까지 이 매크로/인클루드
   경로가 전이적으로 전파됨을 확인했다.
4. `kernel/include/uapi.hpp`를 `git rm`으로 삭제했다.
5. `kernel/`, `init/`, `servers/`(19개 파일)에서
   `#include <uapi.hpp>`를 `#include <mc/syscall.h>`로, `uapi::`
   접두 심벌(약 36종, `k_syscall_process_spawn`→
   `MC_SYSCALL_PROCESS_SPAWN`, `transfer_mode::copy`→
   `MC_TRANSFER_COPY`, `process_spawn_request`→
   `mc_process_spawn_request`, `message`→`mc_message` 등)을 일괄
   치환했다. `kernel/arch/x86_64/kernel_main.cpp`,
   `process_ops.cpp`, `init/initrun/main.cpp`에 남아 있던
   `uapi.hpp` 파일명 언급 프로즈 주석도 `mc/syscall.h` 기준으로
   손으로 정리했다.
6. `init/initrun/CMakeLists.txt`와 14개 `servers/*/CMakeLists.txt`
   전부에 `libs/mc/include`를 인클루드 경로로, `minicore_libmc`를
   링크 대상으로 추가했다(이전에는 오직 `minicore_libk`만
   링크했다 — `uapi.hpp`가 헤더 전용이라 링크가 필요 없었기
   때문).

## 실제로 겪은 문제 — 유저랜드 `memset` 미정의 링크 에러

`uapi::message` 등 옛 구조체는 필드마다 `= 0` 기본 멤버
초기화자(NSDMI)가 있어 비트리비얼 타입이었다 — `{}`로 값
초기화하면 그 생성자 본문(필드별 저장 명령)으로 컴파일됐다. 새
`mc_message` 등은 순수 C aggregate라 트리비얼 타입이고, `{}`
초기화가 "통째로 0으로 채워라"로 인식되어 구조체가 충분히
커지자(수십~백 바이트대) 컴파일러가 인라인 저장 대신 `memset`
호출로 낮췄다. 커널에는 이미 `kernel/core/freestanding_mem.cpp`가
있어 문제가 없었지만, 유저랜드(initrun+서버 14개)는 이런
`memset` 제공자가 지금까지 한 번도 필요 없었다 — 첫 재빌드에서
`initrun.elf` 링크가 `ld.lld: error: undefined symbol: memset`으로
즉시 드러냈다.

호출부(`mc_message`/`mc_process_spawn_request` 등 aggregate
zero-init)가 `init/`+`servers/` 전역에 102곳이라 손으로 개별
전환하는 것은 비현실적이었다(`servers/cfgsrv/main.cpp`,
`servers/vfs/main.cpp`는 이미 같은 문제를 겪어 필드별 수동
초기화로 회피한 기존 주석이 남아 있었다). 대신 시스템 차원에서
해결했다:

1. `libs/mc/src/freestanding_mem.c`(신규) — `kernel/core/
   freestanding_mem.cpp`와 같은 순수 C 구현(memset/memcpy/
   memmove/memcmp)을 유저랜드용으로 추가했다.
2. `libs/mc/CMakeLists.txt`의 `minicore_libmc` STATIC 소스에
   추가하고, `kernel/core/freestanding_mem.cpp`와 같은 이유로
   `-fno-builtin`을 적용했다(안 그러면 루프 자체가 다시
   memcpy/memset 호출로 최적화돼 무한 재귀가 된다).
3. initrun+서버 14개 전부(15개 실행파일) 중 그 어느 것도 이전에
   `minicore_libmc`를 링크하지 않았음(오직 `minicore_libk`만)을
   발견하고, 전부에 `minicore_libmc`를 추가했다. 커널은
   `minicore_libmc`를 링크하지 않으므로 커널 쪽 `memset`과
   중복 심벌 충돌이 없다.

이 조치 후 x86_64 전체 재빌드가 에러 0으로 성공했다(58개 타깃
링크 완료 — 유일한 경고는 `efi_main.cpp:155`의 기존 미사용
매개변수 경고로 이 작업과 무관).

## 부수 발견 — `mc_m12_self_info`는 아직 실사용 중

옛 `uapi.hpp`의 주석은 procsrv/cpio가 갖춰지면
`k_m12_self_info`류 구조체가 "완전히 제거된다"고 적어 뒀지만,
실제로는 `servers/procsrv/main.cpp::run_loader_test`가 지금도
이 구조체(`mc_m12_self_info`, `MC_M12_SELF_ELF_USER_VADDR`,
`MC_M12_SELF_INFO_USER_VADDR`)를 사용한다. 이번 이전 과정에서
그 사실을 그대로 옮기고, 옛 주석의 잘못된 "제거 예정" 서술은
반영하지 않았다(코드가 실제로 그렇게 동작하므로).

## 검증

x86_64 전체 재빌드 성공(58개 타깃, 에러 0). bootdisk 재생성
(`--target minicore_bootdisk_image`, `servers/*`/`init/initrun`
변경이라 필수). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 88) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 11) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 24) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 12) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 6) |

동작 변화 없음(순수 리팩터 + memset 갭 보완) 확인. 커널이 처음으로
`mc`의 헤더(`libs/mc/include/mc/syscall.h`)를 참조하게 됐고, 커널·
유저 양쪽이 같은 헤더의 같은 구조체 정의를 공유함을 확인했다 —
더 이상 손으로 맞춘 두 개의 사본(`uapi.hpp` vs `mc/syscall.h`)이
아니다.

## 남겨 둔 것

`libs-restructure.md` 전체(M49~M50) 완료 — 이 계획에는 더 이상
다음 마일스톤이 없다. `namespace-refactor.md` M45(`uapi`→
`kern::proto` 리네임)가 이 M50으로 대체됐다는 점도 재확인됨(실제로
`uapi.hpp` 자체가 삭제됐으므로 별도 리네임이 무의미해졌다).
