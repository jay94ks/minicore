# 미결정 항목 (OPEN)

[← 설계 문서 색인](index.md)

모든 카테고리 파일에 흩어진 미결정 항목을 한 곳에 모은 문서. 새
미결정 항목은 다음 번호(`OPEN-53`부터)로 여기에 추가하고, 해당
결정을 다루는 카테고리 파일에도 같은 번호로 언급한다. 해결되면
`~~OPEN-N~~`으로 취소선 처리하고 해결한 ADR 번호를 적는다.

## 현재 열려있는 항목

| ID | 내용 | 관련 ADR | 다루는 문서 |
|---|---|---|---|
| OPEN-32 | result/optional의 `[[nodiscard]]` 강제 여부 최종 확정 시점 | ADR-010, ADR-068, ADR-069 | [libk.md](libk.md) |
| OPEN-42 | 위임의 세부 범위(특정 명령만 허용, 특정 시간대만 허용 등) 지원 여부 — 기간/영구성은 ADR-096, 재인증 요구 여부는 ADR-112로 이미 해결됨 | ADR-093, ADR-096, ADR-112 | [security-model.md](security-model.md) |
| OPEN-51 | initrun이 모든 서비스 기동 후 마지막으로 실행하는 "systemd류 초기 프로세스" — procsrv 등 코어 서버가 아닌 **별도의 유저랜드 서비스 관리자 데몬**(정체성 확정, Linux systemd에 대응)의 실제 이름·책임 범위와, 프로세스 트리의 새 루트가 되는 구체적 절차 — **별도 design 문서 대상**(OPEN-52와 같은 성격) | ADR-131 | [boot-and-drivers.md](boot-and-drivers.md) |
| OPEN-52 | 프로세스/서비스의 "초기화 완료(준비됨)" 신호 프로토콜 — ADR-131 범위 밖으로 분리(OPEN-49 해소), 별도 설계·계획 문서가 필요(아직 미착수) | (미정) | (신규 design 문서 예정) |
| OPEN-54 | procsrv의 실제 IPC 와이어 프로토콜(메시지 레이아웃, 각 오퍼레이션의 정확한 label/파라미터) — [procsrv.md](../spec/procsrv.md)가 개념적 절차(프로세스 테이블, fork/exec 시퀀스, fd 진실 공급원)는 정했지만 바이트 단위 메시지 포맷까지는 확정하지 않았다 | (미정) | [procsrv.md](../spec/procsrv.md) |
| OPEN-60 | ADR-154가 "I/O 활성화 권한 부여"를 지금은 initrun의 하드코딩(스폰 시 grant_trusted)으로만 결정하도록 확정했다 — 부팅 완료 이후 cfgsrv 레지스트리를 읽어 **다른** 특수 프로세스에게도 이 권한을 동적으로 부여/회수하는 절차는 cfgsrv가 실제로 존재하는 M19 이후 재검토 대상 | ADR-154 | [boot-and-drivers.md](boot-and-drivers.md) |

## 해결된 항목 (이력)

| ID | 내용 | 해결 ADR |
|---|---|---|
| ~~OPEN-61~~ | ADR-155 §2의 매핑 해제 시점(다음 sys_recv 직전 자동 해제, 스레드별)과 배치 위치(`k_user_stack_top` 위쪽 고정 슬롯 사다리의 다음 자리) 확정 | ADR-159 |
| ~~OPEN-58~~ | TSS IOPB가 코어당 전역 공유 — 스레드별 활성 I/O 범위(`io_port_base`/`count`) + `sys_io_activate`/`sys_io_deactivate` + 컨텍스트 스위치 시 diff 기반 재프로그래밍으로 설계 확정(구현은 M14 착수 시점) | ADR-154 |
| ~~OPEN-59~~ | IPC `pages[]` 페이로드의 cross-address-space 전달 — 수신자가 커널(스레드)이면 ADR-151과 같은 방식의 페이지 단위 번역, 수신자가 유저 프로세스면 참조 카운트+공유 매핑(유저에게 번역 API를 노출하지 않음)으로 설계 확정(구현은 §1은 M14, §2는 M16 전후) | ADR-155 |
| ~~OPEN-55~~ | 서비스 준비완료 신호의 M12 임시방편 — 아무 신호도 두지 않는다(M12는 서비스가 procsrv 하나뿐이라 순서 대기 자체가 불필요, 게다가 sys_yield가 없어 initrun이 "기다렸다 계속"할 수단이 없다) | ADR-150 |
| ~~OPEN-56~~ | procsrv.md §5/7/8의 M12 "프로토콜 골격" 경계 — 코드상 자리조차 만들지 않는다(핸들러 스텁도, 라우팅도, 항상-실패 응답도 없음) | ADR-150 |
| ~~OPEN-57~~ | virtio-blk 클라이언트·cpio/INI 파서·mkbootdisk.py의 구현 순서와 범위 — virtio_blk(레지스터 프로토콜)을 가장 먼저, 이후 cpio/ini 재사용→mkbootdisk.py→initrun 통합→procsrv 골격 순으로 진행, 큐 협상은 feature 0개/큐 1개/요청 1개/순수 폴링까지만 단순화 | ADR-150 |
| ~~OPEN-53~~ | `sys_process_spawn`/`sys_fork`/`sys_exec`/`sys_thread_exit`의 정확한 시그니처·에러 코드·syscall 번호 — QEMU에서 initrun 자신을 fork/exec/spawn하는 전체 왕복까지 실제 검증 완료 | ADR-142 |
| ~~OPEN-49~~ | initrun이 각 서비스의 초기화 완료를 기다린 뒤 다음으로 넘어간다는 방향은 확정(ADR-131 §결정6) — 정확한 준비완료 신호 프로토콜은 범위가 커서 별도 설계로 분리 | OPEN-52로 이관 |
| ~~OPEN-50~~ | initrun이 부트 파티션에서 서비스 바이너리를 찾는 정확한 탐색 규칙 | ADR-131 |
| ~~OPEN-29~~ | initrun 기동 매니페스트 형식(서버 실행 순서·인자 기술 방법) | ADR-131 |
| ~~OPEN-48~~ | `<type_traits>`/`<concepts>`/`<bit>`/`<limits>`/`<atomic>`/`<utility>`/`<new>` 등 나머지 freestanding C++ 헤더 확보 방법(libc++ 실제 빌드 vs 개별 shim vs libk 대체) | ADR-115 |
| ~~OPEN-47~~ | 스왑 압축·해제 루틴의 정확한 구현 위치(커널 내부 vs SWAPFS 서버) | ADR-110 |
| ~~OPEN-46~~ | "최근 스케줄링 빈도" 측정·기록 방식(스왑 압축 대상 선정용) | ADR-109 |
| ~~OPEN-43~~ | 콘솔 드라이버 상세 프로토콜(문자 버퍼 형식, 프레임버퍼 글리프 렌더링, TTY 전환 키 조합 등) | ADR-111 |
| ~~OPEN-44~~ | SWAPFS 압박 상황이 물리 메모리 할당자·정책 서버 쿼터에 어떻게 반영되는지 | ADR-104, ADR-105, ADR-106, ADR-107 |
| ~~OPEN-45~~ | 파일 잠금의 범위(파일 전체만 vs `fcntl`류 바이트 범위 잠금까지) | ADR-102 |
| ~~OPEN-39~~ | 로그인 프롬프트 프로세스와 실제 콘솔/tty 드라이버의 연결 방식, 다중 콘솔 기동 방식 | ADR-097, ADR-098 |
| ~~OPEN-41~~ | 신원 위임의 승인 타임아웃 재조정 규칙(시스템 전역·위임자 개인 설정) | ADR-095 |
| ~~OPEN-40~~ | sudoers류 승인 정책(대상 계정 자격 증명 없이 정책만으로 su/sudo 허가하는 방식) | ADR-093 |
| ~~OPEN-30~~ | trusted 프로세스 플래그 발급 권한·절차 | ADR-074 |
| ~~OPEN-31~~ | 다중 코어 실경합 시 spinlock(TTAS) 확장성, ticket/MCS 전환 필요성 | ADR-076 |
| ~~OPEN-33~~ | libk 단위 테스트 전략(호스트 네이티브 vs QEMU 간접 검증) | ADR-077 |
| ~~OPEN-34~~ | 아키텍처별 커널 가상메모리 레이아웃(higher-half 기준 주소, identity map 범위 등) | ADR-078 |
| ~~OPEN-35~~ | procsrv 사용자 계정 모델(uid/gid 발급, ROOT/Supervisor 식별, 권한 상승 인증 수단) | ADR-079 |
| ~~OPEN-36~~ | jail(J) 비트의 구체적 격리 메커니즘(VFS 루트 제한) | ADR-081 |
| ~~OPEN-37~~ | jail/guest 상태에서 `/sys/proc`·`/sys/dev`·`/sys/live` 가시성 필터링 범위 | ADR-083 |
| ~~OPEN-38~~ | super/root의 badge 붙은 핸들을 jail/guest에게 의도적으로 재위임하는 "confused deputy"류 경로 | ADR-085 |
| ~~OPEN-1~~ | 구체적 아키텍처 조합과 1순위 타겟 | ADR-002, ADR-009 |
| ~~OPEN-2~~ | C++ 언어 부분집합 정책 (예외/RTTI/STL 범위) | ADR-003, ADR-010 |
| ~~OPEN-3~~ | IPC 메시지 전달 방식 | ADR-004, ADR-013 |
| ~~OPEN-4~~ | 커널 메모리 할당 정책 | ADR-012 |
| ~~OPEN-5~~ | 커널 객체 참조/명명 방식 | ADR-011 |
| ~~OPEN-6~~ | 스케줄러 모델 | ADR-014 |
| ~~OPEN-7~~ | 부트 프로토콜 및 초기 유저 프로세스(initrun) 구성 | ADR-017 |
| ~~OPEN-8~~ | POSIX 호환 계층의 배치와 범위 | ADR-005, ADR-008 |
| ~~OPEN-9~~ | 드라이버/서비스의 실행 위치 (커널 내부 vs 유저 프로세스) | ADR-006, ADR-007 |
| ~~OPEN-10~~ | fork 의미론의 구체적 구현 방식 (COW, pager 위임) | ADR-016 |
| ~~OPEN-11~~ | fd → (서버 캐패빌리티, 핸들) 매핑 및 VFS↔FS 프로토콜 | ADR-018 |
| ~~OPEN-12~~ | 핸들 철회(revoke)·권한 축소(파생 핸들) 표현 방식 | ADR-023 |
| ~~OPEN-13~~ | 커널 OOM 정책 및 쿼터 위임/재분배 | ADR-024 |
| ~~OPEN-14~~ | IPC 페이지 자동 번역의 정확한 의미론 (임시매핑/복사/이전) | ADR-015 |
| ~~OPEN-15~~ | 우선순위 승격 권한의 발급/철회 정책 | ADR-027 |
| ~~OPEN-16~~ | 도네이션 IPC의 우선순위 상속 규칙 | ADR-028 |
| ~~OPEN-17~~ | 유저 밴드 내부 스케줄링 세부 알고리즘 | ADR-025 |
| ~~OPEN-18~~ | IPC move/map 모드 사용 권한 제약 필요 여부 | ADR-029 |
| ~~OPEN-19~~ | aarch64 부팅 경로(디바이스 트리) 및 initrun 초기 인자 형식 | ADR-026 |
| ~~OPEN-20~~ | 프록시 핸들 체인 허용 여부·깊이 제한 | ADR-032 |
| ~~OPEN-21~~ | 정책 서버 다운 시 쿼터 조정 폴백 정책 | ADR-051 |
| ~~OPEN-22~~ | boot_info를 initrun에 전달하는 구체적 메커니즘 | ADR-030 |
| ~~OPEN-23~~ | 락 순서(lock ordering) 규칙 및 교착상태 방지 정책 | ADR-052 |
| ~~OPEN-24~~ | NUMA 노드 간 워크 스틸링 여부와 정책 | ADR-053, ADR-054 |
| ~~OPEN-25~~ | AP 기동 절차 및 코어 간 IPI/TLB shootdown 메커니즘 | ADR-055 |
| ~~OPEN-26~~ | 동일 장치에 다중 드라이버 등록 시 충돌 해소 정책 | ADR-056 |
| ~~OPEN-27~~ | FAT32 FS 서버(/boot/uefi ESP)의 로드맵 편입 시점 | ADR-057 |
| ~~OPEN-28~~ | /sys/live 하위 경로와 담당 서버의 구체적 매핑 표 | ADR-058 |
