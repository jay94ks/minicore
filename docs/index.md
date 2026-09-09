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
| [fs-protocol.md](spec/fs-protocol.md) | FS 서버 공통 프로토콜(M13 최소 버전): open/write/read 오퍼레이션, regs[]만 쓰는 M13 한정 인코딩, 상태 코드 |

## plan — 실행 계획 (실행 전)
| 문서 | 설명 |
|---|---|
| [scaffold-repo-skeleton.md](plan/scaffold-repo-skeleton.md) | 저장소 디렉토리·CMake 골격 생성 계획 (실행 완료, 결과는 done 참고) |
| [kernel-bootstrap.md](plan/kernel-bootstrap.md) | x86_64 부팅→IPC→initrun 최초 수직 슬라이스 마일스톤 계획 (M1~M8 전부 완료 — 결과는 done 참고) |
| [smp-fpu-bringup.md](plan/smp-fpu-bringup.md) | M9(FPU/SIMD 컨텍스트 스위칭)~M11b(lazy XSAVE/AVX 전환) — AP 기동·IPI·TLB shootdown(M10), 다중 코어/NUMA 검증+락 순서 문서화(M11) 포함, kernel-bootstrap.md 이후 계획 |
| [system-servers-bringup.md](plan/system-servers-bringup.md) | M12(procsrv)~M20(libc 포팅+로그인 후 셸) — VFS/memfs, devmgr+PCIe+PS/2+USB, virtio-blk, FAT32+ext4, 콘솔/로그인, 보안 모델(su/sudo/jail), cfgsrv 순. libmc(네이티브 C API 라이브러리)가 전 구간 교차 트랙 |

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
| [smp-fpu-bringup-m9.md](done/smp-fpu-bringup-m9.md) | M9: eager FXSAVE/FXRSTOR로 FPU/SIMD 컨텍스트 스위칭 QEMU 확인(양성+음성 대조 모두), 그 과정에서 발견한 슬랩 청크 16바이트 정렬 누락 버그(ADR-134) 진단·수정 |
| [smp-fpu-bringup-m10.md](done/smp-fpu-bringup-m10.md) | M10: IDT 기초(catch-all 예외 진단)+ACPI MADT 파싱(EBDA/BIOS ROM RSDP 스캔)+LAPIC 구동+AP 기동(INIT-SIPI-SIPI)+IPI 기반 TLB shootdown, `-smp N` QEMU로 AP 전원 온라인·shootdown 왕복 확인, 그 과정에서 발견한 SIPI 프로토콜·코어별 IDTR 버그(ADR-135) 진단·수정 |
| [smp-fpu-bringup-m11.md](done/smp-fpu-bringup-m11.md) | M11: ACPI SRAT/SLIT 파싱+다중 NUMA 노드 물리 메모리 풀 분리+거리 기반 할당 폴백(ADR-054)+워크 스틸링(ADR-053, 범위 정정 ADR-137)+실제 락 순서 표(ADR-136), `-numa` QEMU로 검증 |
| [smp-fpu-bringup-m11b.md](done/smp-fpu-bringup-m11b.md) | M11b(smp-fpu-bringup.md 전체 완료): CPUID 기반 XSAVE/AVX 검사+CR0.TS/`#NM` lazy 전환(ADR-133), 그 과정에서 발견한 slab 64바이트 정렬 미보장(ADR-138)·`sched::yield()` enqueue 순서 버그(ADR-139) 진단·수정, `-cpu` 유/무 두 QEMU 구성으로 검증 |
| [system-servers-bringup-m12-kernel-cow.md](done/system-servers-bringup-m12-kernel-cow.md) | M12 일부(선행): COW(프레임 참조 카운트+`clone_address_space_cow`+`#PF` 분기)+syscall 커널 스택 분리+`sys_fork`/`sys_process_spawn`/`sys_exec`/`sys_thread_exit`(ADR-140~142) 구현, initrun 자신을 fork/exec/spawn하는 종단간 QEMU 검증까지 완료. 그 과정에서 발견한 TSS 부재(ADR-143)·TLB 로컬 invlpg 누락(ADR-144)·pml4[0] 과도 공유(ADR-145) 버그 진단·수정 |
| [system-servers-bringup-m12-full.md](done/system-servers-bringup-m12-full.md) | M12(전체 완료): `sys_alloc_dma_buffer`(ADR-148)+legacy virtio-blk 클라이언트+self_info 브릿지 일반화(ADR-149)+initrun의 실제 부트 디스크 마운트·서비스 스폰+procsrv 골격+`tools/mkbootdisk.py`(ADR-150), procsrv가 실제 디스크 I/O로 스폰된 뒤 자기 자신을 fork/exec하는 것까지 QEMU 확인 — system-servers-bringup.md §M12 완료 |
| [system-servers-bringup-m13.md](done/system-servers-bringup-m13.md) | M13(완료): IPC의 cross-process 확장+sys_reply handles[](ADR-151), sys_process_spawn 캐패빌리티 주입(ADR-152), fs-protocol.md 최소 버전+vfs/memfs 서버+procsrv VFS 클라이언트(ADR-153) — procsrv가 vfs 경유로 memfs에 쓰고 다시 읽어 내용 일치까지 QEMU 확인 — system-servers-bringup.md §M13 완료 |
| [system-servers-bringup-m14.md](done/system-servers-bringup-m14.md) | M14(완료): 스레드별 IOPB(ADR-154)+sys_map_phys MMIO 캐패빌리티(ADR-156)+devmgr 유저랜드 ACPI/PCIe ECAM 열거+PS/2 컨트롤러 자체테스트+USB xHCI 리셋·포트 스캔(ADR-157) — 실제 USB 장치 열거/HID는 범위 밖으로 명시 — system-servers-bringup.md §M14 완료 |
| [system-servers-bringup-m15.md](done/system-servers-bringup-m15.md) | M15(완료): `servers/drivers/virtio-blk`(첫 실제 유저 드라이버)+devmgr BAR 재사용 일반화+부트 디바이스 배제(ADR-158), 그 과정에서 발견한 디스크 손상 버그(테스트 드라이버가 부트 디바이스에 그대로 쓰기 테스트해 cpio 아카이브 손상)와 avail_idx 리셋 버그 진단·수정 — system-servers-bringup.md §M15 완료 |
| [system-servers-bringup-m16.md](done/system-servers-bringup-m16.md) | M16(완료): IPC pages[]의 유저 프로세스 매핑 경로 구현(ADR-159/161)+캐패빌리티 슬롯 경계 검증(ADR-160)+FAT32/ext4 읽기전용 FS 서버+VFS 정적 마운트 테이블+fs-protocol v2(ADR-162), 그 과정에서 발견한 devmgr I/O BAR 동시 배정 충돌 버그 진단·수정(ADR-163) — system-servers-bringup.md §M16 완료 |
| [system-servers-bringup-m17.md](done/system-servers-bringup-m17.md) | M17(완료): VGA 텍스트 콘솔+ps2 진짜 IPC 서버 전환+procsrv 최초 서버화(OP_LOGIN)+login 서버(ADR-164/165), 그 과정에서 발견한 initrun 서비스 레지스트리 크기 초과·ps2 폴링 예산 과다 버그 진단·수정(ADR-166) — system-servers-bringup.md §M17 완료 |
| [system-servers-bringup-m18.md](done/system-servers-bringup-m18.md) | M18(완료): fs-protocol v3(OP_WRITE도 pages[] 기반+memfs 커서+OP_OPEN 신원 필드, ADR-168)+procsrv 위임 테이블/OP_SU/실제 경로→ELF 로더+VFS guest/jail 홈 격리(ADR-167) — system-servers-bringup.md §M18 완료 |
| [system-servers-bringup-m19.md](done/system-servers-bringup-m19.md) | M19(완료): cfgsrv 설정 리포지터리 신설(주소 체계/타입 KV/Unix RWX 권한/reg_op 9종/VFS 실제 영속화, ADR-169)+procsrv 왕복·권한 모델 양방향 검증, 그 과정에서 발견한 elf_loader 세그먼트-페이지 공유 미처리 버그 진단·회피 — system-servers-bringup.md §M19 완료 |
| [system-servers-bringup-m20.md](done/system-servers-bringup-m20.md) | M20(완료, 계획 최종 마일스톤): libmc 최소 부분집합 신설(ADR-170)+minicore 전용 대체 셸(userland/shell, ls/cat 빌트인)+procsrv→셸 OP_START 세션 시작(ADR-171)+memfs OP_LIST(fs-protocol v4, ADR-172) — 실제 서드파티 libc/셸 포팅은 이번 라운드에 하지 않음(명시적으로 남긴 갭) — system-servers-bringup.md §M20 완료, 계획 전체(M12~M20) 완료 |
| [real-hardware-boot-verification.md](done/real-hardware-boot-verification.md) | 마일스톤 외 확인 작업(완료): ADR-114가 미검증으로 남겨 뒀던 실제 GRUB Multiboot2 부팅 경로를 Docker 기반 grub-mkrescue ISO로 처음 검증, 그 과정에서 발견한 레거시 8259 PIC 미마스킹으로 인한 IRQ0/#DF(vector 8) 벡터 충돌 버그 진단·수정(ADR-173) — M1~M20 공식 스모크 테스트 82개 문자열 전부 실제 GRUB 경로에서 재확인 |

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
