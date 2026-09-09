#!/usr/bin/env bash
# 빌드 산출물을 QEMU로 부팅한다.
#
# 사용법: tools/run-qemu.sh <x86_64|aarch64> [빌드 디렉토리(기본: build/<arch>-clang)]
#
# x86_64: QEMU에 내장된 -kernel 직접 로더는 Multiboot2도, 64비트
# ELF(EM_X86_64)도 지원하지 않는다 — 이 커널은 둘 다 해당하므로 그
# 경로로는 못 띄운다(확인 경위는 ADR-114 참고). 대신 Xen/PVH ELF Note
# 직접 부팅 경로(QEMU가 함께 배포하는 qboot.rom 펌웨어)를 쓴다. 실제
# 배포·실기 부팅 경로는 여전히 Multiboot2(ADR-017, GRUB 필요)이며 이
# 스크립트의 범위 밖이다 — 이 스크립트는 QEMU 개발 반복 전용이다.
#
# 환경 변수:
#   MINICORE_QEMU_BIN     qemu-system-<arch> 실행파일 경로 (기본: PATH 탐색)
#   MINICORE_QEMU_GDB=1   ADR-125: QEMU를 -S(즉시 정지)로 띄우고 GDB
#                         스텁을 tcp::1234에 연다. 기본은 off — 켜면
#                         GDB가 붙을 때까지 부팅이 멈추므로
#                         smoke-test-x86_64.sh 같은 고정 타임아웃
#                         자동화 경로와는 같이 쓰지 않는다. 다른
#                         터미널에서 tools/debug-gdb.sh <arch>로 붙인다.
#   MINICORE_QEMU_TRACE=1 ADR-125: 트리플폴트/예외 진단용 QEMU 트레이스
#                         (-d cpu_reset,guest_errors,int)를 <빌드
#                         디렉토리>/qemu-trace.log에 남긴다. 기본은
#                         off — 로그량이 커 일반 부팅 경로에는 부담.
#   MINICORE_QEMU_SMP=N   docs/plan/smp-fpu-bringup.md M10(ADR-055):
#                         QEMU를 -smp N으로 띄운다. 기본은 미설정(=1코어,
#                         M1~M9와 동일한 동작 보존) — opt-in이라야
#                         ADR-125의 "기본값 유지" 패턴과 일치한다.
#   MINICORE_QEMU_NUMA=N  docs/plan/smp-fpu-bringup.md M11(ADR-036):
#                         MINICORE_QEMU_SMP개 코어를 N개 NUMA 노드로
#                         균등 분할하고(반드시 나누어져야 함), 노드마다
#                         독립된 memory-backend-ram(128MiB)과
#                         -numa dist(서로 다른 노드는 20)을 구성한다.
#                         MINICORE_QEMU_SMP 없이는 쓸 수 없다. 기본은
#                         미설정(=단일 노드, M1~M10과 동일).
#   MINICORE_QEMU_CPU=... docs/plan/smp-fpu-bringup.md M11b(ADR-133):
#                         -cpu 값을 그대로 전달한다(예: "qemu64"로
#                         XSAVE/AVX 없음, "max"로 XSAVE/AVX 있음 —
#                         fpu.cpp::init_fpu()가 남기는
#                         "[fpu] xsave_avail=.. avx_avail=.."로 실제
#                         반영 여부를 확인한다). 기본은 미설정(QEMU
#                         기본 CPU 모델).
#   MINICORE_QEMU_BOOTDISK=<경로>  docs/plan/system-servers-bringup.md
#                         M12(ADR-131/147): 그 경로의 파일을
#                         virtio-blk-pci 장치(bus 0, device 4,
#                         function 0 고정 — initrun의 disk.cfg가
#                         이 BDF를 그대로 가리킨다, init/initrun/
#                         CMakeLists.txt의 --disk-cfg=0,4,0,0과
#                         반드시 일치해야 한다)로 붙인다. 기본은
#                         미설정(장치 없음, M1~M11과 동일).
#   MINICORE_QEMU_XHCI=1  docs/plan/system-servers-bringup.md M14
#                         (ADR-130): QEMU 기본 xHCI 컨트롤러(qemu-xhci)
#                         를 bus0/device5에 붙인다(virtio-blk의 device4와
#                         겹치지 않는 자리) — devmgr/usb 드라이버가 실제로
#                         찾아 리셋/포트 상태 스캔을 시도할 대상. USB
#                         장치 자체는 붙이지 않는다. 기본은 미설정.
#   MINICORE_QEMU_TESTDISK=<경로>  docs/plan/system-servers-bringup.md
#                         M15(ADR-043): virtio-blk 드라이버가 실제로
#                         쓰고 읽을 **별도의** virtio-blk-pci 장치
#                         (bus0/device6 고정)로 붙인다. 부트 디바이스
#                         (MINICORE_QEMU_BOOTDISK)와 반드시 달라야
#                         한다 — 같은 파일을 재사용하면 그 cpio
#                         아카이브 내용을 실제로 덮어써 손상시킨다.
#                         파일이 없으면 1MiB 빈 파일을 새로 만든다.
#                         기본은 미설정(장치 없음).

set -euo pipefail

ARCH="${1:?"사용법: $0 <x86_64|aarch64> [빌드 디렉토리]"}"
BUILD_DIR="${2:-build/${ARCH}-clang}"

case "$ARCH" in
  x86_64)
    QEMU_BIN="${MINICORE_QEMU_BIN:-qemu-system-x86_64}"
    KERNEL="${BUILD_DIR}/kernel/arch/x86_64/minicore_kernel_x86_64.elf"

    if [[ ! -f "$KERNEL" ]]; then
      echo "커널 이미지가 없다: $KERNEL" >&2
      echo "먼저 빌드: cmake --build ${BUILD_DIR} --target minicore_kernel_x86_64" >&2
      exit 1
    fi

    declare -a EXTRA_ARGS=()
    MEM_ARG="256M"
    if [[ "${MINICORE_QEMU_GDB:-0}" == "1" ]]; then
      EXTRA_ARGS+=(-S -gdb tcp::1234)
    fi
    if [[ "${MINICORE_QEMU_TRACE:-0}" == "1" ]]; then
      EXTRA_ARGS+=(-d cpu_reset,guest_errors,int -D "${BUILD_DIR}/qemu-trace.log")
    fi

    if [[ -n "${MINICORE_QEMU_NUMA:-}" ]]; then
      NUMA_NODES="${MINICORE_QEMU_NUMA}"
      SMP_CORES="${MINICORE_QEMU_SMP:?"MINICORE_QEMU_NUMA는 MINICORE_QEMU_SMP도 함께 설정해야 한다"}"
      if (( SMP_CORES % NUMA_NODES != 0 )); then
        echo "MINICORE_QEMU_SMP(${SMP_CORES})는 MINICORE_QEMU_NUMA(${NUMA_NODES})로 나누어져야 한다" >&2
        exit 1
      fi
      CORES_PER_NODE=$((SMP_CORES / NUMA_NODES))
      MEM_PER_NODE_MB=128
      MEM_ARG="$((MEM_PER_NODE_MB * NUMA_NODES))M"

      EXTRA_ARGS+=(-smp "${SMP_CORES},sockets=1,cores=${SMP_CORES},threads=1")
      for ((n = 0; n < NUMA_NODES; ++n)); do
        EXTRA_ARGS+=(-object "memory-backend-ram,id=m${n},size=${MEM_PER_NODE_MB}M")
        EXTRA_ARGS+=(-numa "node,nodeid=${n},memdev=m${n}")
      done
      core_id=0
      for ((n = 0; n < NUMA_NODES; ++n)); do
        for ((c = 0; c < CORES_PER_NODE; ++c)); do
          EXTRA_ARGS+=(-numa "cpu,node-id=${n},socket-id=0,core-id=${core_id}")
          core_id=$((core_id + 1))
        done
      done
      for ((n = 0; n < NUMA_NODES; ++n)); do
        for ((m = 0; m < NUMA_NODES; ++m)); do
          if ((n != m)); then
            EXTRA_ARGS+=(-numa "dist,src=${n},dst=${m},val=20")
          fi
        done
      done
    elif [[ -n "${MINICORE_QEMU_SMP:-}" ]]; then
      EXTRA_ARGS+=(-smp "${MINICORE_QEMU_SMP}")
    fi

    if [[ -n "${MINICORE_QEMU_CPU:-}" ]]; then
      EXTRA_ARGS+=(-cpu "${MINICORE_QEMU_CPU}")
    fi

    if [[ -n "${MINICORE_QEMU_BOOTDISK:-}" ]]; then
      EXTRA_ARGS+=(-drive "if=none,id=bootdisk,format=raw,file=${MINICORE_QEMU_BOOTDISK}")
      EXTRA_ARGS+=(-device "virtio-blk-pci,drive=bootdisk,addr=04.0")
    fi

    if [[ -n "${MINICORE_QEMU_TESTDISK:-}" ]]; then
      # M15(system-servers-bringup.md, ADR-043 1순위) — virtio-blk
      # 드라이버가 실제로 쓰고 읽을 **별도의** 디스크. bus0/device6
      # 고정(virtio-blk 부트 디바이스의 device4, xHCI의 device5와
      # 겹치지 않는 자리) — 부트 디바이스(MINICORE_QEMU_BOOTDISK)를
      # 재사용하면 그 cpio 아카이브 내용을 실제로 덮어써 버린다(직접
      # 겪은 문제, 2026-09-09 — 그 파일은 이후 재빌드 전까지 손상된
      # 채로 남는다). 파일이 없으면 1MiB짜리 빈 파일을 새로 만든다.
      if [[ ! -f "${MINICORE_QEMU_TESTDISK}" ]]; then
        dd if=/dev/zero of="${MINICORE_QEMU_TESTDISK}" bs=1M count=1 status=none
      fi
      EXTRA_ARGS+=(-drive "if=none,id=testdisk,format=raw,file=${MINICORE_QEMU_TESTDISK}")
      EXTRA_ARGS+=(-device "virtio-blk-pci,drive=testdisk,addr=06.0")
    fi

    if [[ "${MINICORE_QEMU_XHCI:-0}" == "1" ]]; then
      # M14(system-servers-bringup.md, ADR-130) — devmgr/usb 드라이버가
      # 실제로 찾을 xHCI 컨트롤러. bus0/device5 고정(virtio-blk의
      # device4와 겹치지 않는 자리) — QEMU의 기본 xHCI 모델(qemu-xhci)
      # 하나만 붙인다, USB 장치 자체는 아직 붙이지 않는다(포트
      # 리셋/상태 스캔까지만 검증, servers/drivers/usb/main.cpp 참고).
      EXTRA_ARGS+=(-device "qemu-xhci,addr=05.0")
    fi

    exec "$QEMU_BIN" \
      -M q35 -m "$MEM_ARG" \
      -no-reboot -no-shutdown -display none \
      -bios qboot.rom \
      -kernel "$KERNEL" \
      -chardev stdio,id=char0,mux=off -serial chardev:char0 \
      "${EXTRA_ARGS[@]}"
    ;;
  aarch64)
    echo "aarch64 부트 스텁은 아직 없다 (docs/plan/kernel-bootstrap.md 범위 밖 — ADR-009 참고)" >&2
    exit 1
    ;;
  *)
    echo "사용법: $0 <x86_64|aarch64> [빌드 디렉토리]" >&2
    exit 1
    ;;
esac
