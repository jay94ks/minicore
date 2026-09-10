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
| [fs-protocol.md](spec/fs-protocol.md) | FS 서버 공통 프로토콜(M20 v4): open/write/read/list 오퍼레이션, OP_OPEN 호출자 신원 필드, pages[] 기반 wire 포맷, 상태 코드 |

### spec/generated — 자동 생성 와이어 프로토콜 참조표 (ADR-195, 손으로 고치지 않는다)
| 문서 | 설명 |
|---|---|
| [generated/procsrv-wire.md](spec/generated/procsrv-wire.md) | `tools/gen-wire-docs.py`가 `libs/mc/include/mc/procsrv_protocol.h`의 `@wire-op` 마크업에서 추출(M27) — wait/kill/exit_report |

## plan — 실행 계획 (실행 전)
| 문서 | 설명 |
|---|---|
| [scaffold-repo-skeleton.md](plan/scaffold-repo-skeleton.md) | 저장소 디렉토리·CMake 골격 생성 계획 (실행 완료, 결과는 done 참고) |
| [kernel-bootstrap.md](plan/kernel-bootstrap.md) | x86_64 부팅→IPC→initrun 최초 수직 슬라이스 마일스톤 계획 (M1~M8 전부 완료 — 결과는 done 참고) |
| [smp-fpu-bringup.md](plan/smp-fpu-bringup.md) | M9(FPU/SIMD 컨텍스트 스위칭)~M11b(lazy XSAVE/AVX 전환) — AP 기동·IPI·TLB shootdown(M10), 다중 코어/NUMA 검증+락 순서 문서화(M11) 포함, kernel-bootstrap.md 이후 계획 |
| [system-servers-bringup.md](plan/system-servers-bringup.md) | M12(procsrv)~M20(libc 포팅+로그인 후 셸) — VFS/memfs, devmgr+PCIe+PS/2+USB, virtio-blk, FAT32+ext4, 콘솔/로그인, 보안 모델(su/sudo/jail), cfgsrv 순. libmc(네이티브 C API 라이브러리)가 전 구간 교차 트랙. **M12~M20 전부 완료** — 결과는 done 참고 |
| [general-purpose-completion.md](plan/general-purpose-completion.md) | M21(선점형 스케줄링)~M26(실제 libc 포팅 재도전) — 프로세스 생명주기(wait/시그널), 진짜 fork/exec의 fd 상속, 유저랜드 동적 메모리, 최소 네트워킹까지. aarch64 이식보다 먼저 하기로 결정된 계획. **전체(M21~M26) 완료** — 결과는 done 참고. 이 계획에는 더 이상 다음 마일스톤이 없다 |
| [libs-restructure.md](plan/libs-restructure.md) | M49(libk/libmc를 libs/k, libs/mc로 이동+lib 접두사 제거)~M50(uapi.hpp 폐지, mc로 흡수+MC_LAND_KERNEL 매크로) — [build-system.md](../design/build-system.md) ADR-199, [foundations.md](../design/foundations.md) ADR-200(ADR-132 보강)의 실제 적용. M50이 namespace-refactor.md의 M45(uapi→kern::proto 단순 리네임)를 대체한다. **전체(M49~M50) 완료**(결과는 done 참고) — 이 계획에는 더 이상 다음 마일스톤이 없다 |
| [namespace-refactor.md](plan/namespace-refactor.md) | M44(kern:: 최상위+커널 코어 리네임)~M48(kernsrv::proto 분리) — [foundations.md](../design/foundations.md) ADR-198(네임스페이스 컨벤션 규칙 확정)의 실제 적용. 순수 기계적 리네임(동작 변화 없음), 각 마일스톤 5개 QEMU 스위트 회귀 없음 확인. **전체 완료**(M45는 libs-restructure.md M50으로 대체돼 스킵) — 결과는 done 참고. 이 계획에는 더 이상 다음 마일스톤이 없다 |
| [user-service-manager.md](plan/user-service-manager.md) | M40(servers/svcmgr 골격+재부모화 완성)~M43(계정별 유저 서비스 인스턴스, 스트레치) — OPEN-51이 남긴 "유저 서비스 등록 프로토콜" 실제 구현. ADR-196(유닛 모델+시작 절차+컨트롤 프로토콜 개요), ADR-197(@global/system/services 레지스트리 스키마, 새 프로토콜 없이 기존 reg_op 재사용). svcmgr는 순수 minicore 네이티브 서버라 real-libc-syscall-layer.md와 독립적으로 진행 가능(M27만 선행 전제). **착수 전(계획만 존재)** |
| [real-libc-syscall-layer.md](plan/real-libc-syscall-layer.md) | M27(procsrv 실제 프로세스 테이블)~M39(실제 서드파티 셸/coreutils 재포팅 시도, 스트레치) — OPEN-54·62·63·65·66 해소(54·65는 완료, 나머지 진행 중). ADR-183(syscall 번역: 커널 확장 대신 musl의 syscall_arch.h 패치, libmc를 항상 거침), ADR-189(동적 링킹을 M29로 앞당김, musl 자신의 공유 libc.so), ADR-184(LAPIC 타이머 PIT/HPET 보정, 모든 타이머는 유저모드 진입 전 보정), ADR-185/191(진짜 멀티코어 선점+timer_source_interface 추상화), ADR-188(locale), ADR-186(signal 전달, SIGKILL 즉시 unlink), ADR-187(pthread), ADR-190(minicore 타깃 SDK 내보내기), ADR-195(와이어 프로토콜 마크업+추출 도구, M27 선행 작업), ADR-202(M28 실행 중 발견 — arch_prctl/FS_BASE와 Linux ABI 초기 스택을 M28로 앞당김). **M27~M28 완료**(결과는 done 참고, ADR-201/202) — M29부터 진행 중 |

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
| [real-hardware-boot-verification.md](done/real-hardware-boot-verification.md) | 마일스톤 외 확인 작업(완료): ADR-017/114가 미검증으로 남겨 뒀던 실제 부팅 경로 둘 다 처음 검증 — (1) GRUB Multiboot2(Docker grub-mkrescue ISO), 레거시 8259 PIC 미마스킹으로 인한 IRQ0/#DF 벡터 충돌 버그 진단·수정(ADR-173); (2) UEFI(Docker OVMF), 별도 PE32+ EFI 스텁을 처음부터 구현(ADR-175)하며 retf/스택 순서 버그와 initrun/devmgr의 오래된 arch_data_addr=0 버그(ADR-174, 세 경로 전부에 영향) 진단·수정 — 두 경로 모두 M1~M20 공식 스모크 테스트 82개 문자열 기준 동일한 최종 상태 도달 확인(UEFI는 OVMF 고유의 USB xHCI BAR 이슈만 알려진 제약으로 남김) |
| [general-purpose-completion-m21.md](done/general-purpose-completion-m21.md) | M21(완료): LAPIC 타이머 기반 선점형 스케줄링(ADR-176, BSP·ring3 한정) — 새 IDT 벡터+`sched::on_timer_tick()`+`run_queue::lock`을 `irq_safe`로 승격, `init/preempt_demo/`(busy/counter)로 QEMU 실측 검증. 이 과정에서 TSS.RSP0 전역 공유 버그를 발견·수정(ADR-177, M12 ADR-141과 같은 문제 형태) — 스모크/SMP/NUMA/AVX 4개 스위트 전부 회귀 없음 확인 |
| [general-purpose-completion-m22.md](done/general-purpose-completion-m22.md) | M22(완료): 프로세스 생명주기 최소 구현(ADR-178) — `sys_process_kill`(스케줄러가 대상을 다음에 뽑으려는 시점에 폐기)+procsrv가 자기 자신을 wait/kill 타깃으로 재스폰하는 자기테스트. wait는 procsrv의 기존 공유 로그인 endpoint 대신 자식 전용 새 endpoint로 방향을 뒤집어 servers/login의 큐잉된 Call과의 충돌을 해결(실제 재현) — procsrv.md의 완전한 프로세스 테이블/외부 OP_WAIT·OP_KILL 프로토콜은 범위 밖(OPEN-64) |
| [general-purpose-completion-m23.md](done/general-purpose-completion-m23.md) | M23(완료): 진짜 fork/exec 최소 구현(ADR-179) — `sys_fork`가 handle_table 전체를 프록시로 복제(이전에는 자식이 빈 테이블로 시작). procsrv가 test.txt를 부분 읽고 fork+exec한 완전히 새 이미지에서 나머지를 이어 읽어 확인 — open_file_id가 서버측(memfs) 상태이므로 procsrv 매개 fd 프로토콜(procsrv.md §3.6/§4.1) 없이도 오프셋 공유가 성립함을 실측 확인 |
| [general-purpose-completion-m24.md](done/general-purpose-completion-m24.md) | M24(완료): 유저랜드 동적 메모리(ADR-180) — 새 syscall `sys_brk`(고정 1MiB 힙 슬롯, 슬롯 5)+libmc 최소 malloc(`mc_malloc`, 순수 범프 할당자). userland/shell의 cat 빌트인이 스택 배열 대신 malloc 버퍼를 실제로 써서 파일을 정확히 읽어냄을 확인 |
| [general-purpose-completion-m25.md](done/general-purpose-completion-m25.md) | M25(완료): 최소 네트워킹(ADR-181) — virtio-net 드라이버(legacy virtio, virtio-blk와 같은 레지스터 레이아웃 재사용)+처음으로 실제 코드를 채운 netsrv(이더넷/IP/UDP 프레이밍). ARP 없이 DHCPDISCOVER→DHCPOFFER 왕복으로 UDP 검증(QEMU SLIRP 내장 DHCP 서버 이용) — vring 베이스 오프셋 누락+RX 버퍼 페이지 정렬 버그 기록, `tools/smoke-test-net-x86_64.sh` 신설 |
| [general-purpose-completion-m26.md](done/general-purpose-completion-m26.md) | M26(완료, 계획 최종 마일스톤): 실제 libc 포팅 재도전(ADR-182) — third_party/musl(v1.2.6, 이 저장소의 첫 실제 git submodule)의 문자열 함수 부분집합(재구현이 아니라 원본 소스)을 실제로 빌드해 셸이 링크·사용. strdup→mc_malloc 연결(libc/sysdeps/minicore/mem_shim.c), 전체 syscall 계층/동적 링커/스레드/stdio는 범위 밖(OPEN-66) — general-purpose-completion.md 전체(M21~M26) 완료 |
| [namespace-refactor-m44.md](done/namespace-refactor-m44.md) | M44(완료): `kern::` 최상위 도입 — `object`/`ipc`/`mm`/`sched`/`klog`/`initrd` 6개 네임스페이스를 `kern::*`로 리네임(48개 파일). 실행 중 `boot`(boot_info.hpp)가 초기부터 실제로 initrun과 공유되는 ABI 헤더임을 발견해 리네임 대상에서 제외(ADR-198 매핑표 정정) — 빌드+5개 QEMU 스위트 전부 회귀 없음 확인 |
| [namespace-refactor-m46.md](done/namespace-refactor-m46.md) | M46(완료): `arch_x86_64`→`kern::arch::x86_64` 리네임(33개 파일). `kern::proc` 분리는 비용 대비 가치 부족으로 보류 결정(process_ops.*는 그대로 kern::arch::x86_64에 유지) — 5개 QEMU 스위트 전부 회귀 없음 |
| [namespace-refactor-m47.md](done/namespace-refactor-m47.md) | M47(완료): `kernsrv::` 도입 — 서버 14개 전부를 `kernsrv::<서버명>`으로 감쌈(무네임스페이스였으므로 리네임이 아니라 추가). 전부 `extern "C" _start` 관례라 엔트리 포인트 문제 없었음 — 5개 QEMU 스위트 전부 회귀 없음 |
| [libs-restructure-m49.md](done/libs-restructure-m49.md) | M49(완료): `libk`/`libmc`를 `libs/k`/`libs/mc`로 이동, `#include <libk/`→`#include <k/` 26개 파일 치환(`mc`는 내부 세그먼트가 이미 `mc`였어서 소스 변경 없음). CMake 타깃 이름은 유지. 실행 중 aarch64 빌드 실패(M3 시절부터 소스 자체가 없던 무관한 기존 상태)를 회귀와 구분해 확인 — x86_64 빌드+5개 QEMU 스위트 전부 회귀 없음 |
| [namespace-refactor-m48.md](done/namespace-refactor-m48.md) | M48(완료, 계획 최종 마일스톤): `kernsrv::proto` 분리 — netsrv의 IEEE/IANA/RFC 표준 상수 5개만 이동(헤더 구조체 자체는 원래 없었음). 중첩 네임스페이스 정의 함정 두 번 겪고 "netsrv 열기 전에 proto 먼저 정의+using" 방식으로 해결 — namespace-refactor.md 전체(M44/M46~M48, M45는 대체) 완료 |
| [libs-restructure-m50.md](done/libs-restructure-m50.md) | M50(완료, 계획 최종 마일스톤): `kernel/include/uapi.hpp` 폐지 — `libs/mc/include/mc/syscall.h`를 커널·유저 공용 syscall ABI 단일 출처로 재작성하고 `MC_LAND_KERNEL` 매크로로 커널-랜드/유저-랜드를 구분(ADR-200). 실행 중 발견한 유저랜드 `memset` 미정의 링크 에러(트리비얼 aggregate `{}` 초기화가 memset 호출로 낮춰짐)를 `libs/mc/src/freestanding_mem.c` 신설+15개 유저 실행파일에 `minicore_libmc` 링크 추가로 해결 — libs-restructure.md 전체(M49~M50) 완료 |
| [real-libc-syscall-layer-m27.md](done/real-libc-syscall-layer-m27.md) | M27(완료): procsrv 실제 process_entry 테이블(pid/parent_pid/thread_handle/state/exit_code)+범용 `proc_op::wait`/`kill`/`exit_report`(`mc/procsrv_protocol.h`, ADR-195 마크업 최초 실전 적용, `tools/gen-wire-docs.py` 신설, OPEN-54 해소)+재부모화 메커니즘 증명(ADR-192 §결정3, 합성 pid). M22와 달리 procsrv 전용 endpoint가 아니라 pid로 식별되는 임의의 두 유저 프로세스(A/B/C) 사이의 왕복 — caller_pid 자기주장+비블로킹 폴링 wait로 범위 좁힘(ADR-201, OPEN-67 신설). 실행 중 parent_pid 등록 순서 버그(A의 pid를 먼저 할당해야 wait 권한 검사가 통과함) 진단·수정 |
| [real-libc-syscall-layer-m28.md](done/real-libc-syscall-layer-m28.md) | M28(완료): `tools/apply-patches.sh` 실제 구현(patch(1) 기반, git apply의 core.autocrlf 정규화가 패치를 "Skipped patch"로 조용히 무시하는 버그를 겪고 우회)+`third_party/patches/musl/0001-syscall-shim.patch`(syscall_arch.h의 __syscallN을 syscall_shim.c로 우회)+`userland/musl-hello`(musl 진짜 시작 경로로 진입해 "hello from real musl" 출력, 최초의 실제 musl 링크 프로그램). 실행 중 발견(ADR-202): musl의 __init_tls가 arch_prctl(FS_BASE)을 무조건 요구하고 crt_arch.h가 Linux ABI 초기 스택(argc/argv/envp/auxv)을 요구함을 확인해 원래 M30/M29 계획이던 두 커널 기능(FS_BASE MSR 지원, mc_process_spawn_request::linux_abi_stack)을 M28로 앞당김 |

## design — 설계/상세

`docs/design/`는 문서가 많아 자체 색인을 따로 둔다.

| 문서 | 설명 |
|---|---|
| [design/index.md](design/index.md) | **설계 문서 전체 목록** — 주제별로 분리된 ADR 파일(ADR-001~, 항상 최신 번호까지)과 미결정 항목, repo-layout.md 안내 |

## remind — 기억 사항
| 문서 | 설명 |
|---|---|
| [doc-convention.md](remind/doc-convention.md) | 문서 디렉토리별 역할과 인덱싱 규칙 |

## reply — 답변 기록 (자동 생성, 미처리 답변함)
| 문서 | 설명 |
|---|---|
| [reply.md](reply.md) | `tools/docs-dashboard` 웹 대시보드의 "답변 입력" 탭에 입력한 내용을 기록하는 미처리 답변함. 최초 저장 시 자동 생성됨. 처리 규칙은 [CLAUDE.md](../CLAUDE.md) 참고 |
