# minicore 문서 인덱스

마이크로커널 OS `minicore`의 모든 문서 목록.

## spec — 명세
| 문서 | 설명 |
|---|---|
| [boot.md](spec/boot.md) | 커널 진입점(Multiboot2/UEFI/FDT), boot_info 구조체(NUMA 토폴로지 포함), initrd 포맷, initrun 전달 |
| [ipc.md](spec/ipc.md) | Call/Reply·Notification 시스템 콜, message 구조, 도네이션 우선순위, 에러 코드 |
| [debug-console.md](spec/debug-console.md) | 커널 내장 시리얼(16550/PL011) 디버그 로깅 경로, klog API, 패닉 시 스택 백트레이스, QEMU GDB 원격 디버깅 |
| [pcie.md](spec/pcie.md) | ECAM/레거시 설정공간 접근, devmgr 버스 열거, 핫플러그, PnP 드라이버 등록 |
| [objects.md](spec/objects.md) | 핸들 테이블, 프록시 트리, 핸들 전달·cascade revoke 절차 |
| [memory.md](spec/memory.md) | 노드별/코어별 페이지 할당자, 커널 힙(슬랩), 프로세스 쿼터 |
| [scheduler.md](spec/scheduler.md) | 커널/유저 밴드, NUMA 노드별 런큐, 승격, 도네이션 |
| [cxx-conventions.md](spec/cxx-conventions.md) | 언어 부분집합, snake_case+`_interface` 네이밍, libk 구성 요소 |
| [vfs-layout.md](spec/vfs-layout.md) | 런타임 VFS 디렉토리 트리, 경로별 소유 서버, POSIX 경로 호환 전략 |
| [registry.md](spec/registry.md) | 설정 리포지터리(cfgsrv): 스키마/테이블 주소 체계, 권한 모델, 전용 IPC 프로토콜, 비밀 데이터 보호 |
| [virtual-memory-layout.md](spec/virtual-memory-layout.md) | 아키텍처별 커널 가상메모리 레이아웃: physmap/스택/이미지 영역 주소, 부팅 시 페이지테이블 구성 순서 |
| [procsrv.md](spec/procsrv.md) | 프로세스 서버: 프로세스 테이블, fork/exec 시퀀스, fd 진실 공급원 프로토콜, 계정 생성·로그인·session_program 프로토콜 (에스컬레이션/su·sudo/쿼터/콘솔 연동은 후속) |

## plan — 실행 계획 (실행 전)
| 문서 | 설명 |
|---|---|
| [scaffold-repo-skeleton.md](plan/scaffold-repo-skeleton.md) | 저장소 디렉토리·CMake 골격 생성 계획 (실행 완료, 결과는 done 참고) |
| [kernel-bootstrap.md](plan/kernel-bootstrap.md) | x86_64 부팅→IPC→initrun 최초 수직 슬라이스 마일스톤 계획 (M1~M8 전부 완료 — 결과는 done 참고) |

## done — 완료 보고
| 문서 | 설명 |
|---|---|
| [scaffold-repo-skeleton.md](done/scaffold-repo-skeleton.md) | 저장소 스캐폴딩 실행 결과 및 크로스 툴체인 부재 검증 한계 |
| [toolchain-setup.md](done/toolchain-setup.md) | Clang/LLVM 크로스 툴체인 설치 및 x86_64/aarch64 CMake 프리셋 구성 검증 |
| [kernel-bootstrap-m1.md](done/kernel-bootstrap-m1.md) | M1: x86_64 Multiboot2 부트 스텁 + klog, QEMU에서 "hello from kernel" 확인 |
| [kernel-bootstrap-m2.md](done/kernel-bootstrap-m2.md) | M2: Multiboot2 태그 파서 + boot_info 파이프라인, self-test로 메모리맵·initrd 덤프 검증 |
| [kernel-bootstrap-m3.md](done/kernel-bootstrap-m3.md) | M3: libk 10종 + 물리 페이지 할당자(buddy)·슬랩 힙, 호스트 단위 테스트 39개 + QEMU 왕복 검증 |
| [kernel-bootstrap-m4.md](done/kernel-bootstrap-m4.md) | M4: 핸들 테이블+프록시 트리(cascade revoke)·페이지테이블 조작 API, QEMU 왕복 검증 |
| [kernel-bootstrap-m5.md](done/kernel-bootstrap-m5.md) | M5: run_queue+협조적 라운드로빈, 커널 스레드 2개 컨텍스트 스위치 QEMU 확인 |
| [kernel-bootstrap-m6.md](done/kernel-bootstrap-m6.md) | M6: IPC endpoint+Call/Reply+도네이션, 커널 스레드 2개 사이 왕복 + badge 전파 QEMU 확인 |
| [kernel-bootstrap-m7.md](done/kernel-bootstrap-m7.md) | M7: IPC 페이지(copy)·핸들 전달 + notification, 위임된 핸들로 직접 sys_wait까지 QEMU 확인 |
| [kernel-bootstrap-m8.md](done/kernel-bootstrap-m8.md) | M8(최종): initrd→ELF 로드→유저모드 진입(SYSCALL/SYSRET)→initrun의 IPC Call에 커널 응답까지 QEMU 확인 — kernel-bootstrap.md 전체 완료 |

## design — 설계/상세

`docs/design/`는 문서가 많아 자체 색인을 따로 둔다.

| 문서 | 설명 |
|---|---|
| [design/index.md](design/index.md) | **설계 문서 전체 목록** — 주제별로 분리된 ADR 파일(ADR-001~124)과 미결정 항목, repo-layout.md 안내 |

## remind — 기억 사항
| 문서 | 설명 |
|---|---|
| [doc-convention.md](remind/doc-convention.md) | 문서 디렉토리별 역할과 인덱싱 규칙 |

## reply — 답변 기록 (자동 생성, 미처리 답변함)
| 문서 | 설명 |
|---|---|
| [reply.md](reply.md) | `tools/docs-dashboard` 웹 대시보드의 "답변 입력" 탭에 입력한 내용을 기록하는 미처리 답변함. 최초 저장 시 자동 생성됨. 처리 규칙은 [CLAUDE.md](../CLAUDE.md) 참고 |
