# 완료 보고: system-servers-bringup M18 — 보안 모델: 신뢰 위임 체인 + su/sudo + jail/guest

**대상 계획**: [system-servers-bringup.md](../plan/system-servers-bringup.md) §M18
**관련 결정**: [security-model.md](../design/security-model.md) ADR-167
(M18 범위 좁힘), [filesystem.md](../design/filesystem.md) ADR-168
(`fs-protocol.md` v3)
**실행일**: 2026-09-09

## 완료한 것

### D0. 선행 스코핑 — ADR-167/168

cfgsrv(M19)가 아직 없어 ADR-093의 위임 테이블을 procsrv에 하드코딩
했고, ADR-080/081의 jail 오버레이 네임스페이스가 아직 없어 jail을
이 라운드에 한해 guest와 같은 "홈(`/home/`) 밖 거부" 규칙으로
단순화했다. su/sudo의 "호출자가 지정한 명령" 실행은 사용자가 명시적
으로 더 어려운 쪽을 선택해, 임의 명령이 아니라 **실제 경로→ELF
로더**로 검증했다.

### D1. `fs-protocol.md` v3 — `OP_WRITE`도 `pages[]` 기반, memfs 커서, `OP_OPEN` 신원 필드

M16이 `OP_READ`만 바꿨던 것을 대칭으로 마무리했다 — `OP_WRITE`가
이제 `pages[]`로 최대 1페이지를 받는다. memfs의 오픈 인스턴스마다
`read_cursor`/`write_cursor`를 둬서, 여러 번의 `OP_WRITE`(또는
`OP_READ`)를 연속으로 호출하면 파일 앞에서부터 순차적으로 이어
쓰거나 읽는다(오프셋 필드를 와이어에 추가하지 않고 서버 쪽 상태로
처리) — su/sudo 로더가 procsrv 자신의 ELF(수십 KiB)를 여러 페이지로
나눠 쓰고 다시 읽어야 해서 필요했다. `memfs::k_max_file_bytes`를
4096→131072로 늘렸다. `OP_OPEN`의 경로 예산이 32→24바이트로 줄고,
남은 필드에 호출자가 스스로 밝히는 신원(guest/jail 비트)이 실린다
— 이 신원은 자기 선언이며 커널이 강제하지 않는다(OPEN-38이 이미
지적한 제약).

### D2. `servers/vfs` — guest/jail 홈 격리 검사

마운트 테이블을 찾기 전에 호출자가 guest 또는 jail이면 경로가
`/home/`으로 시작하는지 검사한다 — 아니면 어떤 FS 서버에도 전달하지
않고 `GUEST_DENIED`(상태 코드 5)로 즉시 응답한다.

### D3. `servers/procsrv` — 계정 모델 확장 + 위임 테이블 + `OP_SU` + 경로→ELF 로더

계정마다 uid/S·G·J 비트를 채웠다(test=1000, root=0/super,
guest1=2000/guest). 하드코딩 위임 테이블(root→test)과 `OP_SU`
핸들러를 추가했다 — 위임이 있으면 비밀번호 없이 승인, 없으면
대상 계정의 비밀번호를 요구, 대상이 guest/jail이면 위임을 무시하고
항상 비밀번호를 요구한다(ADR-093 §결정7). **경로→ELF 로더**를
실제로 구현했다 — procsrv 자신의 ELF 바이트(M12 self_info 브릿지)를
`/bin/su-target`에 여러 페이지로 나눠 쓰고, 다시 그 경로에서 읽어
재조립한 바이트가 원본과 정확히 일치함을 확인한 뒤, **그 재조립된
바이트**로 `sys_process_spawn`한다. 스폰되는 프로세스는 procsrv와
같은 바이너리이지만 magic 접두사(`0x53555354`)가 붙은 argv로
"su-target 역할을 하라"고 구분해 받는다 — 신원을 로그로 남기고,
guest/jail이면 홈 밖(거부 기대)/홈 안(성공 기대) `open`을 둘 다
시도해 결과를 로그로 남긴다.

### D4. `servers/login` — su/sudo 정책 확인 경로 양쪽 검증

로그인 성공 후 `OP_SU`를 두 번 호출한다: `test→root`(위임 있음,
비밀번호 없이 승인 기대)와 `guest1→test`(위임 없음, 잘못된 비밀번호
로 거부 기대).

## 검증 결과 (QEMU 실측)

```bash
cmake --build build/x86_64-clang --target minicore_kernel_x86_64 minicore_bootdisk_image
tools/smoke-test-x86_64.sh       # 71개 전부 PASS(M17의 66개 + 신규 5개) — 2회 연속 재확인, fat32/ext4 이미지 해시 불변
tools/smoke-test-smp-x86_64.sh   # 11개 전부 PASS(회귀 없음)
tools/smoke-test-numa-x86_64.sh  # 24개 전부 PASS(회귀 없음)
tools/smoke-test-avx-x86_64.sh   # 12개 전부 PASS(회귀 없음)
```

실측 로그(발췌):

```
[procsrv] loader roundtrip ok=1
[process] spawn ok entry=0x10000000 trusted=0
[su-target] uid=2000 super=0 guest=1
[su-target] guest open outside denied=1
[su-target] guest open inside ok=1
[login] auth ok=1
[process] spawn ok entry=0x10000000 trusted=0
[su-target] uid=0 super=1 guest=0
[login] su delegated ok=1
[login] su denied ok=1
```

- **확인함**: 경로→ELF 로더가 procsrv 자신의 ~40KiB ELF를 VFS에
  실제로 쓰고 다시 읽어 재조립한 바이트가 원본과 정확히 일치함 —
  fs-protocol.md v3의 커서 기반 `OP_WRITE`/`OP_READ`가 여러 페이지에
  걸친 순차 접근에서 올바르게 동작함.
- **확인함**: guest 신원의 프로세스가 VFS에서 홈 밖 접근은 거부되고
  홈 안 접근은 성공함 — jail을 guest와 같은 규칙으로 단순화한
  ADR-167 §결정2가 실제로 동작함.
- **확인함**: procsrv의 위임 테이블이 있으면 비밀번호 없이 su가
  승인되고, 없고 잘못된 비밀번호면 거부됨 — ADR-093의 탈중앙화
  위임 모델의 핵심 동작(위임 확인 → 없으면 자격증명 요구)이 최소
  구현으로 성립함.
- **이번 라운드는 실행 중 새로 발견한 버그가 없었다** — M14~M17이
  매번 최소 하나씩 겪었던 것과 달리, 이번 구현은 첫 전체 QEMU
  부팅에서 바로 71/71 통과했다(memfs 페이지 정렬 버퍼 처리, VFS의
  24바이트 경로 절단, su-target argv 판별, 핸들 번호 가정 등을
  구현 중 코드 리뷰 단계에서 미리 걸러냈다 — 특히 `spawn_su_target`
  의 `create_endpoint` 플래그를 잘못 `false`로 뒀다가 핸들 번호가
  하나씩 밀리는 문제를 빌드 전에 발견해 고쳤다).
- **알려진 단순화(M18 범위 밖으로 명시적으로 남긴 것)**: jail의
  실제 오버레이 네임스페이스(ADR-080/081, 지금은 guest와 같은 규칙),
  위임 테이블의 cfgsrv 레지스트리 저장(M19 이후), 호출자가 IPC로
  임의 경로를 지정하는 일반 su/sudo 명령 실행, guest/jail 신원의
  badge 기반 위조 방지(OPEN-38), `/sys/proc`·`/sys/dev` 가시성
  필터링(OPEN-37, synthetic 마운트가 아직 없다).

## 다음 단계

M18은 이것으로 완료됐다. 다음 마일스톤은
[system-servers-bringup.md](../plan/system-servers-bringup.md) §M19
(cfgsrv 설정 리포지터리).
