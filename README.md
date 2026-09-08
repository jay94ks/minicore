# minicore

**AI 네이티브 마이크로커널**

minicore는 처음부터 새로 설계하는 범용 지향 마이크로커널 OS입니다.
커널·디바이스 드라이버·시스템 서비스는 전부 직접 구현하고, 셸·
coreutils 같은 사용자 소프트웨어는 기존 생태계를 포팅해 완성합니다.

## 아키텍처 하이라이트

- **커널**: freestanding C++, 예외·RTTI 없음. x86_64 1순위, aarch64 후속.
- **IPC**: 동기 Call/Reply(도네이션 우선순위 상속) + 비동기 Notification 하이브리드.
- **객체 모델**: 프로세스당 단순 핸들 테이블 + 프록시 기반 위임/철회(cascade revoke).
- **메모리·스케줄러**: NUMA 노드별 풀/런큐, per-CPU 캐시, 락-프리 우선 설계.
- **유저랜드**: Hurd류 서버 분해(procsrv/vfs/fs/netsrv/devmgr/cfgsrv), POSIX 계층은
  서버별로 나뉘어 장애 격리가 가능.
- **설정 리포지터리**: Windows 레지스트리에서 착안한 스키마·테이블 기반 저장소,
  VFS와 완전히 분리된 전용 프로토콜, 신뢰 프로세스(trusted process) 보호.

## 현재 상태

설계·기획 단계입니다. 아직 부팅 가능한 커널 코드는 없습니다.

- 확정된 설계 결정(ADR) 65건, 주제별로 분리 관리 — [docs/design/index.md](docs/design/index.md)
- 구현 가능한 수준의 spec 문서 다수 — [docs/index.md](docs/index.md)
- 저장소 빌드 골격(CMake, 4개 아키텍처×툴체인 프리셋) 스캐폴딩 완료
- 다음 단계: [docs/plan/kernel-bootstrap.md](docs/plan/kernel-bootstrap.md)의
  x86_64 부팅→IPC→initrun 마일스톤 착수

## 문서

모든 문서는 [docs/index.md](docs/index.md)에서 목록화됩니다.

| 디렉토리 | 내용 |
|---|---|
| `docs/spec/` | 구현 가능한 수준의 명세 (부팅, IPC, 객체모델, 메모리, 스케줄러, PCIe, VFS, 레지스트리 등) |
| `docs/design/` | 설계 결정 기록(ADR)과 저장소 구조 설계 |
| `docs/plan/` | 실행 계획 (실행 전인 것만 유지) |
| `docs/done/` | 완료 보고 (실행 완료된 것만 기록) |
| `docs/remind/` | 기억해야 할 규칙 |

## 빌드

크로스 툴체인(Clang 권장, ADR-020/031에 따라 저장소 외부에 별도 설치)이
필요합니다. 아직 컴파일 가능한 커널 소스가 없어 실제 빌드 산출물은
없지만, 골격 구성은 확인 가능합니다:

```bash
cmake --preset x86_64-clang -S . -B build/x86_64-clang
```

## 라이선스

미정.
