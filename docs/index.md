# Minicore 설계 문서 (CNW 사본)

<!-- 자동 생성 - node scripts/export-cnw-docs.mjs로 갱신, 손으로 편집하지 마세요. -->

정본은 claude-native-workflow(CNW) 시스템 DB에 있습니다. 이 폴더는
GitHub에서 코드 없이도 설계 문서를 볼 수 있게 하기 위한 읽기 전용
사본(캐시)입니다.

| 추적 코드 | 제목 | 상태 |
|---|---|---|
| [DC-01434F82](./DC-01434F82.md) | 비동기 프레임워크(SP-F682B889) 세부 결정 미정 | approved |
| [DC-0CC88ABB](./DC-0CC88ABB.md) | HPET 없는 환경에서 스케줄러 LAPIC 틱과 Timer 전역 틱의 하드웨어 소유권 충돌 | approved |
| [DC-23AEA8B4](./DC-23AEA8B4.md) | AsyncTask 컨텍스트 전환: 스택풀 vs C++20 코루틴 미정 | approved |
| [DC-3D3212A4](./DC-3D3212A4.md) | 가드 페이지 구현 중 발견: IST 없이는 오버플로우가 진단 없는 triple fault로 귀결 | approved |
| [DC-427BB6B2](./DC-427BB6B2.md) | 부트 경로 구현 착수 순서 및 부트로더 구현 방식 미정 | approved |
| [DC-474EE823](./DC-474EE823.md) | 4K 페이지의 2M 페이지 병합 정책 미정 | approved |
| [DC-4809BB47](./DC-4809BB47.md) | Slab 할당자(SP-D7013B26) 세부 결정 미정 | approved |
| [DC-48565C0B](./DC-48565C0B.md) | 빌드 시스템/툴체인/CI 미정 | approved |
| [DC-5AB13FFC](./DC-5AB13FFC.md) | 커널 서비스 내부 알고리즘 및 라이선스 미정 | approved |
| [DC-79A2387A](./DC-79A2387A.md) | 커널 C++ 런타임/코딩 컨벤션 세부 미정 | approved |
| [DC-8EA1E7F6](./DC-8EA1E7F6.md) | 스케줄러: 프로세스/스레드 모델 및 컨텍스트 스위칭 구조 미정 | approved |
| [DC-B538A218](./DC-B538A218.md) | 메시징 채널 IPC(SP-1FBC0EEB) 세부 결정 미정 | approved |
| [DC-B80D8D31](./DC-B80D8D31.md) | 스케줄러: 스케줄링 정책/타이머 틱/로드밸런싱 세부 미정 | approved |
| [DC-D868D9EC](./DC-D868D9EC.md) | Syscall 서브시스템(SP-04EE2A18) 세부 결정 미정 | approved |
| [DS-D4E5C451](./DS-D4E5C451.md) | Minicore 초기 설계 결정 확정 (빌드/부팅/커널서비스/라이선스) | review |
| [PL-16E2CDA4](./PL-16E2CDA4.md) | Slab 할당자(libkmm) 구현 | approved |
| [PL-1E247831](./PL-1E247831.md) | 비동기 프레임워크(AsyncTask) 구현 | approved |
| [PL-2070E6EF](./PL-2070E6EF.md) | MSI/MSI-X 인터럽트 지원 | approved |
| [PL-21344323](./PL-21344323.md) | Syscall 디스패치 서브시스템 구현 | approved |
| [PL-2D149D8F](./PL-2D149D8F.md) | IOAPIC 외부 인터럽트 라우팅 | approved |
| [PL-2D3184BC](./PL-2D3184BC.md) | 스케줄러 (프로세스/태스크/컨텍스트 스위칭) | approved |
| [PL-57CF86EF](./PL-57CF86EF.md) | 4K 페이지의 2M 페이지 병합/분할 | draft |
| [PL-65C20380](./PL-65C20380.md) | SMP AP(나머지 코어) 기동 | approved |
| [PL-99562483](./PL-99562483.md) | 물리 메모리 관리 범위를 1GiB 한도 밖으로 확장 | approved |
| [PL-C8648D4D](./PL-C8648D4D.md) | 메시징 채널 IPC(Channel) 구현 | approved |
| [PL-D65F49CC](./PL-D65F49CC.md) | x2APIC 지원 | approved |
| [PL-E68894CD](./PL-E68894CD.md) | HPET 지원 | approved |
| [PL-FC38956C](./PL-FC38956C.md) | multiboot2 + GRUB 부팅 경로 추가 | approved |
| [QA-26450C3E](./QA-26450C3E.md) | Minicore 초기 QA 시나리오 — 부팅/커널 기본 동작 | review |
| [RM-23F4B687](./RM-23F4B687.md) | Minicore 작업 지침 — 코딩 컨벤션 및 문서화 원칙 | review |
| [RM-32D06563](./RM-32D06563.md) | Minicore 용어 및 개념 | review |
| [RM-48E1E610](./RM-48E1E610.md) | Minicore Syscall 할당표 | review |
| [RM-7C249618](./RM-7C249618.md) | Minicore 라이브러리 목록 | review |
| [RM-9B8CA541](./RM-9B8CA541.md) | Minicore 가상주소 공간 관리자 위임 사항 | review |
| [RM-B5764185](./RM-B5764185.md) | Minicore Signal 번호표 | review |
| [RM-C65F7760](./RM-C65F7760.md) | Minicore VFS 구조 | review |
| [SP-04EE2A18](./SP-04EE2A18.md) | Syscall 디스패치 및 비동기 처리 서브시스템 — 설계 제안 | approved |
| [SP-0666DB3C](./SP-0666DB3C.md) | 커널 동기화 프리미티브(Mutex/Semaphore) 및 Signal 전달 — 설계 제안 | review |
| [SP-1FBC0EEB](./SP-1FBC0EEB.md) | 메시징 채널 IPC — 설계 제안 | approved |
| [SP-2AAD7C8D](./SP-2AAD7C8D.md) | mmap 서브시스템 및 Maple Tree 자료구조 — 설계 제안 | review |
| [SP-39F18E30](./SP-39F18E30.md) | DMA 버퍼 관리자 및 메모리 관리자 확장 — 설계 제안 | review |
| [SP-5A255B7C](./SP-5A255B7C.md) | 비동기 프레임워크 우선 설계 원칙 평가 - 동기 구현의 wrapper화 검토 | review |
| [SP-677210E6](./SP-677210E6.md) | TSS/IST 예외 스택 서브시스템 — 설계 제안 | review |
| [SP-68182FBD](./SP-68182FBD.md) | 프로세스 모델(Process/AddressSpace) — 설계 제안 및 1차 구현 | review |
| [SP-7CC5693A](./SP-7CC5693A.md) | VFS 커널 서브시스템 — 설계 제안 | review |
| [SP-8B6B8D25](./SP-8B6B8D25.md) | Minicore 범용 운영체제 — 초기 설계 명세 | review |
| [SP-9DD4F3EA](./SP-9DD4F3EA.md) | 장치 자동 인식 및 핫플러그 프레임워크(PnP) — 설계 제안 | review |
| [SP-B1E258D8](./SP-B1E258D8.md) | RCU(Read-Copy-Update) 인프라 도입 평가 및 설계 제안 | pending |
| [SP-C2670F69](./SP-C2670F69.md) | AHCI(SATA 스토리지 컨트롤러) 드라이버 — 설계 제안 | review |
| [SP-D7013B26](./SP-D7013B26.md) | Slab 할당자(libkmm) — 설계 제안 | approved |
| [SP-DE19BB1C](./SP-DE19BB1C.md) | 커널 영역 TLB 샷다운(IPI 기반) — 설계 제안 | review |
| [SP-E35FD36C](./SP-E35FD36C.md) | USB 스택(호스트 컨트롤러 + 장치 열거) — 설계 제안 | review |
| [SP-F682B889](./SP-F682B889.md) | 커널 전용 비동기 프레임워크(Task 기반) — 설계 제안 | approved |
