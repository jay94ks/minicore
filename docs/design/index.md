# 설계 문서 색인

`docs/design/`의 모든 문서 목록. 설계 결정(ADR)은 하나의 거대한
로그 대신 **주제별로 분리된 파일**에서 관리한다 — 결정을 추가하거나
찾을 때는 아래 표에서 해당 주제의 파일을 먼저 찾는다.

## 설계 결정(ADR) — 주제별

| 문서 | 다루는 ADR | 내용 |
|---|---|---|
| [foundations.md](foundations.md) | ADR-001~003, 005~010, 042, 049, 132 | 프로젝트 목표, 아키텍처 범위, 언어, 코드 소유 경계, 네이밍 컨벤션, libmc(네이티브 유저랜드 C API 기반 라이브러리) |
| [build-system.md](build-system.md) | ADR-019~022, 031, 091, 113, 115, 116, 125 | CMake, 툴체인, 저장소 구조, 서드파티 소스 관리, QEMU fork 정책, freestanding C++ 헤더 확보, freestanding memset/memcpy 제공, 디버그 심볼·QEMU GDB stub/진단 플래그 |
| [kernel-ipc-objects.md](kernel-ipc-objects.md) | ADR-004, 011, 013, 015, 023, 028, 029, 032, 151, 155, 159, 161 | IPC 원시, 핸들·프록시 객체 모델, IPC의 cross-address-space 확장(서로 다른 유저 프로세스 간 실제 통신, sys_reply의 handles[] 지원, sys_recv/sys_reply syscall 노출), pages[] 페이로드의 cross-address-space 전달(수신자별 번역/공유매핑 이원화), 유저 프로세스 대상 매핑의 배치 위치+해제 시점(스레드별 고정 슬롯+자동 해제, M16 구현 완료 시점에 트리거를 "다음 deliver_message 목적지 선택 시점"으로 정정) |
| [kernel-memory.md](kernel-memory.md) | ADR-012, 016, 024, 033, 036, 051, 052, 054, 078, 104~108, 110, 121, 134, 136, 140~142, 145, 148, 149, 152, 160 | 메모리 할당, 쿼터, NUMA, 락 모델(실제 락 순서 표 포함), 가상메모리 레이아웃, 메모리 예약·회수·압박 신호, 스왑 압축, 압축 실행 위치(SWAPFS 서버), 유저 주소공간의 저지대 항등 매핑(GDT) 공유, 슬랩 헤더 16바이트 정렬, COW 실제 구현(프레임 참조 카운트+PTE 소프트웨어 비트+쓰기 폴트 분기), syscall 커널 스택 스레드별 분리, sys_fork/sys_process_spawn/sys_exec/sys_thread_exit(레지스터 수준 fork 재현), pml4[0] 공유 범위를 [0,8MiB)로 정확히 좁힘, trusted 전용 DMA 버퍼 할당 syscall, M12 self_info 브릿지의 build_process() 일반화, sys_process_spawn 스폰 시점 캐패빌리티 주입, 유저 가상주소공간 전체 메모리맵 표 확정(고정 슬롯 크기+경계 검증으로 충돌 구조적 차단) |
| [kernel-scheduler.md](kernel-scheduler.md) | ADR-014, 025, 027, 034, 035, 053, 055, 109, 124, 127, 133, 135, 137~139, 144 | 우선순위 밴드, NUMA 런큐, 승격, 도네이션, 최근 스케줄링 빈도 측정, 스레드 영구 종료(sched::exit), FPU/SIMD 컨텍스트 스위칭(lazy XSAVE/AVX로 개정, 실제 구현은 M11b), AP 기동(INIT-SIPI-SIPI) 구현 세부, 워크 스틸링 범위 정정(커널 밴드도 노드 경계를 넘음), fpu_save_area 별도 페이지 할당(64바이트 정렬), yield() enqueue 순서 버그 수정, TLB shootdown의 로컬 코어 invlpg 누락 수정 |
| [boot-and-drivers.md](boot-and-drivers.md) | ADR-017, 026, 030, 037~041, 043, 046, 056, 057, 097, 111, 114, 117, 119, 120, 122, 123, 126, 129~131, 143, 146, 147, 150, 154, 156, 157, 158, 163, 164, 166 | 부트 프로토콜, 디버그 콘솔, PCIe, 드라이버 로드맵, 콘솔/TTY 드라이버(상세 프로토콜은 기존 표준 채택), QEMU 검증용 PVH 직접 부팅, Multiboot2 파서 self-test 검증 전략, initrd 임베딩(개발 환경), .boot.bss 명시적 제로화, SYSCALL ABI 관례, initrun 링크 주소, 패닉 시 스택 백트레이스, ext4 로드맵/구현 범위, 입력장치(PS/2·USB) 로드맵, initrun 부팅: 부트 디바이스(initrd의 disk.cfg로 전달) 직접 마운트해 서비스 탐색·기동, TSS(RSP0) 최소 설정, boot_device_descriptor 필드, virtio-blk PCI BAR 커널 직접 배정+IOPB, M12 완성(virtio-blk 클라이언트+실제 부트 디스크 마운트+procsrv 골격), 스레드별 활성 I/O 포트 범위(IOPB 재설계), sys_map_phys(MMIO 캐패빌리티), M14 완성(devmgr ACPI/PCIe 열거+PS/2 자체테스트+USB xHCI 리셋), M15 완성(virtio-blk 첫 실제 유저 드라이버+devmgr BAR 재사용 일반화+부트 디바이스 배제, 디스크 손상 버그 기록), devmgr I/O BAR 배정을 전진 커서로 전환(M16이 드러낸 동시 배정 충돌 버그 기록), M17 콘솔/TTY 범위 좁힘(cfgsrv 없는 단일 TTY+VGA 텍스트 모드), M17 완성(콘솔/ps2/procsrv/login 배선, initrun 서비스 레지스트리 크기+ps2 폴링 예산 버그 기록) |
| [filesystem.md](filesystem.md) | ADR-018, 044, 045, 047, 048, 050, 058, 059, 065, 080, 099~103, 128, 153, 162 | fd 라우팅, VFS 런타임 디렉토리 구성, 마운트 네임스페이스·오버레이, 유저 영역 FS(FUSE류), SWAPFS(파티션/파일 백엔드), 파일 잠금·공유 모드(파일 단위), FS 서버 공통 파일 신원(fs_node_id), M13 완성(fs-protocol 최소 버전+vfs/memfs 서버+procsrv VFS 클라이언트), M16 완성(FAT32/ext4 읽기전용 FS 서버+VFS 정적 마운트 테이블+fs-protocol v2의 pages[] 기반 OP_READ) |
| [registry-decisions.md](registry-decisions.md) | ADR-060~064 | 설정 리포지터리(cfgsrv) 서브시스템 |
| [libk.md](libk.md) | ADR-066~073(073은 076으로 대체), 076~077, 118 | libk(커널·서버 공용 프리스탠딩 코어 라이브러리): result/optional/span/intrusive_list/atomic/spinlock·ticket_lock·mcs_lock, 검증 전략, 전역 constexpr 초기화 요구사항 |
| [security-model.md](security-model.md) | ADR-074~075, 079, 081~090, 092~096, 098, 112, 165 | 프로세스 신뢰(trusted) 위임 체인, 유저 프로세스 권한 상승(sudo류)·ROOT 전권 모델, procsrv 계정 모델, jail/guest VFS·실행·시스템 가시성 격리 정책, badge 신원 전파, super badge 재위임 원천 차단, 신원 불변 원칙, 계정 생성·로그인·su/sudo 절차, 탈중앙화 신원 위임, 위임 타임아웃 재조정 규칙, 위임 기간 모드(기본값/명시적/영구), 위임 시 재인증 요구 규칙, 세션 stdio/TTY 상속, M17 로그인 인증 범위 좁힘(procsrv 최소 계정 저장소+OP_LOGIN, 세션 프로그램 스폰은 M20으로 미룸) |

ADR 번호는 파일이 나뉘어도 **프로젝트 전체에서 계속 순차 증가**한다
(파일별로 번호가 다시 시작되지 않는다). 새 결정을 추가할 때는 가장
최근 ADR 번호 + 1을 쓰고, 주제가 맞는 파일에 추가한다. 어느 파일에도
잘 맞지 않는 새로운 주제라면 새 파일을 만들고 이 표에 추가한다.

## 미결정 항목

| 문서 | 내용 |
|---|---|
| [open-items.md](open-items.md) | 모든 카테고리에 걸친 미결정(OPEN) 항목과 해결 이력 |

## 그 외 설계 문서

| 문서 | 내용 |
|---|---|
| [repo-layout.md](repo-layout.md) | minicore **소스 저장소 자체**의 디렉토리 트리, CMake 서브프로젝트 구성 (build-system.md의 결정을 구체화) |

**주의**: [repo-layout.md](repo-layout.md)(저장소 구조)와
[../spec/vfs-layout.md](../spec/vfs-layout.md)(minicore가 부팅한 뒤
보여주는 런타임 파일시스템)는 이름이 비슷하지만 완전히 다른
대상이다 — 혼동하지 않는다.
