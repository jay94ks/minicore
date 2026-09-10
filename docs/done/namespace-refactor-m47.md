# 완료 보고: namespace-refactor M47 — `kernsrv::` 도입, 서버 14개 전부 감싸기

**대상 계획**: [namespace-refactor.md](../plan/namespace-refactor.md) §M47
**관련 결정**: [foundations.md](../design/foundations.md) ADR-198
**실행일**: 2026-09-10

## 완료한 것

`servers/*` 14개 서버 전부를 `namespace kernsrv::<서버명> { ... }`로
감쌌다 — 지금까지 서버들은 무네임스페이스 상태(파일 내부 익명
네임스페이스만 존재)였으므로 이 마일스톤은 "참조를 바꾸는" 리네임이
아니라 "새로 감싸는" 추가 작업이었다(계획이 이미 이렇게 구분해
둔 그대로):

| 서버 | 네임스페이스 |
|---|---|
| procsrv | `kernsrv::procsrv` |
| vfs | `kernsrv::vfs` |
| devmgr | `kernsrv::devmgr` |
| cfgsrv | `kernsrv::cfgsrv` |
| login | `kernsrv::login` |
| netsrv | `kernsrv::netsrv` |
| fs/memfs | `kernsrv::fs::memfs` |
| fs/fat32 | `kernsrv::fs::fat32` |
| fs/ext4 | `kernsrv::fs::ext4` |
| drivers/ps2 | `kernsrv::drivers::ps2` |
| drivers/usb | `kernsrv::drivers::usb` |
| drivers/console | `kernsrv::drivers::console` |
| drivers/virtio-blk | `kernsrv::drivers::virtio_blk` |
| drivers/virtio-net | `kernsrv::drivers::virtio_net` |

각 파일의 마지막 `#include` 줄 바로 뒤부터 파일 끝까지를 감쌌다.
**엔트리 포인트 심볼이 계획이 우려한 문제("네임스페이스로 감싸면
링커가 못 찾는다")를 실제로 겪지 않은 이유**: 사전 조사로 14개
서버 전부가 이미 `extern "C" [[noreturn]] void _start(const void*)`
관례를 쓰고 있음을 확인했다 — `extern "C"` 함수는 감싸는 C++
네임스페이스와 무관하게 링커에 보이는 이름이 그대로 `_start`로
유지된다(C++ 네임 맹글링이 `extern "C"`에는 적용되지 않는다). 그래서
`_start`를 네임스페이스 밖으로 따로 빼낼 필요가 전혀 없었다 —
계획이 우려했던 위험이 이 코드베이스의 기존 관례 덕분에 실제로는
발생하지 않았다.

## 검증

전체 재빌드 성공(첫 시도, 에러 0, 14개 서버 전부 링크 성공).
`servers/*` 변경이라 `cmake --build build/x86_64-clang --target
minicore_bootdisk_image`를 추가로 돌려 부트디스크를 재생성했다
(M22가 겪은 기존 함정 재확인, `add_custom_target`이라 기본 빌드에
안 걸림). QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 88) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 11) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 24) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 12) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 6) |

동작 변화 없음(모든 서버가 이전과 동일하게 기동·통신함) 확인 —
14개 서버가 실제로 서로 IPC로만 통신하고 직접 링크되는 참조가
없다는 계획의 전제(그래서 서버별로 독립적으로 처리해도 안전하다)도
실측으로 재확인됐다.

## 남겨 둔 것

M48(`kernsrv::proto` 분리)이 계속 진행 중.
