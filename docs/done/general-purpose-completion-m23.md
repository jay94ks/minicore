# 완료 보고: general-purpose-completion M23 — 진짜 fork/exec (fd 상속)

**대상 계획**: [general-purpose-completion.md](../plan/general-purpose-completion.md) §M23
**관련 결정**: [kernel-memory.md](../design/kernel-memory.md) ADR-179
(sys_fork가 handle_table 전체를 프록시로 복제)
**실행일**: 2026-09-10

## 완료한 것

### D0. 선행 스코핑

ADR-008(foundations.md)이 "가장 어려운 문제"로 지적한 procsrv.md
§3.6/§4.1의 완전한 fd 진실 공급원 프로토콜(procsrv가 각 fd의 소유
서버에 IPC로 "복제해 달라"고 요청하는 `dup_for_new_client`)은 이번
라운드에도 구현하지 않는다. 대신 커널 레벨에서 `sys_fork`가
`handle_table` 전체를 그대로 복제하는 훨씬 단순한 방법으로 M23의
실제 검증 목표("셸이 파일을 열어 둔 채 fork해, 자식이 상속받은 fd로
다른 exec 이미지에서 계속 읽을 수 있다")를 충족했다 — 이게 가능한
이유는 FS 서버(memfs)가 "열린 파일" 상태를 커널 핸들·발신자 신원과
무관하게 `open_file_id`로만 관리하기 때문이다. 자세한 근거는
ADR-179(kernel-memory.md) 참고.

### D1. 커널 — `sys_fork`가 handle_table을 실제로 복제

[process_ops.cpp::fork_current()](../../kernel/arch/x86_64/process_ops.cpp)
가 자식을 빈 handle_table로 시작시키던 것을, 부모의 handle_table을
순회해(`handle_table::debug_entry()`) 유효한 항목마다
`create_proxy(h, e->rights, *child_handles, 0, false)`로 그대로
복제하도록 고쳤다 — rights 축소 없음, badge도 부모 값 그대로
상속(`create_proxy` 자체가 이미 지원하는 동작). 이전에는(M12~M22)
자식이 완전히 빈 테이블로 시작해, fork만으로는 자식이 부모가 열어
둔 어떤 서버와도 통신할 방법이 없었다.

### D2. procsrv 자기테스트 — fork+exec를 넘어 fd가 살아남는지 검증

[servers/procsrv/main.cpp](../../servers/procsrv/main.cpp)에
`fd_continue_argv`(su-target/wait-target/kill-target과 같은 argv
마커 관례)를 추가했다. `run_fd_inheritance_test()`가:

1. `test.txt`("hello vfs", `run_vfs_roundtrip_test()`가 이미 만들어
   둠)를 **새 open_file_id로** 다시 열어 앞 5바이트("hello")만 읽어
   서버 쪽 `read_cursor`를 5로 옮긴다.
2. `sys_fork`한다.
3. 자식은 상속받은 memfs handle+open_file_id를 argv로 그대로
   실어(exec는 handle_table을 건드리지 않으므로 값 자체는 이미
   유효하다) **완전히 새 ELF 이미지로 exec**한다.
4. 그 새 이미지(`run_as_fd_continue_target`)가 같은 handle+
   open_file_id로 나머지 4바이트(" vfs")를 이어 읽어 정확히
   일치하는지 확인한다(`"[procsrv] fd inherited continue read
   ok=1"`).

QEMU 실측 첫 시도에 바로 통과했다(추가 디버깅 불필요) — M21/M22와
달리 이번 라운드는 설계 단계에서 memfs의 `open_file_id` 키 구조를
먼저 확인해 두어서, 구현 자체는 단순 커널 수정 하나로 끝났다.

### D3. 검증

`tools/smoke-test-x86_64.sh`에 `"[procsrv] fd inherited continue
read ok=1"`를 추가했다. 4개 QEMU 스위트 전부 재확인:

| 스위트 | 결과 |
|---|---|
| `tools/smoke-test-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-smp-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-numa-x86_64.sh` | PASS(exit=0, FAIL 0개) |
| `tools/smoke-test-avx-x86_64.sh` | PASS(exit=0, FAIL 0개) |

(M22에서 겪은 "servers/\* 변경 후 `--target minicore_bootdisk_image`를
따로 돌려야 한다"는 함정을 이번에는 처음부터 반영해 재현하지
않았다.)

## 남겨 둔 것 (OPEN)

- procsrv.md §3.6/§4.1의 완전한 fd 진실 공급원 프로토콜
  (`dup_for_new_client`, 발신자 신원별 접근 제한)은 여전히 없다 —
  ADR-179의 "알려진 단순화" 참고, OPEN-64(ADR-178)와 같은 갭이다.
  새 항목을 추가로 열지 않고 기존 OPEN-64에 편입한다(같은 근본
  원인 — "완전한 프로세스/fd 프로토콜 부재").
- `close-on-exec` 표시가 여전히 없다(procsrv.md §10 기존 미결 항목).

## 다음

[general-purpose-completion.md](../plan/general-purpose-completion.md)
§M24(유저랜드 동적 메모리)로 이어간다.
