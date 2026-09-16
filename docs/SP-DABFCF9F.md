# QEMU gdb stub 기반 커널 디버깅 워크플로 — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-DABFCF9F
  status: approved
  updatedAt: 2026-09-16T04:15:01.976Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# QEMU gdb stub 기반 커널 디버깅 워크플로 — 설계 제안

설계자 의견(2026-09-16, PN-1E7798AF에 남긴 Opinion): "QEMU -s -S +
gdb-multiarch(또는 동등 도구) 기반 워크플로를 표준 개발 절차를
설계해보자." PN-1E7798AF("QEMU gdb stub 기반 커널 디버깅 워크플로
구축 필요")가 근거로 든 세 건(PN-584DB994/PN-57CF48DB/PN-6049A353,
전부 "정적 추론 한계, gdb 필요"로 막힘)에 대한 실제 설계 산출물.

## 1. 현재 상태 (실측, 2026-09-16)

`scripts/run-qemu.sh`(PVH, `-kernel` 직접 부팅)/`run-grub.sh`(GRUB
ISO, multiboot2)는 둘 다 `timeout N qemu-system-x86_64 ... -serial
stdio -display none -no-reboot -d cpu_reset,guest_errors -D
<log>` 패턴 - **디버거 연결 지점이 전혀 없다**. `-d cpu_reset`이
Triple Fault 시 레지스터 일부를 QEMU 자체 로그에 남기긴 하지만
(PN-6049A353이 겪은 "패닉 메시지조차 없는" 침묵은 커널 자신의
시리얼 출력이 없다는 뜻이지 QEMU 트레이스 로그까지 비어 있다는
뜻은 아닐 수 있음 - §5-1 확인 필요), 실제 원인이 된 명령어 단위
실행 흐름은 알 수 없다. SMP(`-smp N`) 옵션 자체도 두 스크립트
어디에도 파라미터화돼 있지 않다(SMP4 시나리오는 지금까지 세션들이
직접 임시 커맨드라인으로 실행해 온 것으로 보인다).

## 2. 목표 / 비목표

**목표**
1. 기존 두 스크립트(run-qemu.sh/run-grub.sh)를 대체하지 않고
   **디버그 변형**을 추가 - `-s -S`(gdb stub, 첫 명령 전 정지)로
   부팅해 `gdb-multiarch`(또는 동등)로 연결할 수 있게 한다.
2. `-mcmodel=kernel` freestanding ELF에 맞는 최소 gdb 초기화
   스크립트(심볼 로드, 진입점/공통 함수 브레이크포인트 매크로) 제공.
3. **재현율이 낮은 버그**(PN-584DB994 ~4-6%) 대응 - 조건부
   브레이크포인트 + 반복 실행 자동화 방향 제시.
4. SMP 디버깅 - QEMU gdbstub은 vCPU마다 별도 gdb 스레드로 노출한다
   (`-smp N` 지정 시) - `info threads`/`thread N`으로 코어별 상태
   전환 가능함을 문서화.
5. 기존 개발 루프(빌드→타임아웃 실행→로그 확인)를 그대로 두고,
   필요할 때만 디버그 변형으로 전환하는 선택적 도구로 유지 -
   기본 워크플로를 무겁게 만들지 않는다.

**비목표(v1)**
- CI/자동화 통합(이 워크플로는 사람/AI 세션이 수동으로 붙는
  대화형 디버깅 - DS-D4E5C451이 확정한 "CI는 로컬 수동 검증 +
  QEMU 스모크 테스트 자동화"와 별개, 스모크 테스트 자체를 gdb로
  감싸지 않음).
- 커널 자신에 원격 디버그 프로토콜을 구현(예: 커널 안에 GDB stub
  탑재) - QEMU가 이미 제공하는 하이퍼바이저 레벨 gdbstub으로 충분.

## 3. 스크립트 변경 (`scripts/`, 스케치)

### 3.1 `run-qemu-gdb.sh`(신규, run-qemu.sh 변형)

```bash
#!/usr/bin/env bash
# run-qemu.sh와 동일하게 빌드하되, timeout 대신 -s -S로 QEMU를
# 일시정지 상태로 띄우고 gdb-multiarch 연결을 기다린다.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SMP="${MINICORE_QEMU_SMP:-1}"          # SMP4 재현 시 MINICORE_QEMU_SMP=4

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-x86_64.cmake" >/dev/null
cmake --build "${BUILD_DIR}" >/dev/null

echo "--- QEMU가 -s -S로 일시정지 상태로 대기 중입니다 ---"
echo "다른 터미널에서: gdb-multiarch -x ${ROOT_DIR}/scripts/kernel.gdb"
qemu-system-x86_64 \
    -kernel "${BUILD_DIR}/minicore.elf" \
    -serial stdio -display none -no-reboot \
    -smp "${SMP}" \
    -d cpu_reset,guest_errors -D "${BUILD_DIR}/qemu-gdb.log" \
    -s -S
```

`run-grub-gdb.sh`도 동일 패턴(ISO 빌드까지는 run-grub.sh 재사용,
마지막 QEMU 호출만 `-s -S` 추가) - SMP4/multiboot2 조합 재현에 사용.

### 3.2 `scripts/kernel.gdb`(신규) - 최소 초기화 스크립트

```gdb
# gdb-multiarch -x scripts/kernel.gdb (프로젝트 루트에서)
target remote :1234
symbol-file build/minicore.elf
set architecture i386:x86-64
# higher-half 커널이므로 심볼 주소 자체가 이미 KERNEL_VMA 기준 -
# 별도 오프셋 계산 불필요(linker.ld 참고).
break kmain
# SMP: info threads로 vCPU별(코어별) 스레드 확인, thread N으로 전환.
```

## 4. 낮은 재현율 버그 대응 - 반복 + 조건부 정지

PN-584DB994(~4-6%)처럼 매번 수동으로 붙어 기다리기 비효율적인
경우를 위한 2단계 절차 제안:

1. **1단계 - 비gdb 반복으로 재현만 확인**: 기존 `run-qemu.sh`(또는
   `run-grub.sh`)를 스크립트로 N회 반복 실행하며 로그에서 실패
   시그니처(패닉 메시지, 또는 PN-6049A353처럼 **침묵도 실패로
   간주** - 예상 로그 줄이 timeout까지 안 찍히면 실패)를 grep한다.
2. **2단계 - 재현되면 그 자리에서 gdb 변형으로 전환**: 1단계가 실패를
   감지한 그 실행 파라미터(있다면 시드/타이밍 관련 옵션)를 그대로
   §3.1 스크립트에 넘겨 `-s -S`로 다시 띄우고 수동 연결 - 다만 QEMU
   자체의 비결정성(실제 벽시계 타이밍) 때문에 "같은 파라미터로 다시
   실행"이 항상 같은 시점에 재현된다는 보장은 없다(PN-584DB994도
   이 문제를 겪었다) - **조건부 브레이크포인트**(gdb `break FUNC if
   COND`, 예: `break kSyncCr3 if coreIndex==1`)로 특정 상태에서만
   멈추게 하면 반복 실행 중 자연 재현을 기다리며 그 시점을 놓치지
   않을 수 있다.

## 5. 확인 필요 → [전부 해소, 2026-09-16, PN-1E7798AF 구현/실측 완료]

1. **`-d cpu_reset` 로그, Triple Fault 시에도 남는다 확인됨** -
   PN-6049A353의 실제 재현 로그에서 `CPU Reset (CPU N)` 항목 14건
   확인 - 게다가 요청한 core 0/1뿐 아니라 core 2/3까지 반복 리셋된
   흔적이 있어, 그 버그가 국소적이 아니라 전역 공유 상태 손상일
   가능성이라는 새 단서로 PN-6049A353에 반영됨.
2. **`gdb-multiarch` 불필요로 확정** - WSL에 설치돼 있지 않았지만,
   호스트/타겟이 둘 다 x86_64라 평범한 `gdb`로 `target remote`/
   브레이크포인트/`detach`까지 전부 정상 동작 확인 - `scripts/
   kernel.gdb`는 plain `gdb` 전제로 확정.
3. **`-S`가 리셋 벡터(`0xfff0`)에서 실제로 정지함을 확인** - `.boot`
   저지대 코드(higher-half 전환/페이징 이전)부터 부팅 전체를 처음부터
   단일 스텝 가능.

구현/실측(commit cc01edb/96b1d13) - `scripts/run-qemu-gdb.sh`/
`run-grub-gdb.sh`(`MINICORE_QEMU_SMP`/`MINICORE_QEMU_INITRD` 환경변수화)
+ `scripts/kernel.gdb`(`break kMain`/`break kPanic` 둘 다 정확한 소스
위치로 resolve 확인) + 최상위 `CMakeLists.txt`에 전역 `-g`(DWARF, 런타임
동작 무변화, QEMU 4개 표준 시나리오 회귀 없음 확인) 추가. 자세한 내용은
PN-1E7798AF(completed) 참고.

## 착수 조건

없음 - 설계/구현/실측 전부 완료(PN-1E7798AF).

