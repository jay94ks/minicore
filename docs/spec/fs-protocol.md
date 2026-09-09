# FS 서버 공통 프로토콜 스펙 (M13 최소 버전)

**관련 결정**: [filesystem.md](../design/filesystem.md) ADR-018(fd 라우팅),
ADR-047(synthetic FS 서버), ADR-101(공유 모드/잠금), ADR-128(`fs_node_id`),
[kernel-ipc-objects.md](../design/kernel-ipc-objects.md) ADR-151(IPC의
cross-address-space 확장)
**관련 스펙**: [ipc.md](ipc.md)(Call/Reply, message 구조), [procsrv.md](procsrv.md)
§4.1(`open_file_id`)

ADR-018 §영향이 "spec 단계에서 상세 정의"라고 예고해 둔 문서다. 이
라운드(M13)는 **VFS↔FS 서버, 클라이언트↔FS 서버** 양쪽에서 실제로
동작을 검증하는 데 필요한 최소 오퍼레이션 3개(open/write/read)만
정의한다 — close, 공유 모드/잠금(ADR-101), `fs_node_id`(ADR-128)의
실제 필드 배치, 디렉터리 나열 등은 이후 라운드에서 채운다.

## 1. 메시지 인코딩 — 전부 `regs[]`만 사용(M13 한정)

[ipc.md](ipc.md) §4의 `message`는 `pages[]`로 임의 크기 데이터를 옮길
수 있지만, **M13은 그 경로를 쓰지 않는다** — ADR-151이 메시지 구조체
자체(label/regs/handle_count)는 서로 다른 유저 프로세스 사이에서도
안전하게 번역하지만, `pages[]`가 가리키는 실제 데이터 버퍼 내용은
아직 같은 주소공간 전제로만 안전하다(OPEN-59). M13은 경로/데이터를
전부 `regs[4]`(32바이트) 안에 눌러 담는 것으로 이 제약을 피해 간다 —
그래서 경로는 32바이트, 쓰기/읽기 payload는 16바이트로 제한된다.
이후 라운드가 `pages[]`의 cross-address-space 전달을 구현하면(OPEN-59
해소) 이 제약은 사라진다.

## 2. 오퍼레이션

| `label` | 이름 | 방향 | 의미 |
|---|---|---|---|
| `1` | `OP_OPEN` | 클라이언트→VFS | 경로를 열거나 만든다 |
| `2` | `OP_WRITE` | 클라이언트→FS 서버(직접) | 열린 파일에 쓴다 |
| `3` | `OP_READ` | 클라이언트→FS 서버(직접) | 열린 파일에서 읽는다 |

### 2.1. `OP_OPEN` (클라이언트 → VFS)

- 요청: `label=1`, `regs[0..3]` = 경로(ASCII, NUL 패딩, 최대
  32바이트 — 32바이트를 넘는 경로는 M13 범위 밖이다). `page_count=0`,
  `handle_count=0`.
- 처리: VFS는 M13에서 마운트 테이블이 없다(단일 memfs, ADR-018의
  경로 탐색은 이후 라운드) — 받은 경로를 그대로 memfs에게
  `OP_OPEN`으로 다시 전달한다(VFS 자신이 memfs의 클라이언트가 된다 —
  스폰 시점에 주입된 endpoint 프록시, ADR-152). memfs가 없으면
  만든다(M13은 O_CREAT를 항상 암묵적으로 가정 — 플래그 필드는 이후
  라운드).
- 응답: `label=1`, `regs[0]`=`open_file_id`(memfs가 발급, 0=실패),
  `regs[1]`=상태(0=성공, 그 외는 실패), `handle_count=1`,
  `handles[0]`=memfs endpoint에 대한 프록시(`CAN_SEND`) — ADR-018의
  "VFS가 FS 서버 핸들을 위임" 그대로. 클라이언트는 이 핸들을 받아
  이후 `OP_WRITE`/`OP_READ`를 **memfs에게 직접** 보낸다(VFS를 다시
  거치지 않음).

### 2.2. `OP_WRITE` (클라이언트 → FS 서버 직접)

- 요청: `label=2`, `regs[0]`=`open_file_id`, `regs[1]`=길이(바이트,
  0~16), `regs[2..3]`=데이터(최대 16바이트, 길이만큼만 유효).
- 응답: `label=2`, `regs[0]`=실제로 쓴 바이트 수, `regs[1]`=상태.
- M13의 memfs 구현은 항상 파일 오프셋 0부터 덮어쓴다 — seek/append는
  이후 라운드.

### 2.3. `OP_READ` (클라이언트 → FS 서버 직접)

- 요청: `label=3`, `regs[0]`=`open_file_id`, `regs[1]`=요청 길이
  (0~16).
- 응답: `label=3`, `regs[0]`=실제로 읽은 바이트 수, `regs[1]`=상태,
  `regs[2..3]`=데이터(읽은 바이트 수만큼만 유효).
- M13의 memfs 구현은 항상 파일 오프셋 0부터 읽는다.

## 3. 상태 코드 (`regs[1]`, 모든 응답 공통)

```
0 = OK
1 = NOT_FOUND      (OP_WRITE/OP_READ에 알 수 없는 open_file_id)
2 = TOO_LARGE       (요청/데이터 길이가 16바이트 초과)
3 = NO_SPACE        (memfs의 고정 파일 슬롯이 가득 참)
```

## 4. M13이 명시적으로 다루지 않는 것

- `close`(fd 닫기) — M13의 클라이언트는 프로세스 종료까지 파일을
  열어 둔 채로 둔다.
- 공유 모드/잠금(ADR-101), `fs_node_id`(ADR-128)를 응답에 싣는 것 —
  memfs 내부적으로는 파일마다 슬롯 인덱스가 있지만 아직 프로토콜에
  노출하지 않는다.
- 디렉터리 나열, 경로 계층(VFS는 모든 경로를 memfs 루트의 평평한
  이름 하나로 취급한다 — 예: `/test.txt`도 `test.txt`도 같은 파일을
  가리키지 않을 수 있다, 정확한 정규화 규칙은 이후 라운드).
- 파일 크기 제한을 넘는 쓰기(memfs 슬롯 하나는 고정 4096바이트).
