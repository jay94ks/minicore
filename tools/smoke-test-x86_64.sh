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
#                            ps2+usb를 담아 만든다, M13~M14) — M12부터 항상 실제
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
TIMEOUT_SEC=60  # M14부터 부트 디스크가 6개 서비스(memfs/vfs/procsrv/devmgr/ps2/usb)를 담아 훨씬 오래 걸린다.

if [[ -z "${MINICORE_QEMU_BOOTDISK:-}" ]]; then
  DEFAULT_BOOTDISK="${BUILD_DIR}/servers/bootdisk.img"  # M13부터 memfs+vfs+procsrv+devmgr+ps2+usb(servers/CMakeLists.txt).
  if [[ -f "$DEFAULT_BOOTDISK" ]]; then
    export MINICORE_QEMU_BOOTDISK="$DEFAULT_BOOTDISK"
  fi
fi

# M14 — usb 드라이버가 실제로 찾을 xHCI 컨트롤러도 기본으로 붙인다
# (run-qemu.sh 상단 주석 참고).
if [[ -z "${MINICORE_QEMU_XHCI:-}" ]]; then
  export MINICORE_QEMU_XHCI=1
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
