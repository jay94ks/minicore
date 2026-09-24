# UEFI 직접 부팅 - 커널 물리 재배치(physicalBase) 설계

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-CC2B18C6
  status: review
  updatedAt: 2026-09-24T05:00:23.804Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# UEFI 직접 부팅 - 커널 물리 재배치(physicalBase) 설계

`PN-7FBF255A`(BIOS/UEFI 직접 부팅 경로 구현)의 2차 조사가 "boot.S
하나만 고치면 끝나는 게 아니다"라고 확인해 둔 것을, 실제 코드
3곳(`linker.ld`가 만드는 상수를 소비하는 지점)을 직접 대조해 하나의
일관된 설계로 정리한다. `QU-6778DA4C` 답변(설계자, "UEFI 경로만
별도로 처리")이 이미 확정한 방향을 구체화하는 문서이지, 그 방향
자체를 바꾸는 새 결정은 아니다.

## 0. [갱신, 2026-09-24, QU-A2CABBC6 답변] 승인이 아니라 방향 확장 - 디렉터리 재편 + 스테이지 로더로 범위가 커짐

설계자가 이 문서를 그대로 승인하는 대신, 더 큰 방향을 지시했다
(RM-23F4B687 §3도 함께 갱신됨 - "지침 자체의 변경"):

1. **디렉터리 재편**: 지금 이 문서가 가리키는 `minicore/arch/x86_64/`
   (boot.S/linker.ld)는 `minicore/boot/x86_64/`로 이름이 바뀐다.
   `minicore/boot-uefi/`는 `minicore/boot/x86_64/uefi/`로 흡수된다.
2. **스테이지 로더 도입** - 아래 §3이 제안했던 "UEFI 로더가 직접
   최소 트램폴린을 만들어 곧장 커널로 jmp"하는 방식 대신, **UEFI
   로더는 커널로 바로 넘어가지 않고 기존 GRUB/PVH 부팅 코드
   (`minicore/boot/x86_64/boot.S`)를 재활용할 수 있는 스테이지
   로더를 거친다.** 즉 §3의 "트램폴린을 UEFI 로더 자신이 새로
   작성" 부분은 이 방향으로 대체된다 - 정확히 어느 단계에서
   스테이지 로더로 넘어가는지(ExitBootServices 직후 boot.S의 어느
   진입점을 재사용하는지, physicalBaseDelta를 스테이지 로더가
   계산해 전달하는지 등)는 미결이며, §2(physicalBaseDelta 통일 설계
   자체)는 이 새 구조 위에서도 그대로 유효한 것으로 보이나 착수
   세션이 스테이지 로더 설계를 구체화하며 재확인해야 한다.
3. **별개 갈래 - `minicore/libs/x86_64` → `minicore/arch/x86_64`
   이전 + 유저/커널 공용화**는 이 문서/계획과 파일 집합이 겹치지
   않는 독립 작업이라 `PN-E612E714`로 분리 등록했다 - 이 문서는
   관여하지 않는다.

**이 문서 §1/§2/§4는 physicalBaseDelta 계산 자체(무엇을 얼마나
더할지)로서 여전히 유효한 설계로 보이나, §3(UEFI 로더의 트램폴린
구성 방식)과 §5(미결 사항)는 위 스테이지 로더 방향으로 다시
구체화돼야 한다** - 이 문서를 승인 대기(review) 상태로 유지하고,
착수 세션이 스테이지 로더 세부를 채운 개정판을 마저 작성한 뒤 다시
승인을 요청하는 것을 권장한다. `PN-7FBF255A` 본문에도 이 갱신을
반영해 뒀다.

## 1. 문제 - 물리 배치 가정이 3곳에 독립적으로 박혀 있다

`minicore/arch/x86_64/linker.ld`:

```
KERNEL_LMA = 1M;
KERNEL_VMA = 0xFFFFFFFF80000000;
...
. = KERNEL_LMA;
kernel_phys_start = .;
...
kernel_end = .;
kernel_phys_end = kernel_end - KERNEL_VMA;
```

이 두 상수(`KERNEL_LMA`/`KERNEL_VMA`)는 **링크 타임에 고정**된다 -
"커널은 항상 물리주소 1MiB에 로드된다"는 전제 위에서만 성립하는
계산이다. 이 전제를 실제로 소비하는 지점 셋:

1. **`minicore/arch/x86_64/boot.S`**(227번 줄 부근) - GRUB(multiboot2)/
   PVH 진입 경로의 초기 페이지테이블(`pd_low`/`pdpt_high`)이 물리
   `0~1GiB`를 컴파일 타임 상수로 identity+higher-half 매핑한다.
2. **`minicore/kernel/kmain.cpp`**(632-635번 줄) - `kernel_phys_start`/
   `kernel_phys_end` 링커 심볼 값을 그대로
   `PageFrameAllocator::init()`에 넘겨 "커널 자신이 차지한 물리
   범위"를 usable 메모리에서 예약(제외)한다.
3. **`minicore/kernel/paging.cpp`**(`Paging::init()`, 593-595번 줄) -
   direct map PDPT(`gDirectMapPdptStorage`, 커널 이미지 안 정적
   배열)의 물리주소를 `pdptPhys = pdptVirt - kKernelVma`(상수
   `0xFFFFFFFF80000000`)로 **독립적으로 다시** 계산한다.

커널이 UEFI에 의해 1MiB가 아닌 다른 물리주소에 로드되면(이미
`PN-7FBF255A`가 실측으로 확인 - 가장 큰 PT_LOAD 세그먼트를 그대로
1MiB에 배치하면 `EfiACPIMemoryNVS` 세 조각과 실제로 겹친다), 이 셋
전부가 틀린 값을 계산한다 - boot.S는 잘못된 페이지테이블을,
kmain.cpp는 잘못된 예약 범위(최악의 경우 살아있는 커널 코드/데이터
위에 다른 메모리를 할당하는 조용한 손상)를, paging.cpp는 깨진
direct map을 만든다.

## 2. 핵심 설계 결정 - 단일 `physicalBaseDelta`, GRUB/PVH는 델타=0으로 수학적으로 무변경

세 지점 모두 **"물리주소 = 링크 타임 상수 계산값 + delta"** 형태로
일반화할 수 있고, `delta = 실제 로드된 물리 베이스 - KERNEL_LMA(1MiB)`
하나만 알면 충분하다:

- kmain.cpp(항목2): `actualPhysStart = kernel_phys_start + delta`,
  `actualPhysEnd = kernel_phys_end + delta` - 두 심볼의 **차이**
  (커널 이미지 크기)는 물리 로드 위치와 무관한 링크 타임 상수이므로
  안 바뀐다. delta만 더하면 된다.
- paging.cpp(항목3): `pdptPhys = pdptVirt - kKernelVma + delta`.

**GRUB/PVH 경로는 `delta = 0`을 그대로 넘기면 오늘 코드와 수치적으로
완전히 동일한 결과가 나온다** - 즉 이 변경은 GRUB/PVH 두 지점에
"조건 분기"가 아니라 "델타 파라미터 추가"로만 끝나고, 델타가 0이면
동작이 바뀌지 않음을 코드 리뷰만으로 증명할 수 있다(회귀 위험 최소화
- 이 프로젝트가 boot.S/paging.cpp/page_frame_allocator.cpp를 다룰 때
계속 강조해 온 신중함과 부합).

**항목1(boot.S)은 이 델타 공식으로 "고치는" 게 아니라 애초에 적용
대상이 아니다** - 아래 §3 참고.

## 3. UEFI 경로는 boot.S를 아예 타지 않는다 - 별도 트램폴린이 필요

`PN-7FBF255A`가 이미 실측으로 확인한 사실: `minicore/boot-uefi/`
(PE32+/COFF, `lld-link`)와 `minicore/kernel/`(ELF,
`ENTRY(pvh_start)`)는 **링크 단계에서 한 번도 연결된 적 없는 완전히
별개의 두 바이너리**다. 즉 "boot.S를 고쳐서 UEFI도 처리하게 한다"는
전제 자체가 성립하지 않는다 - UEFI 로더 실행 시점엔 이미
`ExitBootServices()` 이후, 롱 모드+페이징이 켜진 상태(펌웨어가 만든
자기 자신의 매핑)로 우리 C++ 코드가 실행 중이므로, GRUB/PVH처럼
32비트 보호모드에서 새로 페이징을 켜는 절차 자체가 필요 없다.

대신 UEFI 로더(`minicore/boot-uefi/`) 자신이 다음을 순서대로 한다
("다음 세션 착수 순서" 5-7번을 이 설계로 구체화):

1. `GetMemoryMap()`으로 얻은 맵에서 커널 이미지 전체
   (`kernel_phys_end - kernel_phys_start` 바이트)를 담을 수 있는
   연속된 `EfiConventionalMemory` 구간을 찾는다(이미 실측 확인:
   기존 실패 지점 주변에도 자유 구간이 충분히 있음 - §1의 겹침은
   "1MiB 고정" 때문이었지, 이 물리 공간 자체에 여유가 없어서가
   아니다). 이 구간의 시작 주소가 `physicalBase`.
2. `ExitBootServices()` 호출(이미 구현/검증 완료, commit 8d8aa79).
3. 커널 ELF의 각 PT_LOAD 세그먼트를 `physicalBase` 기준 오프셋
   (`p_paddr - KERNEL_LMA`)에 memcpy/memset(이미 파싱 완료,
   commit fccefa8 - 아직 실제 복사는 미구현).
4. **최소 트램폴린 페이지테이블**을 새로 구성 - boot.S의
   `pd_low`/`pdpt_high`와 같은 역할이지만 컴파일 타임 상수가 아니라
   런타임에 읽은 `physicalBase`로 채운다:
   - 낮은 주소 identity map: `physicalBase` 근방(트램폴린 코드
     자신이 실행 중인 위치, UEFI 로더 이미지 자체의 물리 위치)만
     최소로 - CR3 전환 직후 몇 명령어만 실행하면 되므로 GRUB/PVH의
     "0~1GiB 전체"보다 훨씬 좁은 범위로 충분할 것으로 보임(정확한
     범위는 착수 세션이 UEFI 로더 자신의 링크 주소를 실측 확인 후
     확정).
   - higher-half 매핑: `[physicalBase, physicalBase+imageSize)` →
     `[KERNEL_VMA, KERNEL_VMA+imageSize)` - boot.S의 `pdpt_high[510]`
     와 동일한 슬롯, 물리 베이스만 다름.
5. `mov cr3, <새 PML4>` 후 higher-half 커널 진입점으로
   `movabs+jmp`(boot.S가 하는 것과 동일한 패턴, 이 트램폴린은
   C++이 아니라 인라인 어셈블리 몇 줄로 충분할 것).
6. 커널 진입점(`kMain` 또는 그 직전 얇은 UEFI 전용 진입 스텁)에
   `physicalBaseDelta = physicalBase - KERNEL_LMA`를 넘긴다.

## 4. physicalBaseDelta 전달 경로 - BootInfo 패턴 재사용

`BootInfo`(`minicore/kernel/boot_info.h`)는 이미 "PVH/multiboot2
어느 프로토콜로 부팅했든 kMain이 통일된 형태로 받는다"는 정확히
같은 문제를 풀어 둔 기존 패턴이다(`Multiboot2Info::parse()`가
GRUB의 태그 기반 정보를 이 형태로 변환). 새 필드 하나만 추가하는
것을 권장:

```cpp
struct BootInfo {
    const char* cmdline;
    const char* bootloaderName;
    uint32_t moduleCount;
    BootModule modules[kBootInfoMaxModules];
    uint64_t physicalBaseDelta;  // [신규] 0 = 재배치 없음(GRUB/PVH 기본값)
};
```

GRUB(`Multiboot2Info::parse()`)/PVH 경로는 이 필드를 그냥 0으로
채운다(또는 `BootInfo{}`의 기본 초기화에 맡김) - §2가 증명했듯
수치적으로 오늘과 동일. `kmain.cpp`의 `PageFrameAllocator::init()`/
`Paging::init()` 호출부만 이 delta를 받아 §2의 공식을 적용하도록
한 줄씩 고치면 된다 - 두 함수의 시그니처에 파라미터를 추가하거나,
호출 직전에 `kernel_phys_start`/`kernel_phys_end`/`pdptPhys` 계산
자체에 미리 더해서 넘기는 두 방식 다 가능(착수 세션이 기존 시그니처
안정성/호출부 개수를 보고 결정 - 구현 디테일 수준, RM-23F4B687 §4).

## 5. 이 설계가 명시적으로 다루지 않는 것 (착수 세션 몫)

- **UEFI 로더 자신의 트램폴린 코드(§3-4)의 정확한 어셈블리** - 로더
  이미지 자신이 실행 중인 물리 주소 범위를 실측해야 identity map
  최소 범위를 확정할 수 있다.
- **PT_LOAD 세그먼트 memcpy 시 파일 오프셋과 물리 오프셋의 관계** -
  `PN-7FBF255A`가 이미 파싱해 둔 `p_offset`/`p_filesz`/`p_memsz`를
  그대로 쓰면 될 것으로 보이나 착수 시 재확인.
- **`.ap_trampoline16`(SMP AP 기동, 물리주소 0x8000 고정)과의 상호작용** -
  `linker.ld` 주석이 이미 "0-2MiB는 kernel_phys_start/End와 무관하게
  이미 통째로 예약"이라고 명시해 뒀으므로 이 설계와 충돌하지 않을
  것으로 보이나, `physicalBase`가 우연히 0x8000 근방에 겹치는 경우가
  없는지는 착수 세션이 §3-1의 free-region 탐색 조건에 "0x8000 부근
  회피"를 포함시켜야 하는지 확인 필요.
- **실측 검증 전체** - 이 프로젝트의 확립된 원칙대로, 위 설계를
  코드로 옮긴 뒤 GRUB/PVH 표준 회귀(delta=0 무변경 확인) + 실제 UEFI
  QEMU+OVMF 부팅(재배치된 물리주소에서 커널이 정상 기동하는지)
  둘 다 실측해야 완결된다.

## 참고
- `PN-7FBF255A` - 이 설계가 구체화하는 원 조사 계획(2차 조사 절이
  이 설계의 직접적인 재료).
- `QU-6778DA4C`(resolved) - "UEFI 경로만 별도로 처리" 확정 답변.
- `minicore/kernel/boot_info.h`/`multiboot2.h` - 이 설계가 재사용하는
  기존 BootInfo 통일 패턴.
- `minicore/arch/x86_64/linker.ld` - `KERNEL_LMA`/`KERNEL_VMA`/
  `kernel_phys_start`/`kernel_phys_end` 정의부.
- `minicore/kernel/kmain.cpp:632-635` / `minicore/kernel/paging.cpp:593-595` -
  §1의 항목2/3이 가리키는 실제 코드 위치.

