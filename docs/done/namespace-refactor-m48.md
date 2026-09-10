# 완료 보고: namespace-refactor M48 — `kernsrv::proto` 분리 (계획 최종 마일스톤)

**대상 계획**: [namespace-refactor.md](../plan/namespace-refactor.md) §M48
**관련 결정**: [foundations.md](../design/foundations.md) ADR-198
**실행일**: 2026-09-10

## 완료한 것

착수 시점에 실제로 확인해 보니, ADR-198의 매핑표가 예로 든 "netsrv의
이더넷/IP/UDP **헤더 구조체**"는 실제로 존재하지 않았다 — netsrv는
구조체 타입 없이 원시 바이트 버퍼를 `put_be16`/`get_be16`류 헬퍼로
직접 오프셋 조작한다(M25가 그렇게 구현했다). 대신 그 조작 코드
안에 흩어진 **이름 붙은 상수** 몇 개가 실제로 "이 서버가 발명한
값이 아니라 외부 표준이 정의하는 값"이었다:

- `k_ethertype_ipv4`(IEEE EtherType 레지스트리)
- `k_ip_proto_udp`(IANA IP 프로토콜 번호)
- `k_dhcp_client_port`/`k_dhcp_server_port`(RFC 2131 well-known 포트)
- `k_dhcp_magic_cookie`(RFC 2131 매직 쿠키)

이 5개를 `servers/netsrv/main.cpp`에서 `kernsrv::proto`(신설, 서버
소속이 아닌 공용 네임스페이스)로 옮겼다. `k_dhcp_xid`(0x4D435231,
"MCR1")는 주석이 이미 밝히고 있던 대로 이 왕복 테스트만의 식별자—
netsrv 자신의 값이라 옮기지 않았다.

## 실제로 겪은 문제 — 중첩 네임스페이스 정의 구문의 함정

처음에는 이미 열려 있는 `namespace kernsrv::netsrv { ... }` **안에서**
`namespace kernsrv::proto { ... }`를 열려고 했다가 두 번 실패했다 —
C++의 중첩 한정 네임스페이스 정의(`namespace A::B { }`)는 시작
지점부터 `namespace A { namespace B { } }`를 그 자리에 그대로
쓴 것과 같아서, 이미 `kernsrv::netsrv` 안에 있는 상태에서 쓰면
`kernsrv::netsrv::kernsrv::proto`라는 **새 이름**이 만들어진다
(바깥의 전역 `::kernsrv`를 찾아 재사용하지 않는다) — 컴파일러가
`use of undeclared identifier ... did you mean
'::kernsrv::netsrv::kernsrv::proto::...'` 로 즉시 드러냈다.
`kernsrv::netsrv` 블록을 중간에 닫고 다시 여는 방식도 두 번째
시도에서 같은 함정에 다시 걸렸다(파일에 이미 있던 익명
`namespace { ... }`가 파일 끝까지 열려 있어서 내가 연 지점이 실제로는
그 안이었다). 최종적으로는 **`kernsrv::proto` 블록을
`kernsrv::netsrv`가 열리기 전에 먼저 정의**하고, `kernsrv::netsrv`
안에서 `using namespace kernsrv::proto;`로 끌어와 기존 호출부
(9곳)를 전혀 건드리지 않는 방식으로 정리했다.

## 검증

전체 재빌드 성공 + bootdisk 재생성 + QEMU 5개 회귀 스위트 전부 확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 88) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 11) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 24) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 12) |
| `tools/smoke-test-net-x86_64.sh` | PASS(exit=0, FAIL 0, PASS 6, DHCP 왕복 자체도 회귀 없음) |

## 계획 완료

[namespace-refactor.md](../plan/namespace-refactor.md)의 M44/M46/M47/M48
전부 완료됐다(M45는 [libs-restructure.md](../plan/libs-restructure.md)
M50으로 대체돼 스킵). 이 계획 자체에는 더 이상 다음 마일스톤이 없다.
남겨 둔 판단 지점(M44의 `boot` 처리, M46의 `kern::proc` 미신설)은
각각의 done 보고에 기록돼 있다.
