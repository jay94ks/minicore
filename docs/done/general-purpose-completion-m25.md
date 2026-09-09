# 완료 보고: general-purpose-completion M25 — 최소 네트워킹

**대상 계획**: [general-purpose-completion.md](../plan/general-purpose-completion.md) §M25
**관련 결정**: [boot-and-drivers.md](../design/boot-and-drivers.md) ADR-181
(virtio-net 드라이버 + netsrv DHCP 왕복 자기테스트, ARP 없이)
**실행일**: 2026-09-10

## 완료한 것

### D1. `servers/drivers/virtio-net` — 순수 하드웨어 드라이버

[servers/drivers/virtio-blk/main.cpp](../../servers/drivers/virtio-blk/main.cpp)
(M15, ADR-043)와 완전히 같은 legacy virtio I/O 포트 레지스터
레이아웃을 재사용해 devmgr에 vendor:device=`0x1AF4:0x1000`으로
등록한다. RX(큐 0)/TX(큐 1) 두 virtqueue를 직접 구성하고, RX는
버퍼 4개를 미리 avail 링에 올려 두는 방식(수신은 디바이스가
비동기로 채운다), TX는 blk의 단일 요청-응답 패턴을 그대로 재사용
(디스크립터 하나, notify 후 `used_idx` 폴링)한다. 이더넷 프레임을
그대로 송수신할 뿐 ARP/IP/UDP는 전혀 모른다 — netsrv와의 IPC
프로토콜은 `OP_SEND_FRAME`/`OP_RECV_FRAME`/`OP_GET_MAC` 세 개뿐이다.

### D2. `servers/netsrv` — 최소 프로토콜 스택 + DHCP 왕복 자기테스트

지금까지 빈 `INTERFACE` 라이브러리였던 `servers/netsrv`에 처음으로
실제 코드를 채웠다. virtio-net에 IPC로 의존해 이더넷/IP/UDP
프레이밍을 직접 구현하고, 부팅 시 DHCPDISCOVER를 브로드캐스트로
보내 DHCPOFFER를 받는 왕복을 자기테스트로 증명한다. ARP를 구현하지
않는다 — DHCPDISCOVER는 이더넷·IP 둘 다 브로드캐스트라 목적지
MAC을 미리 구할 필요가 없고, QEMU의 usermode 네트워킹(SLIRP)이
내장 DHCP 서버를 항상 갖고 있어 외부 네트워크 접근 없이도 결정적
으로 응답이 온다. IP 체크섬은 정확히 계산하고, UDP 체크섬은
0(RFC 768이 허용하는 비활성)으로 보낸다.

### D3. 부트 디스크/QEMU 배선

`servers/CMakeLists.txt`의 `mkbootdisk.py` 호출에 virtio-net
(virtio-blk 다음, trusted)과 netsrv(fat32/ext4 다음, `depends=virtio-net`,
non-trusted — vfs와 같은 정신)를 추가했다. `tools/run-qemu.sh`에
`MINICORE_QEMU_NET=1`(bus0/device9, `-netdev user`+`virtio-net-pci`)
opt-in을 추가했다(ADR-125의 "기본값 유지" 패턴 — 기본은 여전히
장치 없음).

### D4. 실제로 겪은 버그(2건, 둘 다 실기 관련이 아니라 드라이버 자체 버그)

1. **vring 베이스 오프셋 누락**: `compute_layout()`이 반환하는
   오프셋은 그 큐 자신의 vring 시작을 기준으로 한 상대값인데,
   초기 구현이 이를 DMA 버퍼 전체 시작에 곧바로 더했다 — RX는
   자기 vring이 오프셋 0이라 우연히 값이 맞았지만, TX는 그렇지
   않아 즉시 드러났다(`used_idx`가 영원히 그대로, TX가 조용히
   멈춤).
2. **RX 버퍼 영역 페이지 정렬**: RX 버퍼 4개의 총 크기(개수×버퍼
   크기)가 4096의 배수가 아니어서, 그 다음에 오는 TX vring이
   페이지 정렬되지 않는 주소에 놓였다 — `queue_address` 레지스터가
   물리주소를 페이지 프레임 번호로 저장하므로 정렬이 깨지면 완전히
   다른 위치를 가리킨다. 버퍼 크기를 1536→2048로 늘려 해결했다.

둘 다 QEMU 실측(`debug_log_hex`로 오프셋/디스크립터 값을 직접
확인)으로 발견·수정했다 — 자세한 진단 경위는 ADR-181 참고.

### D5. 검증

새 스위트 `tools/smoke-test-net-x86_64.sh`(`MINICORE_QEMU_NET=1`
opt-in)를 만들어 DHCPDISCOVER 전송("[netsrv] dhcp discover
sent=1")과 DHCPOFFER 왕복("[netsrv] udp roundtrip ok=1")을 확인했다.
기존 4개 스위트(smoke/SMP/NUMA/AVX, 네트워킹 미활성)도 전부 재확인
했다 — virtio-net/netsrv가 부트 디스크 서비스 목록에 새로 끼어들어도
(virtio-blk와 fat32 사이) 나머지 순서를 깨지 않았다.

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh`(네트워킹 미활성) | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-net-x86_64.sh`(신설) | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0개) |

## 남겨 둔 것 (OPEN)

새 OPEN 항목은 열지 않는다 — ARP 부재, DHCP 옵션 미해석(임대
관리 없음), TCP 부재, 실제 소켓 API 부재는 모두 ADR-181 본문에
"알려진 단순화"로 명시했고, 계획이 스스로 정한 "UDP 정도로 시작"
범위를 그대로 충족한 것이라 별도 추적이 필요한 미결 사항으로
보지 않는다(TCP는 계획 자체가 이미 "후속"이라고 명시했다).

## 다음

[general-purpose-completion.md](../plan/general-purpose-completion.md)
§M26(실제 libc 포팅 재도전, 이 계획의 마지막 마일스톤)으로 이어간다.
