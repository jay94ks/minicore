#!/usr/bin/env bash
# x86_64 커널 스모크 테스트: QEMU로 커널을 부팅해 시리얼 콘솔에서
# 마일스톤별 완료 기준 문자열을 확인한다.
#   M1 (docs/plan/kernel-bootstrap.md, debug-console.md §5): "hello from kernel"
#   M2 (boot.md §2~3): "[boot_info:selftest]" 자체 테스트 결과에
#     memory_map_count와 initrd_addr이 담겨 나온다 — 이 개발 머신에는
#     GRUB가 없어(ADR-114) 실제 Multiboot2("[boot_info:real]")는 항상
#     비어 있으므로 검증 대상이 아니다.
#   M3 (memory.md §2~4/§6): mm::init/alloc_pages/free_pages/slab_alloc이
#     왕복 성공하는지 확인한다. total_bytes/free_bytes의 정확한 16진수
#     값이나 페이지 물리주소는 커널 이미지 크기가 바뀔 때마다 달라지므로
#     여기서 검사하지 않는다 — 구조적으로 "성공했는가"만 본다.
#   M4 (objects.md §3~6): 핸들 생성·프록시 위임(권한 축소)·cascade
#     revoke·이중 close 감지, 페이지테이블 map/remap 거부/protect/unmap
#     왕복.
#   M5 (scheduler.md §1~3): 커널 스레드 2개가 yield()로 번갈아 실행됨
#     (협조적 라운드로빈, 타이머 선점 없음 — 아직 IDT가 없다).
#   M6 (ipc.md §3~5): 커널 스레드 2개 사이의 Call → Recv → Reply 왕복,
#     프록시 badge가 sys_recv까지 정확히 전파됨.
#   M7 (ipc.md §4/§7): 1페이지 데이터를 copy 모드로 전달(내용 검증
#     포함), IPC로 위임된 핸들이 실제로 쓸 수 있는 핸들임을 그 핸들로
#     직접 sys_wait해서 확인, sys_notify로 그 대기를 깨움.
#   M8 (boot.md §4~6, kernel-bootstrap.md 최종 완료 기준): MCPACK
#     initrd에서 initrun ELF를 찾아 로드하고, 새 주소공간(GDT 등
#     저지대 공유 매핑 포함)·핸들 테이블을 가진 유저 스레드로 SYSCALL/
#     SYSRET 기반 IRETQ 진입시켜, initrun이 SYSCALL로 보낸 IPC Call에
#     커널이 응답한다.
#   M9 (smp-fpu-bringup.md, ADR-127): 서로 다른 두 커널 스레드가 xmm0에
#     넣어 둔 값이 yield()를 여러 차례 거쳐도 서로 오염되지 않음을
#     확인한다(eager FXSAVE/FXRSTOR).
#   M10 (smp-fpu-bringup.md, ADR-055): IDT+ACPI MADT 파싱+LAPIC 구동이
#     BSP 단일 코어 경로에서도 항상 실행된다 — AP 트램폴린 스크래치
#     페이지(kernel/arch/x86_64/smp.hpp::k_ap_trampoline_phys)가
#     memory_map에 추가 엔트리로 잡혀 selftest의 memory_map_count가
#     4에서 5로 바뀐다(M1~M9는 4였다). SMP/AP 기동 자체의 검증은 opt-in
#     이라 별도 스크립트(tools/smoke-test-smp-x86_64.sh)로 분리한다.
#   M11b (smp-fpu-bringup.md, ADR-133): eager FXSAVE/FXRSTOR를 lazy
#     CR0.TS/`#NM` 트랩으로 개정했다 — thread_fpu_a/b는 여전히 값
#     보존을 확인하고(이제는 매 스위치가 아니라 실제로 FPU를 쓰는
#     순간에만 저장/복원됨), 추가로 thread_fpu_c(8회 반복, a/b보다
#     길게 산다)가 a/b가 먼저 끝난 뒤에는 소유자가 안 바뀌어 `#NM`
#     자체가 더 이상 발생하지 않음(owner_changed=0 또는 트랩 자체가
#     없음)을 보인다. AVX 유/무 두 QEMU 구성 검증은 opt-in
#     MINICORE_QEMU_CPU로 별도 확인한다(기본 QEMU CPU는 XSAVE/AVX가
#     없어 FXSAVE 폴백 경로를 그대로 검증한다).
#   M12 (system-servers-bringup.md, ADR-131/147/149): initrun이 실제
#     virtio-blk 부트 디스크(tools/mkbootdisk.py로 만든 이미지, 기본으로
#     자동 첨부 — 아래 MINICORE_QEMU_BOOTDISK 참고)의 PCI BAR를 스스로
#     배정하고, 그 디스크의 cpio 아카이브를 읽어 procsrv를
#     sys_process_spawn한 뒤, procsrv가 자기 자신을 sys_fork+sys_exec —
#     "[process] fork/exec/spawn ok" 로그는 이제 initrun의 self-test
#     데모가 아니라 **procsrv가 실제 디스크 I/O로 만들어진 뒤** 남기는
#     로그다(kernel/arch/x86_64/process_ops.cpp가 호출자를 구분하지
#     않고 남기는 로그라 문자열은 그대로 재사용된다).
#   M13 (system-servers-bringup.md, ADR-151/152, docs/spec/fs-protocol.md):
#     같은 부트 디스크가 이제 memfs+vfs+procsrv 세 서비스를 담는다
#     (의존 순서 memfs→vfs→procsrv, ADR-152의 스폰 시점 캐패빌리티
#     주입). procsrv가 vfs에 파일을 열고(vfs가 memfs에 위임) 그
#     응답으로 받은 memfs 핸들에 직접 쓰고 다시 읽어 내용이 일치함을
#     "[procsrv] vfs write/read roundtrip ok=1"로 확인한다(klog가
#     유저에 노출된 적이 없어 새 sys_debug_log syscall로만 관찰
#     가능 — uapi.hpp 참고).
#   M14 (system-servers-bringup.md, ADR-154/156, docs/spec/pcie.md):
#     부트 디스크에 devmgr/ps2/usb가 추가된다(의존 순서 devmgr→usb,
#     ps2는 devmgr와 무관 — ADR-130의 고정 레거시 프로브). devmgr가
#     ACPI RSDP/MCFG를 유저랜드에서 직접 파싱해(sys_map_phys, ADR-156)
#     PCIe bus 0을 ECAM으로 열거하고, ps2는 8042 컨트롤러 자체
#     테스트(사용자 입력과 무관하게 결정적)를 수행하며(sys_io_activate,
#     ADR-154), usb는 devmgr에게 등록해 위임받은 xHCI BAR를 매핑해
#     컨트롤러 리셋(HCRST)과 포트 상태(PORTSC) 스캔까지 시도한다 —
#     실제 USB 장치 열거/HID는 범위 밖(docs/done/
#     system-servers-bringup-m14.md 참고). QEMU에 `qemu-xhci`
#     컨트롤러를 기본으로 붙인다(MINICORE_QEMU_XHCI, 아래 참고).
#   M15 (system-servers-bringup.md, ADR-043 1순위): 부트 디스크에
#     virtio-blk 드라이버가 추가된다(의존 devmgr, ADR-041 등록/매칭).
#     devmgr에 vendor:device=0x1af4:0x1001로 등록해 위임받은 I/O
#     포트로 initrun의 부트 디바이스와 **같은 물리 장치**를 다시
#     초기화하고(devmgr가 이미 배정된 BAR를 그대로 재사용, ADR-157
#     갱신) 임의 섹터에 알려진 패턴을 쓰고 다시 읽어 내용이
#     일치하는지 확인한다 — initrun의 부트 목적은 이미 끝나 있어
#     안전하다.
#   M16 (system-servers-bringup.md, ADR-057/ADR-129): 부트 디스크에
#     fat32/ext4 서버가 추가된다(둘 다 의존 devmgr, 각자 자신의
#     virtio-blk-pci 장치를 등록해 마운트). vfs가 이제 마운트
#     테이블(fs-protocol.md v2 §2.1)로 `/mnt/fat32/`, `/mnt/ext4/`
#     접두사를 각 FS 서버에게 라우팅한다. IPC pages[]의 유저 프로세스
#     매핑 경로(ADR-159/161)가 처음으로 실전에서 쓰인다 — OP_READ
#     응답이 이제 이 경로로 온다(memfs도 이 라운드에서 함께
#     갱신했다). 호스트에서 미리 만든 각 이미지의 hello.txt를 열어
#     읽어낸 내용이 호스트가 심어 둔 내용과 일치하는지 확인한다.
#   M17 (system-servers-bringup.md, ADR-097/111/164, security-model.md
#     ADR-165): 부트 디스크에 console(VGA 텍스트 콘솔, 0xB8000)과
#     login이 추가된다(login은 console/ps2/procsrv 모두에 의존).
#     ps2가 이제 self-test 후 종료하지 않고 OP_READ_KEY를 받는 진짜
#     서버가 된다. procsrv도 이 라운드에서 처음으로 서버가 되어
#     OP_LOGIN(최소 계정 저장소, 평문 비교)에 응답한다. 실제 키 입력이
#     QEMU 자동화 환경에 주입되지 않으므로(ps2가 M14부터 겪은 것과
#     같은 제약), login은 폴링해도 키가 없으면 내장 자체 테스트
#     계정으로 같은 OP_LOGIN 경로를 그대로 검증한다.
#   M18 (system-servers-bringup.md, security-model.md ADR-167,
#     docs/spec/fs-protocol.md v3): procsrv가 자기 자신의 ELF를 VFS
#     경로(/bin/su-target)에 실제로 쓰고 다시 읽어 재조립한 뒤(경로→
#     ELF 로더, OP_WRITE도 이제 pages[] 기반) 원본과 일치하는지
#     확인한다. 그 재조립된 바이트로 procsrv 자신을 다시 스폰해
#     "su-target" 역할(magic 접두사 argv로 판별)을 태운다 — 하나는
#     guest 신원으로(VFS가 홈 밖 open을 거부하는지 확인), 다른 하나는
#     login이 OP_SU로 요청한 신원 전환(root로, procsrv 하드코딩
#     위임 테이블 덕에 비밀번호 없이 승인)으로. 위임이 없고 잘못된
#     비밀번호를 쓰는 두 번째 OP_SU 요청은 거부돼야 한다.
#   M20 (system-servers-bringup.md, docs/design/foundations.md ADR-170,
#     security-model.md ADR-171, filesystem.md ADR-172): 부트 디스크에
#     userland/shell이 추가된다(의존 vfs/console/ps2, procsrv보다
#     먼저 떠 있어야 한다). 셸은 부팅 즉시 자기 handle 1에서 블록하며
#     기다리다가, 로그인 성공 시 procsrv가 보내는 OP_START를 받아야만
#     프롬프트를 낸다("[shell] session started"/"[procsrv] shell
#     session start ok=1"). 실제 키 입력이 없는 자동화 환경에서는
#     ls/cat을 빌트인으로 한 번씩 결정적으로 실행해 검증한다(ls는
#     새 fs-protocol v4 OP_LIST로 memfs 파일 목록을, cat은 procsrv의
#     M13 자체 테스트가 만들어 둔 test.txt의 내용 "hello vfs"를
#     확인한다). 실제 서드파티 libc/셸 포팅은 이번 라운드에 하지
#     않는다(ADR-170 §결정1/2 — libmc 최소 부분집합 + minicore 전용
#     대체 셸).
#   M22 (general-purpose-completion.md §M22, kernel-scheduler.md
#     ADR-178): procsrv가 자기 자신을 두 번 더 재조립 스폰해(wait/kill
#     타깃, su-target과 같은 argv 마커 관례) 프로세스 생명주기를
#     검증한다. wait 타깃은 procsrv가 만들어 준 **자기 전용**
#     endpoint(create_endpoint=true — su-target/OP_START처럼 procsrv의
#     공유 로그인 endpoint를 다시 쓰면 이미 큐잉된 servers/login의
#     OP_LOGIN Call과 충돌한다, 실제로 재현·수정)에서 procsrv의 Call을
#     받아 exit_code(42)를 Reply로 돌려주고("[procsrv] wait exit_code
#     ok=1"), kill 타깃은 스스로 끝나지 않는 유한 반복(M21의 "무한
#     루프가 나머지 부팅을 굶긴다" 실수 재발 방지) 자식을
#     sys_process_kill로 강제 종료해 그 결과를 커널이 직접 로그로
#     남긴다("[procsrv] kill requested ok=1"/"[sched] thread killed
#     (discarded before scheduling)" — 스케줄러가 다음에 그 스레드를
#     뽑으려는 시점에 실제로 폐기한다).
#   M23 (general-purpose-completion.md §M23, kernel-memory.md
#     ADR-179): sys_fork가 이제 부모의 handle_table 전체를 자식에게
#     프록시로 복제한다(이전에는 자식이 빈 테이블로 시작했다).
#     procsrv가 test.txt를 열어 앞 5바이트("hello")만 읽어 서버 쪽
#     read_cursor를 옮겨 두고 fork한 뒤, 자식이 상속받은 그 handle+
#     open_file_id로 exec까지 마친(exec는 handle_table을 건드리지
#     않는다) 완전히 새 이미지에서 나머지 4바이트(" vfs")를 이어
#     읽어 정확히 일치함을 확인한다("[procsrv] fd inherited continue
#     read ok=1") — open_file_id가 커널 핸들/발신자 신원과 무관하게
#     서버 쪽(memfs)에 상태를 두므로, 별도의 procsrv 매개 fd 복제
#     프로토콜(procsrv.md §3.6/§4.1) 없이도 오프셋 공유가 성립한다.
#   M24 (general-purpose-completion.md §M24, kernel-memory.md
#     ADR-180): 새 syscall sys_brk(고정 1MiB 힙 슬롯, 슬롯 5) +
#     libmc의 최소 malloc(mc_malloc, 순수 범프 할당자 — free는
#     no-op)를 셸의 cat 빌트인이 실제로 쓴다(스택 배열 대신
#     mc_malloc으로 받은 버퍼). 버퍼 확보 자체("[shell] malloc
#     buffer ok=1")와 그 버퍼로 읽은 파일 내용이 여전히 정확함
#     ("[shell] cat ok=1", 기존 M20 검증과 동일 기준)을 함께 확인해
#     유저랜드 동적 메모리 왕복을 증명한다.
#   M26 (general-purpose-completion.md §M26, foundations.md
#     ADR-182): third_party/musl(v1.2.6 고정, git submodule)의
#     문자열 함수 부분집합(memcpy/strlen/strcpy/strcat/strdup 등,
#     재구현이 아니라 원본 소스를 그대로 빌드)을 셸이 실제로 링크해
#     쓴다. musl 자신의 malloc 참조는 libc/sysdeps/minicore/mem_shim.c
#     가 M24의 mc_malloc(sys_brk)으로 연결한다. strcpy+strcat으로
#     문자열을 조립하고 musl의 strlen/memcmp/strdup으로 왕복 검증한다
#     ("[shell] libc strcpy/strcat/strdup ok=1"). 실제 syscall 계층
#     (open/read/write 등)·동적 링커·스레드까지의 완전한 포팅은 범위
#     밖이다(ADR-182 "알려진 단순화" — M20/ADR-170이 미뤄 둔 것의
#     연장, 이번에도 전체가 아니라 검증 가능한 부분집합만).
#   M28 (real-libc-syscall-layer.md §M28, foundations.md ADR-183):
#     third_party/patches/musl/0001-syscall-shim.patch(tools/apply-patches.sh
#     로 처음 실전 적용)가 musl의 arch/x86_64/syscall_arch.h를 패치해
#     모든 __syscallN을 libc/sysdeps/minicore/syscall_shim.c의
#     __minicore_syscall_dispatch로 우회시킨다. userland/musl-hello가
#     musl의 진짜 시작 경로(crt1.c→__libc_start_main.c→__init_tls.c)를
#     거쳐 main()에 진입해 musl 자신의 write()/_exit()로 종료한다
#     ("hello from real musl"). __init_tp가 무조건 요구하는
#     arch_prctl(ARCH_SET_FS)는 원래 M30 계획이었으나 이 마일스톤으로
#     앞당겨 커널에 실제 FS_BASE MSR 지원(kern::arch::x86_64::sync_fs_base,
#     새 syscall MC_SYSCALL_ARCH_PRCTL_SET_FS)을 추가했다. 커널의
#     기존 spawn 경로(initrun/서버들)는 전혀 안 쓰는 Linux ABI 초기
#     스택(argc/argv/envp/auxv)도 이번에 처음 구현했다
#     (mc_process_spawn_request::linux_abi_stack, musl-hello 전용).
#     구현하지 않은 syscall(예: SYS_set_tid_address=218)은 조용히
#     무시되지 않고 반드시 "[syscall_shim] unimplemented n=" 로그를
#     남긴 뒤 -ENOSYS를 반환한다(musl은 이 실패를 무시하고 계속
#     진행하도록 설계돼 있어 크래시하지 않는다).
#   M32 (real-libc-syscall-layer.md §M32): 프로세스 syscall — 진짜
#     musl fork()/execve()/waitpid()/getpid()가 procsrv의 새
#     self_register/fork_register 오퍼레이션(mc/procsrv_protocol.h
#     label=13/14)을 왕복한다("musl getpid ok=1" — procsrv가 이미
#     자신의 여러 self-test로 pid를 소비해 둬서 정확한 숫자는 매번
#     달라질 수 있어 값 자체는 확인하지 않는다). fork()의 자식이 execve()로
#     **다른** 실행 이미지(userland/musl-exec-target — procsrv가
#     부팅 시 VFS에 미리 써 둔다, run_exec_target_seed())를 실행하고
#     ("musl fork ok=1"), 부모가 waitpid()로 그 고유한 exit code(42)
#     를 회수한다("musl fork+exec+wait ok=1"). SYS_execve를 지원하려면
#     exec_current()(kernel/arch/x86_64/process_ops.cpp)도 M28의
#     linux_abi_stack 경로를 받아야 했다(M28 시점엔 "exec()은 이번
#     라운드에 지원하지 않는다"로 미뤄 뒀던 부분 — mc_exec_request에
#     linux_abi_stack 필드 추가). SYS_wait4는 pid>0(특정 자식)만
#     지원한다(pid<=0의 "임의의 자식" 의미론은 범위 밖 — 2026-09-10
#     사용자 확인, fork/clone 범위 좁힘과 같은 결정).
#   M35 (real-libc-syscall-layer.md §M35, foundations.md ADR-188):
#     musl locale — "C"/"POSIX" 고정만 검증한다. setlocale(LC_ALL, "")
#     는 POSIX 관례상 항상 성공해야 한다("musl setlocale empty
#     ok=1"). 계획 문서 원문은 "ko_KR.UTF-8 같은 미지원 로케일 요청은
#     실패(NULL)해야 한다"고 적어 뒀지만, third_party/musl/src/locale/
#     locale_map.c::__get_locale()을 실제로 읽어 보면 musl은 알 수
#     없는 로케일 이름도 실패시키지 않는다(malloc 실패나 '/'·선행
#     '.'이 있는 이름만 진짜로 실패한다) — 그래서 이 라운드는 "요청
#     자체는 성공하지만("musl setlocale unknown name ok=1") ctype
#     동작은 전혀 안 바뀐다("musl locale ctype still C ok=1")"로
#     검증 목표를 조정했다(ADR-210, docs/done 참고). 새 링크 의존성
#     (setlocale/locale_map/c_locale/__mo_lookup/getenv/toupper 등)
#     중 __map_file(MUSL_LOCPATH 환경변수 탐색 — envp가 항상 비어
#     있어 실제로는 안 타는 경로)은 원본 대신 항상 실패하는
#     sysdeps/minicore/locale_shim.c로 대체했다(fstat 등 안 쓸 의존성
#     을 새로 끌어올 이유가 없다).
#   M31 (real-libc-syscall-layer.md §M31, kernel-memory.md ADR-183): 파일
#     I/O syscall(SYS_open/openat/read/readv/close/writev, VFS/FS
#     프로토콜은 이미 있는 libmc의 mc_vfs_open/mc_fs_read를 그대로
#     재사용)+진짜 musl stdio(fopen/fread/fclose/printf, 재구현이
#     아니라 musl 소스 자체)를 musl-hello가 처음 실전에 쓴다.
#     musl-hello를 부트 초기(VFS가 아직 없는 시점)에서 initrun이
#     관리하는 15번째 서비스(--depends=musl-hello:vfs,
#     --linux-abi-stack=musl-hello, servers/CMakeLists.txt)로
#     옮겼다 — VFS를 거쳐 procsrv의 기존 자기테스트
#     (run_vfs_roundtrip_test)가 이미 써 둔 "test.txt"("hello vfs")
#     를 진짜 fopen/fread로 열어 읽고 printf로 확인한다("musl fopen
#     ok=1"/"musl fread content ok=1"/"musl printf read: hello vfs
#     (9 bytes)"). SYS_lseek/SYS_fstat/SYS_ioctl은 스텁(항상 실패 —
#     이 순차 읽기 테스트는 요구하지 않는다, SYS_ioctl 실패는 musl
#     stdio가 stdout을 완전 버퍼링으로 판정하는 데 필요). vfprintf.c
#     (printf의 %f/%e/%g 등)가 SysV 관례대로 double을 XMM0로
#     반환해 SSE가 필요해졌다 — 이 파일 하나만 SSE를 켠다(M9~M11b가
#     이미 완성한 유저 스레드별 FPU/SSE 컨텍스트 스위칭 덕분에
#     안전하다).
#   M30 (real-libc-syscall-layer.md §M30, kernel-memory.md ADR-183):
#     musl 자신의 진짜 malloc(lite_malloc.c, 순수 SYS_mmap 기반 범프
#     할당자)이 M26의 손으로 짠 mem_shim.c를 완전히 대체한다.
#     musl-hello가 malloc(64)+memcpy+memcmp+free()로 왕복하고
#     ("musl malloc ok=1"/"musl malloc content ok=1"), 잘못된 fd로
#     write()를 호출해 errno가 실제로 EBADF로 설정됨을 확인한다
#     ("musl errno ok=1" — M28의 arch_prctl/FS_BASE가 이 TLS 경로를
#     이미 갖춰 뒀다). musl의 malloc이 요구하는 SYS_brk를 항상
#     실패로 답해(syscall_shim.c) 무조건 SYS_mmap 경로로 우회시킨다
#     — libmc의 mc_malloc(ADR-180, 셸이 직접 쓴다)이 이미 sys_brk의
#     같은 커널 상태(heap_top)를 쓰고 있어, musl의 malloc도 같은
#     것을 공유하면 서로의 캐시된 커서가 어긋나 겹칠 수 있다는
#     것을 실행 전 분석으로 발견해 완전히 분리된 새 영역
#     (mmap_top, kernel_objects.hpp)으로 피했다.
#   M27 (real-libc-syscall-layer.md §M27, security-model.md ADR-201):
#     procsrv가 실제 process_entry 테이블(pid 발급, parent_pid)을
#     처음으로 갖는다. procsrv가 스폰하는 세 프로세스 C(kill 대상)→
#     B(자식)→A(부모) 중, A가 mc/procsrv_protocol.h의 범용
#     proc_op::wait/kill(mc/syscall.h가 아니라 이 M27이 처음 도입한
#     별도 프로토콜 헤더, ADR-195 마크업 최초 실전 적용)로 B의 pid를
#     물어 exit_code(77)를 회수하고("[procsrv] m27 wait exit_code
#     ok=1"), C의 pid로 kill을 요청해 성공을 확인한다("[procsrv] m27
#     kill ok=1") — M22의 wait/kill 자기테스트와 달리 procsrv 자신의
#     전용 endpoint가 아니라 **pid로 식별되는 임의의 두 유저 프로세스
#     사이**의 왕복이다(caller_pid는 자기주장 값, badge 검증은 범위
#     밖 — done 보고 참고). 재부모화 메커니즘(ADR-192 §결정3, 대상은
#     매개변수)도 합성 pid로 별도 증명한다("[procsrv] reparent
#     mechanism ok=1").
#   M19 (system-servers-bringup.md, registry-decisions.md ADR-060~064/169):
#     부트 디스크에 cfgsrv가 추가된다(의존 vfs, procsrv는 이제
#     vfs+cfgsrv 둘 다에 의존). procsrv가 cfgsrv에 "@global/test/settings"
#     테이블을 만들고 값을 쓰고 다시 읽는 왕복을 확인한 뒤("[procsrv]
#     cfgsrv roundtrip ok=1"), 소유자가 아닌 uid는 기본 비공개
#     테이블을 열 수 없고("permission denied before grant=1")
#     소유자가 set_permissions로 열어 주면 그 뒤엔 읽을 수
#     있음("permission granted after chmod=1")을 확인해 권한 모델
#     (owner/other RWX, group은 이번 라운드 범위 밖)이 실제로 동작함을
#     보인다. 나머지 프로토콜(list_values/list_children/delete_value/
#     delete_table)도 한 번씩 행사해 9종 전부를 확인한다
#     ("full protocol ok=1"). cfgsrv는 자신의 상태를 VFS 경로
#     (/sys/etc/registry.dat, memfs로 기본 라우팅)에 실제로 쓴다
#     ("[cfgsrv] persist write ok=1" — procsrv 왕복 도중 여러 번
#     나온다).
#
# 커널은 아직 종료 수단이 없어 hlt 루프에서 영원히 멈춰 있으므로, 고정
# 시간 뒤 QEMU를 강제 종료하고 그때까지 나온 로그를 검사한다.
#
# 사용법: tools/smoke-test-x86_64.sh [빌드 디렉토리(기본: build/x86_64-clang)]
#
# 환경 변수:
#   MINICORE_QEMU_BIN        qemu-system-x86_64 실행파일 경로(run-qemu.sh로 그대로 전달)
#   MINICORE_QEMU_BOOTDISK   기본값은 <빌드 디렉토리>/servers/bootdisk.img
#                            (servers/CMakeLists.txt가 memfs+vfs+procsrv+devmgr+
#                            ps2+usb+virtio-blk를 담아 만든다, M13~M15) — M12부터 항상 실제
#                            부트 디스크를 붙여야 procsrv 경로가 검증되므로 비워
#                            두면 이 스크립트가 그 경로를 채워 넣는다. 그 파일이
#                            없으면(아직 빌드 안 함) 디스크 없이 부팅하고 그만큼의
#                            어써션은 실패한다 — 먼저
#                            `cmake --build <빌드 디렉토리> --target minicore_bootdisk_image`.
#   MINICORE_QEMU_XHCI       기본값 1(M14부터 usb 드라이버 검증을 위해 항상 켠다) —
#                            0으로 설정하면 xHCI 컨트롤러 없이 부팅한다(usb 관련
#                            어써션은 실패한다).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-build/x86_64-clang}"
TIMEOUT_SEC=120  # M17부터 부트 디스크가 11개 서비스(memfs/devmgr/ps2/console/usb/virtio-blk/fat32/ext4/vfs/procsrv/login)를 담는다. M18은 서비스 수는 그대로지만 su-target 스폰 2회+로더 IPC 왕복이 추가돼 여유를 더 둔다. M19는 12번째 서비스(cfgsrv)와 procsrv↔cfgsrv IPC 왕복(9종 오퍼레이션+VFS 영속화 쓰기 여러 번)이 늘어 여유를 더 둔다. M20은 13번째 서비스(shell)+procsrv→셸 OP_START IPC+셸의 ls/cat 자체 테스트가 늘어 여유를 더 둔다.

if [[ -z "${MINICORE_QEMU_BOOTDISK:-}" ]]; then
  DEFAULT_BOOTDISK="${BUILD_DIR}/servers/bootdisk.img"  # M16부터 memfs+devmgr+ps2+usb+virtio-blk+fat32+ext4+vfs+procsrv(servers/CMakeLists.txt).
  if [[ -f "$DEFAULT_BOOTDISK" ]]; then
    export MINICORE_QEMU_BOOTDISK="$DEFAULT_BOOTDISK"
  fi
fi

# M14 — usb 드라이버가 실제로 찾을 xHCI 컨트롤러도 기본으로 붙인다
# (run-qemu.sh 상단 주석 참고).
if [[ -z "${MINICORE_QEMU_XHCI:-}" ]]; then
  export MINICORE_QEMU_XHCI=1
fi

# M15 — virtio-blk 드라이버가 쓰고 읽을 부트 디스크와 별도인 테스트
# 디스크도 기본으로 붙인다(run-qemu.sh 상단 주석 참고 — 부트
# 디스크를 재사용하면 그 cpio 아카이브 내용을 실제로 덮어써 손상시킨다).
if [[ -z "${MINICORE_QEMU_TESTDISK:-}" ]]; then
  export MINICORE_QEMU_TESTDISK="${BUILD_DIR}/testdisk.img"
fi

# M16 — fat32/ext4 서버가 마운트할, 미리 내용을 심어 둔 이미지 두
# 개도 기본으로 붙인다(run-qemu.sh 상단 주석 참고). 없으면
# make-fs-test-images.sh로 만든다 — M15의 TESTDISK처럼 빈 파일로
# 대신할 수 없다(내용이 있어야 검증이 성립한다).
if [[ -z "${MINICORE_QEMU_FAT32DISK:-}" ]]; then
  export MINICORE_QEMU_FAT32DISK="${BUILD_DIR}/fat32-test.img"
fi
if [[ -z "${MINICORE_QEMU_EXT4DISK:-}" ]]; then
  export MINICORE_QEMU_EXT4DISK="${BUILD_DIR}/ext4-test.img"
fi
if [[ ! -f "$MINICORE_QEMU_FAT32DISK" || ! -f "$MINICORE_QEMU_EXT4DISK" ]]; then
  bash "$SCRIPT_DIR/make-fs-test-images.sh" "$MINICORE_QEMU_FAT32DISK" "$MINICORE_QEMU_EXT4DISK"
fi

declare -a EXPECTED=(
  "hello from kernel"
  "[boot_info:selftest] memory_map_count=5"
  "[boot_info:selftest] initrd_addr=0x1000000 initrd_size=0x100000"
  "[mm:init] node[0]"
  "[mm:alloc] order0 ok=1"
  "order2 ok=1"
  "[mm:slab] freed both chunks"
  "[mm:refcount] initial=0 after_addref=2 release1=0 release2=0 release3=1 (expect 0,2,0,0,1)"
  "[cow] after clone: parent_write=0 parent_cow=1 child_write=0 child_cow=1 same_phys=1 refcount=1 (expect 0,1,0,1,1,1)"
  "[cow] child write fault: handled=1 child_write_after=1 child_phys_changed=1 refcount_after=0 (expect 1,1,1,0)"
  "[cow] parent write fault: handled=1 parent_write_after=1 parent_phys_same=1 (expect 1,1,1)"
  "[object] create_proxy ok=1"
  "[object] proxy handle_info ok=1 kind=0 rights=1 (expect rights=1)"
  "[object] proxy handle_info after owner close: ok=0 (expect 0 — cascade revoke)"
  "[object] close(owner) again: is_err=1 error=2 (expect already_closed=2)"
  "[pgtbl] remap same addr: is_err=1 (expect 1, already_mapped)"
  "[pgtbl] query after protect(read-only): write=0 (expect 0)"
  "[pgtbl] query after unmap: present=0 (expect 0)"
  "[sched] thread A iteration 0"
  "[sched] thread B iteration 0"
  "[sched] thread A iteration 1"
  "[sched] thread B iteration 1"
  "[sched] thread A iteration 2"
  "[sched] thread B iteration 2"
  "[ipc] server sys_recv ok=1 badge=0xcafe (expect 0xcafe) label=0x1234 regs0=41"
  "[ipc] client sys_call ok=1 reply_label=0x5eed (expect 0x5eed) reply_regs0=42 (expect 42)"
  "[ipc2] receiver sys_recv ok=1 page_count=1 content_ok=1 handle_count=1 received_handle_kind=3 (expect notification=3)"
  "[ipc2] sender sys_call ok=1 ack_label=0xacc0"
  "[ipc2] receiver sys_wait ok=1 bits=0x2 (expect 0x2)"
  "[initrun] mcpack find_entry ok=1"
  "[initrun] load_elf ok=1"
  "[pci] assign_virtio_blk_bar ok=1 vendor=0x1af4 device=0x1001"
  "[initrun] setup_initrun_process ok=1"
  "[initrun] kernel received boot call ok=1 label=0xb007 (expect 0xb007) - 부팅 성공"
  "[initrun] cpio/ini self-test ok=1"
  "[process] fork ok"
  "[process] exec ok entry=0x10000000"
  "[process] spawn ok entry=0x10000000 trusted=0"
  "[fpu] thread A iteration 0 xmm0 preserved=1"
  "[fpu] thread A iteration 1 xmm0 preserved=1"
  "[fpu] thread A iteration 2 xmm0 preserved=1"
  "[fpu] thread A done all_preserved=1"
  "[fpu] thread B iteration 0 xmm0 preserved=1"
  "[fpu] thread B iteration 1 xmm0 preserved=1"
  "[fpu] thread B iteration 2 xmm0 preserved=1"
  "[fpu] thread B done all_preserved=1"
  "[fpu] xsave_avail=0 avx_avail=0 using_xsave=0 area_size=512"
  "[fpu-lazy] thread C iteration 7 xmm0 preserved=1"
  "[fpu-lazy] thread C done all_preserved=1"
  "[smp] BSP apic_id=0"
  "[smp] online_cpu_count=1"
  "[procsrv] vfs write/read roundtrip ok=1"
  "[devmgr] mcfg ecam_base=0xb0000000"
  "[devmgr]   class=0xc0330"
  "[ps2] controller self-test ok=1"
  "[usb] xhci hcrst_done=0x1"
  "[usb] xhci controller_ready=0x1"
  "[usb] xhci reset+port scan done"
  "[virtio-blk] write/read roundtrip ok=1"
  "[fat32] mount ok=0x1"
  "[ext4] mount ok=0x1"
  "[procsrv] fat32 read ok=1"
  "[procsrv] ext4 read ok=1"
  "[console] vga init ok=1"
  "[login] no keyboard input, using self-test account"
  "[login] auth ok=1"
  "[procsrv] loader roundtrip ok=1"
  "[procsrv] fd inherited continue read ok=1"
  "[su-target] guest open outside denied=1"
  "[su-target] guest open inside ok=1"
  "[login] su delegated ok=1"
  "[login] su denied ok=1"
  "[cfgsrv] persist load found=0"
  "[procsrv] cfgsrv roundtrip ok=1"
  "[procsrv] cfgsrv permission denied before grant=1"
  "[procsrv] cfgsrv permission granted after chmod=1"
  "[procsrv] cfgsrv full protocol ok=1"
  "[procsrv] wait exit_code ok=1"
  "[procsrv] kill requested ok=1"
  "[sched] thread killed (discarded before scheduling)"
  "[procsrv] m27 wait exit_code ok=1"
  "[procsrv] m27 kill ok=1"
  "[procsrv] reparent mechanism ok=1"
  "hello from real musl"
  "musl malloc ok=1"
  "musl malloc content ok=1"
  "musl errno ok=1"
  "musl fopen ok=1"
  "musl fread content ok=1"
  "musl printf read: hello vfs (9 bytes)"
  "[procsrv] musl-exec-target seed ok=1"
  "musl getpid ok=1"
  "musl fork ok=1"
  "hello from musl-exec-target (a different image)"
  "musl fork+exec+wait ok=1"
  "musl setlocale empty ok=1"
  "musl setlocale unknown name ok=1"
  "musl locale ctype still C ok=1"
  "[shell] session started"
  "[procsrv] shell session start ok=1"
  "[shell] no keyboard input, running self-test commands"
  "[shell] ls ok=1"
  "[shell] malloc buffer ok=1"
  "[shell] cat ok=1"
  "[shell] libc strcpy/strcat/strdup ok=1"
  "[shell] self-test done"
)

LOG_FILE="$(mktemp)"
trap 'rm -f "$LOG_FILE"' EXIT

timeout "$TIMEOUT_SEC" "$SCRIPT_DIR/run-qemu.sh" x86_64 "$BUILD_DIR" \
  > "$LOG_FILE" 2>&1

failed=0
for expected in "${EXPECTED[@]}"; do
  if grep -qF "$expected" "$LOG_FILE"; then
    echo "PASS: \"$expected\" 확인"
  else
    echo "FAIL: \"$expected\"를 찾지 못했다" >&2
    failed=1
  fi
done

if [[ "$failed" -ne 0 ]]; then
  echo "--- 전체 로그 ---" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

exit 0
