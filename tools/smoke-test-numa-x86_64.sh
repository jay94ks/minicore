#!/usr/bin/env bash
# x86_64 NUMA 스모크 테스트: docs/plan/smp-fpu-bringup.md §M11(ADR-036)의
# ACPI SRAT/SLIT 파싱 + 노드별 물리 메모리 풀 분리 + 워크 스틸링
# (ADR-053)을 QEMU `-numa`로 검증한다.
#
# 기본 스모크 테스트(smoke-test-x86_64.sh)·SMP 스모크 테스트
# (smoke-test-smp-x86_64.sh)와 분리하는 이유: MINICORE_QEMU_NUMA는
# MINICORE_QEMU_SMP처럼 opt-in이라(ADR-125 패턴) 기존 경로들은 이
# 스크립트 없이도 항상 그대로 통과해야 한다.
#
# 사용법: tools/smoke-test-numa-x86_64.sh [빌드 디렉토리(기본: build/x86_64-clang)]
#
# 환경 변수:
#   MINICORE_QEMU_BIN   qemu-system-x86_64 실행파일 경로(run-qemu.sh로 그대로 전달)
#   MINICORE_QEMU_SMP   코어 수(기본: 4)
#   MINICORE_QEMU_NUMA  노드 수(기본: 2) — 반드시 MINICORE_QEMU_SMP를 나눠야 한다.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-build/x86_64-clang}"
TIMEOUT_SEC=20
export MINICORE_QEMU_SMP="${MINICORE_QEMU_SMP:-4}"
export MINICORE_QEMU_NUMA="${MINICORE_QEMU_NUMA:-2}"

declare -a EXPECTED=(
  "hello from kernel"
  "[numa] srat_ok=1 node_count=${MINICORE_QEMU_NUMA}"
  "[numa] cpu[0] apic_id=0 node=0"
  "[numa] distance[0][0]=10"
  "[numa] distance[0][1]=20"
  "numa_node_count=${MINICORE_QEMU_NUMA}"
  # mm::init()이 self-test fixture가 아니라 SRAT의 실제 메모리
  # 어피니티로 초기화됐다는 뜻 — node[1]이 실존해야 한다(SMP만 켜고
  # NUMA를 안 켠 smoke-test-smp-x86_64.sh는 항상 node[0] 하나뿐이다).
  "[mm:init] node[1]"
  # 워크 스틸링(ADR-053) — preferred_node=1로 만든 스레드는
  # g_run_queues[1]에 들어가는데, 이 협조적 스케줄러는 BSP(노드 0)
  # 한 코어만 sched::start()/yield()를 실행한다(M10 done 보고 참고,
  # AP는 온라인 신호만 보내고 스케줄러에는 참여하지 않는다) — 그런데도
  # 이 스레드가 실제로 실행됐다면 노드를 넘어 훔쳐온 것이다.
  "[numa-sched] thread on preferred_node=1 ran"
  "[smp] AP apic_id=1 online cpu_index=1"
  "[smp] AP apic_id=2 online cpu_index=2"
  "[smp] AP apic_id=3 online cpu_index=3"
  "[smp] bring_up_aps done online_count=${MINICORE_QEMU_SMP}"
  # 완료 기준(계획 §M11): "기존 M1~M8 스모크 테스트 전체 + M9/M10 검증이
  # 함께 통과" — NUMA 경로(self-test fixture가 아니라 실제 SRAT 메모리
  # 어피니티로 mm::init된 상태)에서도 M1~M9의 핵심 검증 문자열이 전부
  # 그대로 나오는지 확인한다.
  "[object] proxy handle_info after owner close: ok=0 (expect 0 — cascade revoke)"
  "[pgtbl] query after unmap: present=0 (expect 0)"
  "[ipc] client sys_call ok=1 reply_label=0x5eed (expect 0x5eed) reply_regs0=42 (expect 42)"
  "[ipc2] receiver sys_wait ok=1 bits=0x2 (expect 0x2)"
  "[sched] thread A done"
  "[sched] thread B done"
  "[initrun] mcpack find_entry ok=1"
  "[initrun] load_elf ok=1"
  "[initrun] setup_initrun_process ok=1"
  "[initrun] kernel received boot call ok=1 label=0xb007 (expect 0xb007) - 부팅 성공"
  "[fpu] thread A done all_preserved=1"
  "[fpu] thread B done all_preserved=1"
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
