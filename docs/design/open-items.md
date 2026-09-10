# 미결정 항목 (OPEN)

[← 설계 문서 색인](index.md)

모든 카테고리 파일에 흩어진 미결정 항목을 한 곳에 모은 문서. 새
미결정 항목은 다음 번호(가장 최근 OPEN 번호(이 문서에서 확인) + 1,
2026-09-10 기준 `OPEN-68`부터)로 여기에 추가하고, 해당 결정을
다루는 카테고리 파일에도 같은 번호로 언급한다. 해결되면
`~~OPEN-N~~`으로 취소선 처리하고 해결한 ADR 번호를 적는다.

## 현재 열려있는 항목

| ID | 내용 | 관련 ADR | 다루는 문서 |
|---|---|---|---|
| OPEN-32 | result/optional의 `[[nodiscard]]` 강제 여부 최종 확정 시점 | ADR-010, ADR-068, ADR-069 | [libk.md](libk.md) |
| OPEN-42 | 위임의 **시간대 단위** 제한(특정 시간대에만 위임 유효) 지원 여부만 남음 — 기간/영구성은 ADR-096, **명령 단위 범위는 ADR-194로 해결**, 재인증 요구 여부는 ADR-112로 이미 해결됨 | ADR-093, ADR-096, ADR-112, ADR-194 | [security-model.md](security-model.md) |
| OPEN-60 | ADR-154가 "I/O 활성화 권한 부여"를 지금은 initrun의 하드코딩(스폰 시 grant_trusted)으로만 결정하도록 확정했다 — 부팅 완료 이후 cfgsrv 레지스트리를 읽어 **다른** 특수 프로세스에게도 이 권한을 동적으로 부여/회수하는 절차는 cfgsrv가 실제로 존재하는 M19 이후 재검토 대상 | ADR-154 | [boot-and-drivers.md](boot-and-drivers.md) |
| OPEN-62 | ADR-176이 LAPIC 타이머의 initial_count/divide를 **보정 없이**(PIT/HPET 실측 없이) 고정 상수로 정했다 — `base_time_slice_us` 필드는 이름과 달리 지금 실제 마이크로초를 보장하지 않고 "타이머 틱 수"로만 쓰인다. 실제 마이크로초 단위 타임슬라이스가 필요해지면 PIT/HPET 기반 보정을 추가해야 한다(어느 시점에, 어떤 기준 클록으로 보정할지는 아직 미정) — **ADR-184 + [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) M33이 해소 예정** | ADR-176, ADR-184 | [kernel-scheduler.md](kernel-scheduler.md) |
| OPEN-63 | ADR-176은 AP가 여전히 협조적 스케줄러/run_queue에 전혀 참여하지 않는 것을 그대로 유지한 채(M10/M11 결정) BSP 한 코어에만 선점을 추가했다 — 진짜 멀티코어 선점형 스케줄러(코어별 `g_current`/타이머, AP도 유저 스레드 실행)는 이 ADR의 범위 밖으로 남겨 뒀다. general-purpose-completion.md의 M22~M26 어디에도 아직 이 작업이 배정돼 있지 않다 — **ADR-185+ADR-191(timer_source_interface 추상화) + [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) M34가 해소 예정**(M37 pthread의 진짜 병렬성이 이 위에서 성립) | ADR-176, ADR-185, ADR-191 | [kernel-scheduler.md](kernel-scheduler.md) |
| OPEN-64 | (범위 좁혀짐, ADR-201/M27로 실제 프로세스 테이블+범용 wait/kill은 해결) ADR-179(M23)의 procsrv.md §3.6/§4.1 완전한 fd 진실 공급원 프로토콜(`dup_for_new_client`, 발신자 신원별 접근 제한)은 여전히 미구현이다 — 커널의 handle_table 통째 복제로 fork 시나리오만 충족. **이 부분은 real-libc-syscall-layer.md에서도 명시적으로 범위 밖으로 남는다** — fork는 신원 불변이라 M23의 대체로 충분하고, su/sudo 경유 실행이 실제로 필요해지는 시점까지 미룬다 | ADR-179, ADR-201 | [kernel-memory.md](kernel-memory.md), [procsrv.md](../spec/procsrv.md) |
| OPEN-66 | ADR-182(M26)는 musl의 문자열 함수 부분집합만 실제로 포팅했다 — 진짜 syscall 계층(open/read/write/mmap/fork/exec을 musl 자신의 경로로 감싸는 새 레이어), 동적 링커, pthread, locale, stdio(FILE/printf 계열) 포팅은 전혀 없다. 실제 포팅된 셸/coreutils로 M20의 완료 기준을 다시 달성하는 것은 이 OPEN이 해소된 뒤의 일이다 — **ADR-183~195 + [real-libc-syscall-layer.md](../plan/real-libc-syscall-layer.md) M27~M39가 착수 예정**(M28·M30~M32 syscall 계층, M29 동적 링킹, M35 locale, M36 signal, M37 pthread, M38 SDK 내보내기). 각 항목의 범위가 의도적으로 좁아(musl 자신의 공유 libc.so 하나만, 표준 시그널만, futex 기초 연산만 등) 완료돼도 OPEN-66은 완전히는 해소되지 않는다 — 완료 시점에 남는 세부(다중 `.so` 일반화, 실시간 시그널, job control 등)를 새 OPEN 번호로 분리한다 | ADR-182~195 | [foundations.md](foundations.md) |
| OPEN-67 | ADR-201(M27)이 procsrv의 실제 process_entry 테이블+범용 proc_op::wait/kill을 만들면서 의도적으로 남긴 두 가지 — (1) 호출자가 자신의 pid를 메시지 필드로 스스로 주장한다(커널 badge로 검증하지 않는다, 위조 가능), (2) wait는 진짜 블로킹이 아니라 비블로킹 폴링이다(procsrv가 단일 요청-응답 루프라 Call을 붙들고 대기할 수 없다). 각각 badge에 pid를 인코딩하는 커널 캐패빌리티 확장과 procsrv의 동시성/비동기 응답 모델이 필요하다 — musl `SYS_wait4`(M32) 착수 시점이나 user-service-manager.md 착수 시점에 재검토 대상 | ADR-201 | [security-model.md](security-model.md) |

## 해결된 항목 (이력)

| ID | 내용 | 해결 ADR |
|---|---|---|
| ~~OPEN-54~~ | procsrv의 실제 IPC 와이어 프로토콜(메시지 레이아웃, 각 오퍼레이션의 정확한 label/파라미터) — 방법론은 ADR-195(@wire-op 마크업+tools/gen-wire-docs.py), 실제 바이트 레이아웃은 M27의 `mc/procsrv_protocol.h`(wait/kill/exit_report)로 확정됐다(`docs/spec/generated/procsrv-wire.md`가 정본 참조표) | ADR-195, ADR-201 |
| ~~OPEN-65~~ | ADR-178의 kill은 대상이 IPC 대기열에 갇혀 있고 아무도 다시 깨우지 않으면 영원히 폐기되지 않음 — `SIGKILL` 한정으로 대기열에서 즉시 `unlink`하는 메커니즘으로 해소(POSIX SIGKILL이 인터럽터블 슬립도 즉시 깨우는 것과 같은 동작). 일반 시그널은 여전히 "다음 실행 시점"에만 전달됨(그대로 유지) | ADR-186 |
| ~~OPEN-52~~ | 프로세스/서비스의 "초기화 완료(준비됨)" 신호 프로토콜 — 새 커널 프리미티브 없이, spawn 시점에 이미 만들어지는 전용 endpoint 위의 예약 label(`k_service_ready_label`) Call/Reply로 통일(종료 회수의 `OP_WAIT`와 정확히 같은 모양) | ADR-193 |
| ~~OPEN-51~~ | initrun이 마지막으로 실행하는 "systemd류 초기 프로세스"의 정체성·책임 범위 — 존재는 유지, 역할을 두 계층으로 분리해 해소: "커널 서버"(procsrv/vfs/devmgr 등)는 initrun이 하드코딩된 이름으로 직접 실행하고, 이 데몬(`servers/svcmgr`)은 그와 겹치지 않는 "유저 서비스"만 관리한다. 프로세스 트리의 영구 루트는 이 데몬이고, initrun 종료 시 남은 커널 서버들은 이 데몬으로 재부모화된다. 유닛 모델·시작 절차·컨트롤 프로토콜 개요는 ADR-196, 레지스트리 스키마(@global/system/services)는 ADR-197로 설계 완료 — 실제 구현은 [user-service-manager.md](../plan/user-service-manager.md) M40~M43 | ADR-192, ADR-196, ADR-197 |
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
