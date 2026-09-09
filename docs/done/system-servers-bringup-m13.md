# 완료 보고: system-servers-bringup M13 — VFS + memfs + fd 라우팅 실동작

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M13
**관련 결정**: [kernel-ipc-objects.md](../design/kernel-ipc-objects.md) ADR-151,
[kernel-memory.md](../design/kernel-memory.md) ADR-152,
[filesystem.md](../design/filesystem.md) ADR-153
**관련 스펙**: [fs-protocol.md](../spec/fs-protocol.md)(신규)
**실행일**: 2026-09-09

## 완료한 것

### C1. IPC를 실제 cross-process로 확장 (ADR-151)

`kernel/core/ipc/endpoint.cpp`의 `deliver_message()`가 메시지 구조체
(label/regs/page_count/handle_count)를 그 메시지가 있는 스레드의
주소공간으로 번역해서 읽고 쓴다(`arch_translate_user_page` 훅, ADR-002
HAL 경계 유지). `sys_reply`가 이제 `handles[]`도 옮긴다(ADR-018의
open() 응답 방향 요구사항). 새 syscall `sys_ipc_recv`/`sys_ipc_reply`
를 유저모드에 노출(M6~M12는 IPC 수신측이 전부 커널 스레드였다).

### C2. `sys_process_spawn` 스폰 시점 캐패빌리티 주입 (ADR-152)

`create_endpoint`(새 프로세스 handle 1에 endpoint 자동 생성 + 호출자
테이블에 프록시 반환) + `inherited_handles`(호출자가 이미 가진 핸들을
새 프로세스 테이블에 미리 심음) — 등록/탐색 서비스가 없는 상태에서
독립적으로 스폰된 프로세스들이 서로의 endpoint를 알아내는 유일한
방법.

### C3. `docs/spec/fs-protocol.md` 최소 버전 + vfs/memfs 서버 (ADR-153)

`OP_OPEN`/`OP_WRITE`/`OP_READ` 3개 오퍼레이션, 전부 `regs[]`만 사용
(pages[]의 cross-process 내용 전달은 아직 미구현, OPEN-59). `servers/
fs/memfs`(고정 8개 파일 슬롯, 평평한 이름공간) + `servers/vfs`(마운트
테이블 없이 전부 memfs로 위임, open() 응답에 memfs 핸들 위임).

### C4. procsrv를 VFS 클라이언트로 확장 + 부트 디스크 의존성 배선

`servers/procsrv/main.cpp`가 자기 자신 fork/exec 데모 뒤 vfs에
`open("test.txt")`→위임받은 memfs 핸들로 직접 `write("hello vfs")`→
`read()`→내용 일치 확인까지 수행한다. `tools/mkbootdisk.py`에
`--depends=이름:의존이름` 추가, `init/initrun/main.cpp`의 스폰 루프가
스폰한 서비스의 endpoint 프록시를 이름으로 기록해 두고 다음 서비스의
`depends=`에 물려준다. `servers/CMakeLists.txt`가 memfs→vfs→procsrv
순서로 부트 디스크를 조립한다.

### C5. `sys_debug_log` syscall

klog가 유저에 노출된 적이 없어, procsrv가 라운드트립 결과를 관찰
가능하게 만드는 최소 수단(순수 진단용).

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh       # 52개 전부 PASS(M12의 51개 + 신규 1개)
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌, 순서 그대로):

```
[process] spawn ok entry=0x10000000 trusted=0   ← memfs
[process] spawn ok entry=0x10000000 trusted=0   ← vfs
[process] spawn ok entry=0x10000000 trusted=0   ← procsrv
[process] fork ok child_pml4=...                ← procsrv 자기 자신
[process] exec ok entry=0x10000000              ← procsrv 자기 자신
[procsrv] vfs write/read roundtrip ok=1         ← M13 검증 목표
```

- **확인함**: procsrv가 VFS 경유로 memfs에 파일을 쓰고 다시 읽어
  내용이 일치함(M13의 최종 QEMU 검증 목표) — 세 개의 독립적으로
  스폰된 유저 프로세스가 IPC로 실제 협력하는 첫 사례.
- **확인함**: `sys_reply`의 `handles[]` 위임(ADR-018)이 실제로
  서로 다른 프로세스 사이에서 동작함(vfs→procsrv).
- **확인하지 못함**: VFS의 실제 마운트 테이블/경로 탐색, 공유 모드/
  잠금, `fs_node_id` 노출, 디렉터리 나열 — 전부 M13 범위 밖(ADR-153
  §알려진 단순화 참고).

## 다음 단계

M13은 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M14
(devmgr + PCIe 버스 열거 + 입력 장치). 새로 열린 미결정 항목은
[open-items.md](../design/open-items.md) OPEN-59(pages[]의 진짜
cross-process 전달, 서비스 간 이름 탐색을 스폰 시점 주입이 아닌
정식 메커니즘으로 대체) — 실제로 필요해지는 시점에 다시 연다.
