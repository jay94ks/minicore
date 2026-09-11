# M56 완료 — coreutils(echo/ls/cat)+`[`을 별도 ELF 대신 msh 빌트인으로 흡수

[musl-userland-porting.md](../plan/musl-userland-porting.md) §M56
(M51~M55 완료 뒤 사용자 지시로 추가). 관련 ADR:
[ADR-228](../design/foundations.md)(유저랜드 재설계),
[ADR-229](../design/kernel-ipc-objects.md)(실행 중 발견한 커널 버그).

## 무엇을 했는가

ADR-221(M52)이 세운 "명령은 항상 별도 ELF로 fork+exec한다"는 원칙을
사용자 지시로 뒤집었다 — "독립된 ls/cat/`[` 같은 동작이 자체
바이너리를 갖지 않고 msh 하나의 ELF가 모두 처리하도록 바꾸자."
사전 확인(진짜 빌트인 vs 멀티콜 바이너리, 범위, `[` 추가 여부)에
대한 답변을 그대로 반영했다:

- **`userland/echo`/`userland/ls`/`userland/cat` 완전 삭제** — 로직을
  msh 자신의 함수(`builtin_echo`/`builtin_ls`/`builtin_cat`)로
  옮겼다. `servers/procsrv`의 VFS 시딩(`run_coreutils_seed()`)도
  함께 없앴다.
- **새 빌트인 `[`**(`builtin_test`) — POSIX test의 아주 좁은
  부분집합: `=`/`!=`(문자열), `-eq`/`-ne`/`-lt`/`-le`/`-gt`/`-ge`
  (정수), `-z`/`-n`(빈 문자열), 단항 문자열 진위. `-f`/`-d`는
  일부러 뺐다(아래 "실행 중 발견" 참고).
- **파이프라인은 real musl pthread로 동시 실행** —
  `pthread_create`/`pthread_join`(M37)을 그대로 쓴다("병렬 실행을
  흉내낸다"는 요구를 코루틴을 새로 고안하지 않고 이미 있는 진짜
  스레드로 충족). pthread는 `owner_space`/`handle_table`을 msh와
  그대로 공유하므로(ADR-212) 빌트인 스레드가 msh 자신의 handle
  2(vfs)/4(pipesrv)를 별도 설정 없이 바로 쓴다.
- **빌트인은 M54의 fd 번호 계층을 완전히 우회** — 여러 pthread가
  같은 handle_table/BSS를 공유하는 상황에서 "지금 이 스레드의 fd
  1" 같은 전역 슬롯 하나로 서로 다른 파이프라인 단계를 표현하면
  충돌한다 — 그래서 raw `pipe_id`/`{fs_handle, open_file_id}`를
  함수 인자로 직접 받아 `mc_pipe_create/read/write/close`+
  `mc_vfs_open`/`mc_fs_read`/`mc_fs_write`를 직접 부른다.
  `run_pipeline()`도 `pipe()` 대신 `mc_pipe_create()`를 직접 불러
  raw id를 얻는다 — msh 자신의 fd 테이블에 파이프 항목을 전혀
  등록하지 않으므로 M54가 겪은 "fork()의 자동 dup" 문제도 이
  경로엔 없다. 알려지지 않은 명령(`loop-test`뿐)은 여전히 기존
  `fork()`+`execve()`(`spawn_stage()`, 변경 없음)로 돈다 — 셸의
  일반성은 유지된다.

## 실행 중 발견 — 이 프로젝트 역사상 첫 진짜 커널 동시성 버그

빌트인을 pthread로 동시 실행하자마자 `cat`이 파일을 여는 건 항상
성공했는데 그 직후 읽기가 항상 0바이트를 돌려주는 버그를 만났다 —
단일 명령("cat test.txt")에도 나타나 파이프라인 특유의 문제가
아니었다. 원인은 msh 코드가 아니라 **커널 자체**였다:

1. **`kern::object::handle_table`에 락이 전혀 없었다**(OPEN-68이
   M11부터 지적해 뒀던 것). `pthread_create()`가 메인 스레드에서
   "새 스레드를 가리키는 소유 핸들"을 만드는 동안, 방금 만든
   스레드가 다른 코어에서 이미 스케줄돼 자기 `mc_vfs_open()` 응답의
   `handles[]` 위임을 처리하면(ADR-212가 만든 handle_table 공유가
   이 경합 지점을 만든다), `allocate_slot()`의 읽고-나서-쓰는
   비원자적 빈 슬롯 탐색이 두 코어에서 같은 슬롯을 골라 하나가
   다른 하나를 조용히 덮어썼다.
2. **IPC `pages[]` 매핑 슬롯이 프로세스당 고정된 자리 하나뿐이었다.**
   두 pthread가 동시에 서로 다른 응답(하나는 `mc_vfs_open`의
   handles[] 위임, 다른 하나는 `mc_fs_read`의 페이로드)을 받으면
   똑같은 물리 슬롯에 두 번 매핑을 시도해 나중 것이 먼저 것을
   지웠다.

memfs 서버 쪽에 임시 진단(`file_index`/`file_size`/`read_cursor`를
직접 로그로 남김)을 추가해 memfs 자신은 항상 올바른 계산(`size=9`,
`read_cursor=0` → 9바이트 반환)을 하고 있다는 것을 먼저 확인한
뒤에야, 문제가 클라이언트(msh)도 서버(memfs)도 아니라 그 사이
**커널의 응답 전달 경로**에 있다는 것을 좁혀냈다.

**수정**: (1) `handle_table`의 모든 변경/조회 진입점을 단일 전역
스핀락으로 감싼다(테이블별 락은 `create_proxy`가 다루는 두 테이블+
`close`의 cascade가 다른 프로세스 테이블까지 건너는 것 때문에 락
순서 문제가 생겨, 짧은 배열 연산이라는 특성상 전역 락 하나로 묶는
게 더 안전하다고 판단했다). (2) IPC pages[] 매핑 슬롯(`kernel-memory.md`
ADR-160 슬롯 4, 1MiB 예산)을 스레드별 부분 슬롯(16KiB × 최대 64
스레드 = 1MiB, 정확히 맞아떨어진다)으로 나눈다 — 새 필드
`thread::ipc_pages_slot_index`를 스레드 생성 시점(`create_user_thread`/
`create_forked_thread`/`sys_exec`)에 `address_space::next_ipc_pages_slot`
카운터에서 배정한다. 자세한 설계는 [ADR-229](../design/kernel-ipc-objects.md)
참고 — OPEN-68을 해소한다.

## 검증 (QEMU, x86_64)

핵심 확인 로그:

```
[msh] running: ls
[msh] running: cat
sys/etc/registry.dat
test.txt
musl-exec-target.elf
bin/loop-test
bin/su-target
home/guest1/allowed.txt
[msh] running: [
[msh] running: [
[msh] self-test done ok=1
```

수정 전에는 "cat test.txt" 단독 호출조차 0바이트를 반환했다 —
수정 후 memfs 서버 쪽 진단으로 정확한 파일/크기/커서, 클라이언트
쪽으로 정확한 바이트 수(9, 18)를 직접 확인했다. `tools/
smoke-test-x86_64.sh`의 msh 관련 어서션을 갱신(빌트인 로그 형식
변경 — "@pipefd"/"@filefd" 토큰이 더 이상 안 보인다, `cat test.txt`
로 대상 변경). 5개 회귀 스위트 전부 PASS(exit 0, FAIL 없음): 스모크
147, SMP 11, NUMA 24, AVX 12, net 6.

## 범위 밖으로 남긴 것

- `[`의 `-f`/`-d`(파일 존재 검사) — memfs의 open-always-creates
  관례 때문에 이 프로토콜로는 부작용 없이 구현할 수 없다.
- `test`라는 별명(사용자가 `[`만 요청했다).
- 64개를 넘는 스레드가 IPC 매핑 슬롯 예약 범위를 넘어설 수 있다는
  것을 이 라운드는 명시적으로 검사하지 않는다(ADR-229 참고).

## 다음

musl-userland-porting.md(M51~M56)가 이제 전부 완료됐다 — 이 계획에는
더 이상 다음 마일스톤이 없다.
