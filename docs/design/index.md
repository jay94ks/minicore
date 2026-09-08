# 설계 문서 색인

`docs/design/`의 모든 문서 목록. 설계 결정(ADR)은 하나의 거대한
로그 대신 **주제별로 분리된 파일**에서 관리한다 — 결정을 추가하거나
찾을 때는 아래 표에서 해당 주제의 파일을 먼저 찾는다.

## 설계 결정(ADR) — 주제별

| 문서 | 다루는 ADR | 내용 |
|---|---|---|
| [foundations.md](foundations.md) | ADR-001~003, 005~010, 042, 049 | 프로젝트 목표, 아키텍처 범위, 언어, 코드 소유 경계, 네이밍 컨벤션 |
| [build-system.md](build-system.md) | ADR-019~022, 031 | CMake, 툴체인, 저장소 구조, 서드파티 소스 관리 |
| [kernel-ipc-objects.md](kernel-ipc-objects.md) | ADR-004, 011, 013, 015, 023, 028, 029, 032 | IPC 원시, 핸들·프록시 객체 모델 |
| [kernel-memory.md](kernel-memory.md) | ADR-012, 016, 024, 033, 036, 051, 052, 054 | 메모리 할당, 쿼터, NUMA, 락 모델 |
| [kernel-scheduler.md](kernel-scheduler.md) | ADR-014, 025, 027, 034, 035, 053, 055 | 우선순위 밴드, NUMA 런큐, 승격, 도네이션 |
| [boot-and-drivers.md](boot-and-drivers.md) | ADR-017, 026, 030, 037~041, 043, 046, 056, 057 | 부트 프로토콜, 디버그 콘솔, PCIe, 드라이버 로드맵 |
| [filesystem.md](filesystem.md) | ADR-018, 044, 045, 047, 048, 050, 058, 059, 065 | fd 라우팅, VFS 런타임 디렉토리 구성 |
| [registry-decisions.md](registry-decisions.md) | ADR-060~064 | 설정 리포지터리(cfgsrv) 서브시스템 |

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
