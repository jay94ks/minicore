# 설계 문서 색인

`docs/design/`의 모든 문서 목록. 설계 결정(ADR)은 하나의 거대한
로그 대신 **주제별로 분리된 파일**에서 관리한다 — 결정을 추가하거나
찾을 때는 아래 표에서 해당 주제의 파일을 먼저 찾는다.

## 설계 결정(ADR) — 주제별

| 문서 | 다루는 ADR | 내용 |
|---|---|---|
| [foundations.md](foundations.md) | ADR-001~003, 005~010, 042, 049 | 프로젝트 목표, 아키텍처 범위, 언어, 코드 소유 경계, 네이밍 컨벤션 |
| [build-system.md](build-system.md) | ADR-019~022, 031, 091, 113, 115, 116 | CMake, 툴체인, 저장소 구조, 서드파티 소스 관리, QEMU fork 정책, freestanding C++ 헤더 확보, freestanding memset/memcpy 제공 |
| [kernel-ipc-objects.md](kernel-ipc-objects.md) | ADR-004, 011, 013, 015, 023, 028, 029, 032 | IPC 원시, 핸들·프록시 객체 모델 |
| [kernel-memory.md](kernel-memory.md) | ADR-012, 016, 024, 033, 036, 051, 052, 054, 078, 104~108, 110 | 메모리 할당, 쿼터, NUMA, 락 모델, 가상메모리 레이아웃, 메모리 예약·회수·압박 신호, 스왑 압축, 압축 실행 위치(SWAPFS 서버) |
| [kernel-scheduler.md](kernel-scheduler.md) | ADR-014, 025, 027, 034, 035, 053, 055, 109 | 우선순위 밴드, NUMA 런큐, 승격, 도네이션, 최근 스케줄링 빈도 측정 |
| [boot-and-drivers.md](boot-and-drivers.md) | ADR-017, 026, 030, 037~041, 043, 046, 056, 057, 097, 111, 114, 117 | 부트 프로토콜, 디버그 콘솔, PCIe, 드라이버 로드맵, 콘솔/TTY 드라이버(상세 프로토콜은 기존 표준 채택), QEMU 검증용 PVH 직접 부팅, Multiboot2 파서 self-test 검증 전략 |
| [filesystem.md](filesystem.md) | ADR-018, 044, 045, 047, 048, 050, 058, 059, 065, 080, 099~103 | fd 라우팅, VFS 런타임 디렉토리 구성, 마운트 네임스페이스·오버레이, 유저 영역 FS(FUSE류), SWAPFS(파티션/파일 백엔드), 파일 잠금·공유 모드(파일 단위) |
| [registry-decisions.md](registry-decisions.md) | ADR-060~064 | 설정 리포지터리(cfgsrv) 서브시스템 |
| [libk.md](libk.md) | ADR-066~073(073은 076으로 대체), 076~077, 118 | libk(커널·서버 공용 프리스탠딩 코어 라이브러리): result/optional/span/intrusive_list/atomic/spinlock·ticket_lock·mcs_lock, 검증 전략, 전역 constexpr 초기화 요구사항 |
| [security-model.md](security-model.md) | ADR-074~075, 079, 081~090, 092~096, 098, 112 | 프로세스 신뢰(trusted) 위임 체인, 유저 프로세스 권한 상승(sudo류)·ROOT 전권 모델, procsrv 계정 모델, jail/guest VFS·실행·시스템 가시성 격리 정책, badge 신원 전파, super badge 재위임 원천 차단, 신원 불변 원칙, 계정 생성·로그인·su/sudo 절차, 탈중앙화 신원 위임, 위임 타임아웃 재조정 규칙, 위임 기간 모드(기본값/명시적/영구), 위임 시 재인증 요구 규칙, 세션 stdio/TTY 상속 |

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
