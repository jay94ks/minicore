# FS 서버 공통 프로토콜 스펙 (M18 v3)

**관련 결정**: [filesystem.md](../design/filesystem.md) ADR-018(fd 라우팅),
ADR-047(synthetic FS 서버), ADR-101(공유 모드/잠금), ADR-128(`fs_node_id`),
ADR-162(v2), ADR-168(v3 — 이 문서), [kernel-ipc-objects.md](../design/kernel-ipc-objects.md)
ADR-151(IPC의 cross-address-space 확장), ADR-155/159/161(pages[]의
유저 프로세스 매핑 경로), [security-model.md](../design/security-model.md)
ADR-167(M18 범위 좁힘 — 이 문서의 신원 필드가 실제로 쓰이는 곳)
**관련 스펙**: [ipc.md](ipc.md)(Call/Reply, message 구조), [procsrv.md](procsrv.md)
§4.1(`open_file_id`)

v2(M16)는 `OP_READ`만 `pages[]` 기반으로 바꿨다. 이 문서(v3, M18)는
`OP_WRITE`도 대칭으로 `pages[]` 기반으로 바꾸고(su/sudo 로더가 procsrv
자신의 ELF를 VFS에 실제로 써야 한다), `OP_OPEN`에 호출자 신원 필드를
추가한다(guest/jail의 홈 밖 접근 거부, ADR-167 §결정2).

## 1. 메시지 인코딩

- **`OP_OPEN`의 경로 예산이 32→24바이트로 줄었다**(`regs[0..2]`) —
  남은 `regs[3]`에 호출자 신원(§2.1)을 싣는다.
- **`OP_WRITE`도 이제 `pages[]`를 쓴다**(§2.2) — M13의 16바이트
  `regs[2..3]` 상한은 사라진다.
- `OP_READ`는 v2와 와이어 포맷이 동일하다(§2.3) — 달라진 것은 memfs
  내부의 읽기 위치가 이제 **커서**(오픈 인스턴스마다 유지, 자동
  전진)를 쓴다는 점뿐, 요청/응답 필드는 그대로다.
- FAT32/ext4는 여전히 읽기전용(`OP_WRITE` 미구현)이고, `OP_READ`도
  항상 오프셋 0부터 1페이지만 읽는다(커서 개념 없음) — 호스트가
  미리 만든 작은 테스트 픽스처만 다루므로 필요 없다.

## 2. 오퍼레이션

| `label` | 이름 | 방향 | 의미 |
|---|---|---|---|
| `1` | `OP_OPEN` | 클라이언트→VFS | 경로를 열거나(마운트 테이블로 대상 FS 서버 결정) 만든다 |
| `2` | `OP_WRITE` | 클라이언트→FS 서버(직접) | 열린 파일의 쓰기 커서 위치에 최대 1페이지를 쓰고 커서를 전진시킨다(memfs만 지원) |
| `3` | `OP_READ` | 클라이언트→FS 서버(직접) | 열린 파일의 읽기 커서 위치에서 최대 1페이지를 읽고 커서를 전진시킨다 |

### 2.1. `OP_OPEN` (클라이언트 → VFS)

- 요청: `label=1`, `regs[0..2]` = 경로(ASCII, NUL 패딩, 최대
  24바이트). `regs[3]` = 호출자 신원 — `bit0`=guest, `bit1`=jail,
  `bits[2:33]`=uid(이 라운드는 검사에 안 쓰지만 자리를 마련해 둔다).
  **이 신원은 호출자가 스스로 채우는 자기 선언**이다 — 커널이
  위조를 막지 않는다(badge 기반 강제는 아직 없다, OPEN-38 참고).
  `page_count=0`, `handle_count=0`.
- 처리: VFS는 마운트 테이블(v2와 동일, `/mnt/fat32/`→fat32,
  `/mnt/ext4/`→ext4, 그 외→memfs)로 대상 FS 서버를 고르기 **전에**,
  `regs[3]`의 guest/jail 비트가 켜져 있으면 경로가 `/home/`으로
  시작하는지 검사한다 — 시작하지 않으면 어떤 FS 서버에도 전달하지
  않고 즉시 `GUEST_DENIED`(§3)로 응답한다. super 신원 검사는 이
  라운드에서 아직 구현하지 않는다(ADR-081 §결정3의 "super는 이
  제약을 안 받는다"는 uid를 확인해야 하는데, 지금은 guest/jail
  비트만으로 충분한 시나리오만 검증한다).
- 응답: `label=1`, `regs[0]`=대상 FS 서버가 발급한 `open_file_id`
  (0=실패), `regs[1]`=상태, `handle_count=1`, `handles[0]`=대상 FS
  서버 endpoint에 대한 프록시(`CAN_SEND`) — 거부(`GUEST_DENIED`)면
  `handle_count=0`.

### 2.2. `OP_WRITE` (클라이언트 → FS 서버 직접, memfs만) — v3: `pages[]` 요청

- 요청: `label=2`, `regs[0]`=`open_file_id`, `regs[1]`=길이
  (≤4096으로 클램프), `page_count=1`, `pages[0]`=쓸 내용(발신자가
  페이지 정렬된 자기 버퍼에 준비 — ADR-155 §2의 §2 경로, 발신자가
  실제 유저 프로세스여야 한다).
- 처리: memfs는 그 오픈 인스턴스의 `write_cursor`(open 시 0으로
  시작) 위치부터 `regs[1]`바이트를 덮어쓰고, 파일 크기가 그 지점까지
  못 미쳤으면 늘리고, `write_cursor`를 그만큼 전진시킨다. 여러 번
  연속으로 호출하면 파일 앞에서부터 순차적으로 이어 쓴다(seek 없음).
- 응답: `label=2`, `regs[0]`=실제로 쓴 바이트 수, `regs[1]`=상태.
  FAT32/ext4에게 보내면 `NOT_FOUND`(지원 안 함)로 응답한다.

### 2.3. `OP_READ` (클라이언트 → FS 서버 직접) — v2와 와이어 동일, memfs는 커서 사용

- 요청: `label=3`, `regs[0]`=`open_file_id`, `regs[1]`=요청 길이
  (≤4096으로 클램프).
- 처리(memfs): 그 오픈 인스턴스의 `read_cursor`(open 시 0으로
  시작) 위치부터 최대 `regs[1]`바이트(파일 끝을 넘지 않게)를 읽고
  `read_cursor`를 그만큼 전진시킨다. 여러 번 연속으로 호출하면
  파일 앞에서부터 순차적으로 이어 읽는다.
- 처리(FAT32/ext4): v2와 동일 — 항상 파일 오프셋 0부터, 커서 없음.
- 응답: `label=3`, `regs[0]`=실제로 읽은 바이트 수(≤4096),
  `regs[1]`=상태, `page_count=1`, `pages[0]`=커널이 매핑해 준
  페이지(`vaddr`는 출력, `length=4096`, 앞 `regs[0]`바이트만
  유효하고 나머지는 0).
- **알려진 제약**(v2와 동일, ADR-155 §3): FS 서버가 응답 버퍼를
  다음 요청 전에 재사용하면 그 사이 수신자가 못 다 읽은 내용이
  바뀔 수 있다 — 이 문서의 모든 FS 서버가 "한 요청을 다 처리하고
  회신한 뒤에야 다음 요청을 받는" 단일 스레드 루프라 실제로 부딫힐
  상황이 아니다.

## 3. 상태 코드 (`regs[1]`, 모든 응답 공통)

```
0 = OK
1 = NOT_FOUND      (알 수 없는 open_file_id, 또는 그 FS 서버가 지원하지 않는 오퍼레이션)
2 = TOO_LARGE       (v3 이전 규약의 잔재 — 지금은 OP_WRITE/OP_READ 모두 내부 클램프라 이 코드를 안 씀)
3 = NO_SPACE        (memfs의 고정 파일 슬롯이 가득 참)
4 = MOUNT_ERROR      (FAT32/ext4가 마운트에 실패함 — 잘못된 시그니처, 지원 안 하는 feature 비트 등)
5 = GUEST_DENIED     (호출자가 guest/jail이고 경로가 /home/ 밖 — ADR-167 §결정2)
```

## 4. 이 문서가 명시적으로 다루지 않는 것

- `close`(fd 닫기), 공유 모드/잠금(ADR-101), `fs_node_id`(ADR-128)를
  응답에 싣는 것 — M13부터 동일하게 이후 라운드.
- 디렉터리 나열, 하위 디렉터리 진입(FAT32/ext4는 v1 그대로 **루트
  디렉터리 평평한 스캔만**, memfs는 원래부터 평평한 이름공간).
- 임의 위치로 이동하는 명시적 `seek` — memfs의 커서는 항상
  0에서 시작해 순차적으로만 전진한다(§2.2/§2.3). 필요해지면 별도
  seek 오퍼레이션을 그때 추가한다(YAGNI, ADR-168 §근거).
- FAT32/ext4에 대한 쓰기 — 두 FS 모두 v1은 읽기전용(ADR-057/129).
- **guest/jail 신원의 위조 방지** — `OP_OPEN`의 신원 필드는 호출자
  자기 선언이며 커널이 강제하지 않는다(ADR-084의 badge 기반 신원
  전파는 아직 구현되지 않았다) → **OPEN-38**이 이미 지적해 둔
  근본 제약 그대로.
- 동적 마운트/언마운트 syscall — 마운트 테이블은 여전히 VFS 코드에
  정적으로 박혀 있다(cfgsrv 등장 이후로 미룸).
