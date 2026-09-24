# UEFI 직접 부팅 - 커널 물리 재배치(physicalBase) 설계

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-CC2B18C6
  status: approved
  updatedAt: 2026-09-24T06:38:27.991Z
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

## 3. [개정, 2026-09-24, QU-A2CABBC6 답변 반영] 스테이지 로더 설계 - "재활용"은 32비트 진입 코드가 아니라 higher_half_entry 꼬리 + 페이지테이블 저장소를 가리킨다

### 3-0. 핵심 발견 - boot.S의 32비트 코드로 직접 점프하는 건 CPU 모드 불일치로 불가능

실제 `minicore/boot/x86_64/boot.S`(구 `arch/x86_64/boot.S`)를 코드
단위로 대조한 결과, 설계자가 지시한 "기존 GRUB/PVH 부팅 코드를
재활용"을 문자 그대로 "UEFI가 `pvh_start`/`mb2_start`/
`common_32bit_entry`로 점프한다"로 구현할 수 없다는 게 확인됐다 -
근본적인 CPU 모드 불일치 때문이다:

- GRUB/PVH는 boot.S에 **32비트 보호 모드, 페이징 꺼짐** 상태로
  진입한다(`common_32bit_entry`가 `.code32`) - `setup_page_tables`/
  `enable_long_mode`가 이 상태에서 처음부터 페이지테이블을 만들고
  PAE/LME/PG를 켜 롱 모드로 전환한다.
- UEFI는 `ExitBootServices()` 호출 시점에 **이미 64비트 롱 모드,
  페이징 켜짐**(펌웨어 자신의 페이지테이블) 상태다 - 32비트 코드
  세그먼트로 되돌아가는 것 자체가 불필요할 뿐 아니라(펌웨어가 이미
  롱 모드 전환을 끝냈음), `common_32bit_entry`의 `.code32` 명령들을
  그대로 실행하면 명령 디코딩 자체가 어긋난다.

**따라서 "재활용"의 실제 의미는 재해석해야 한다**: 32비트 설정
루틴(`setup_page_tables`/`enable_long_mode`)은 재사용 **불가능** -
UEFI 스테이지 로더가 그 논리(페이지테이블 채우기)를 자신의 64비트
C++ 코드로 다시 구현해야 한다. 대신 진짜로 재사용 가능하고 재사용해야
하는 것은:

1. **페이지테이블 저장소 자체**(`pml4`/`pdpt_low`/`pd_low`/
   `pdpt_high`/`pd_high`, `.boot.bss`) - 이 심볼들은 커널 ELF 안에
   있으므로, UEFI 로더가 PT_LOAD 세그먼트를 `physicalBase`로
   memcpy할 때 **같은 이미지의 일부로 함께 재배치된다** - 별도
   할당이 필요 없다.
2. **`higher_half_entry`**(`.text`, `.code64`) - `.bss` 제로클리어 +
   부트 스택 설정 + `kMain(startInfoAddr, bootProtocol)` 호출까지
   하는 실제 커널 진입 코드. UEFI 스테이지 로더가 페이지테이블만
   올바르게 채워 CR3를 전환한 뒤 이 지점으로 점프하면, 이후는
   GRUB/PVH와 완전히 동일한 코드 경로를 탄다.

### 3-1. pd_low(0~1GiB identity)는 손댈 필요가 없다 - 대신 physicalBase < 1GiB를 강제한다

`PN-7FBF255A`가 이미 실측 확인한 사실("우리 UEFI 이미지는 항상 낮은
1GiB 안에 로드된다", `belowOneGiB=1`)과 `pd_low`가 **물리 0~1GiB를
무조건 identity map**한다는 사실(§1 원문 그대로, physicalBase와
무관하게 하드코딩)을 조합하면 중요한 단순화가 나온다: **UEFI 로더가
고를 `physicalBase`가 `physicalBase + 커널이미지크기 ≤ 1GiB`를
만족하는 한, `pd_low`는 재배치된 커널 이미지(그 안의 `.boot`
섹션/트램폴린 코드 자신 포함)를 그대로 올바르게 identity-map한다 -
`pd_low`는 한 글자도 고칠 필요가 없다.**

이 조건은 §3의 free-region 탐색(원래 §3-1이었던 절차, 아래 3-2로
번호만 이동)에 **명시적 제약으로 추가**해야 한다 - "커널 이미지
전체를 담을 수 있는 자유 구간"뿐 아니라 "그 구간 + 이미지 크기가
1GiB를 넘지 않음"까지 확인. 기존 실측(자유 구간이 1.4MB~20MB대에
있음)으로 볼 때 이 조건은 사실상 항상 만족될 것으로 보이나, 코드로
명시적으로 검증(만족 못 하면 정직하게 실패)해야 한다.

`pd_high`/`pdpt_high`(higher-half → 물리 1GiB 창)만 `physicalBase`
기준으로 다시 채워야 한다 - §1의 항목1이 실제로 가리키는 유일한
변경 지점.

### 3-2. UEFI 스테이지 로더 절차 (개정판)

`minicore/boot/x86_64/uefi/`(구 `minicore/boot-uefi/`)가 다음을
순서대로 한다:

1. `GetMemoryMap()`에서 커널 이미지 전체를 담을 수 있고 **1GiB 미만에
   완전히 들어가는** 연속 `EfiConventionalMemory` 구간을 찾는다 -
   이 구간의 시작 주소가 `physicalBase`(§3-1 제약 반영, 기존
   §3-1(이전 판)의 탐색 로직에 이 조건만 추가).
2. `ExitBootServices()` 호출(이미 구현/검증 완료, commit 8d8aa79).
3. 커널 ELF의 각 PT_LOAD 세그먼트를 `physicalBase` 기준 오프셋
   (`p_paddr - KERNEL_LMA`)에 memcpy/memset(파싱은 완료, commit
   fccefa8 - 실제 복사는 미구현) - `pml4`/`pdpt_low`/`pd_low`/
   `pdpt_high`/`pd_high`(전부 `.boot.bss`, PT_LOAD의 일부)도 이
   memcpy로 자동으로 함께 재배치된다(별도 처리 불필요, 다만 `.bss`라
   파일 내용은 없으므로 해당 범위는 `memset(0)`).
4. UEFI 로더 자신의 C++ 코드로 이 재배치된 물리 메모리 위에 **직접**
   페이지테이블을 채운다(32비트 `setup_page_tables`의 로직을 64비트
   C++로 재구현, §3-0이 이미 밝힌 대로 원본 어셈블리 루틴은 재사용
   불가):
   - `pml4[0] -> pdpt_low -> pd_low[0..511] = 0~1GiB identity`
     (boot.S의 `setup_page_tables`와 완전히 동일한 값 - physicalBase와
     무관).
   - `pml4[511] -> pdpt_high[510] -> pd_high -> [i] = physicalBase +
     i*2MiB`(**physicalBase 오프셋만 다름** - 이게 이 설계 전체에서
     유일하게 "새로" 계산해야 하는 값).
   - 이 네 테이블의 실제 물리 주소는 각각 `physicalBase +
     (심볼의 원래 링크 타임 LMA - KERNEL_LMA)`로 계산(ELF PT_LOAD
     세그먼트 안 오프셋 그대로 - 이미 §3-2 3번이 계산해 둔 것과
     같은 산수).
5. `mov cr3, <physicalBase 기준 pml4 물리주소>` - 이 시점 CPU는 이미
   롱 모드+페이징 상태(UEFI 자신의 테이블)이므로, 이 스왑이 안전하려면
   **이 명령 직후 명령 스트림이 새 테이블에서도 유효하게 매핑돼
   있어야 한다** - UEFI 로더 자신의 코드가 §3-1이 보장하는 저지대
   1GiB identity map 범위 안에 있으므로(위 실측으로 이미 확인됨)
   이 조건은 자동으로 만족된다.
6. **`higher_half_entry`의 (고정된) 가상주소로 직접 점프** -
   `KERNEL_VMA`는 physicalBase와 무관하게 항상 같은 상수이므로,
   `higher_half_entry`의 링크 타임 가상주소는 UEFI 로더가 별도
   계산 없이 그대로 쓸 수 있다(단, 그 심볼의 정확한 가상주소 값
   자체를 UEFI 로더가 알아야 함 - §5 미결 항목 1번 참고). `mov
   cr3` 직후 곧바로 이 가상주소로 `jmp`하면 CR3 전환과 higher-half
   진입이 한 번에 끝난다 - boot.S의 `long_mode_entry`(세그먼트
   레지스터를 자체 `gdt64`로 재적재하는 절차)를 **거칠 필요가
   있는지는 미결**(아래 §5) - UEFI가 이미 flat 64비트 세그먼트
   환경이라 생략 가능할 가능성이 높으나 착수 세션이 실측 확인.
7. `higher_half_entry`(GRUB/PVH와 완전히 동일한 코드)가 `.bss`
   제로클리어 + 부트 스택 설정 후 `kMain(startInfoAddr,
   bootProtocol)`을 호출한다 - `startInfoAddr`/`bootProtocol` 대신
   UEFI 전용 정보(physicalBaseDelta 포함 BootInfo)를 어떻게 넘길지는
   §4가 이미 설계한 대로(BootInfo 필드 추가) - 다만 `higher_half_entry`
   자체를 그대로 재사용한다면 그 함수가 `kMain`에 넘기는 두 인자
   (`rdi`/`rsi`)의 의미를 UEFI 경로용으로 확장하거나, UEFI 전용의
   `higher_half_entry` 변형(같은 .bss 제로클리어+스택 설정 로직을
   공유하되 `kMain` 호출 인자만 다른) 중 무엇을 쓸지는 착수 세션이
   결정(구현 디테일 수준).

### 3-3. [신설, 2026-09-24, 설계자 답변(QU-946C0859)] §5 미결 두 가지 해소 - 매직 넘버 마커로 진입점 주소를 런타임 발견

QU-946C0859(위 3-0~3-2 개정판 승인 요청)에 대한 설계자 답변 전문:

> boot.S에 { magic number, long mode entry address }를 마킹해두고
> 그걸 찾아서 점프하게 만들면 되잖아

이 한 문장이 §5가 남겨 뒀던 미결 두 가지를 동시에 해소한다:

1. **UEFI 로더가 진입점의 가상주소를 어떻게 아는가** - ELF 심볼
   테이블을 새로 파싱하는 대신(§5가 후보로 들었던 (a)), boot.S
   자신이 링크 타임에 `{매직 넘버, 진입점 주소}` 쌍을 데이터로
   박아 두고, UEFI 로더는 이미 memcpy해 둔 커널 이미지 바이트를
   스캔해 그 매직 넘버를 찾아 바로 뒤의 값을 읽는다(§5 후보 (b),
   "고정 오프셋 마커"를 매직 넘버 스캔 방식으로 구체화한 것).
2. **어느 진입점으로 점프해야 하는가** - 마커가 가리키는 대상이
   `long_mode_entry`라고 답변에 명시돼 있다 - 즉 §5가 미결로 남겼던
   "`higher_half_entry`로 바로 갈지 `long_mode_entry`(세그먼트
   재적재)를 거칠지" 질문도 **`long_mode_entry` 경유로 확정**됐다
   (§5가 "안전 우선이면 이쪽을 기본값으로" 제안했던 바로 그 경로).

#### 구체 메커니즘 (착수 세션이 실측하며 세부 조정)

```asm
; boot.S, .boot 섹션 내 데이터 - 링크 타임에 값이 확정되는 순수 데이터,
; 코드가 아니므로 실행되지 않는다(스캔 대상으로만 존재).
.align 8
boot_marker_magic:      .quad 0x4D494E4943424F54   ; 예시 값 - 착수 세션이 고유값으로 확정(아래 참고)
boot_marker_entry_addr: .quad long_mode_entry        ; 링크 타임 상수 - KERNEL_VMA는
                                                       ; physicalBase와 무관하므로 이 값
                                                       ; 자체는 GRUB/PVH/UEFI 어느 경로든 항상 동일
```

UEFI 스테이지 로더(§3-2 3번, PT_LOAD memcpy 직후)가:

1. 복사된 커널 이미지의 바이트 범위(이미 파싱해 둔 PT_LOAD들의
   `p_paddr`/`p_memsz`로 경계를 앎)를 8바이트 정렬 단위로 순회하며
   `boot_marker_magic` 패턴을 찾는다.
2. 찾은 위치 바로 다음 8바이트를 `long_mode_entry`의 가상주소로
   읽는다.
3. §3-2 5-6번(CR3 전환 후 이 주소로 jmp)에 그대로 사용 - 6번 항목의
   "고정된 가상주소" 문구를 이 마커가 실제로 채워 준다.

**왜 이 방식이 ELF 심볼 테이블 파싱보다 나은가**: UEFI 로더와 커널은
완전히 별개의 두 빌드 트리(§3-0 참고)라 컴파일 타임에 서로의 심볼을
알 수 없다 - 심볼 테이블 파싱은 ELF `.symtab`/`.strtab` 섹션 파서를
새로 구현해야 하는 추가 작업이지만, 매직 넘버 스캔은 이미 파싱해 둔
PT_LOAD 범위 안을 8바이트 정렬로 훑는 것뿐이라 훨씬 단순하고, 커널의
링크 옵션(예: strip 여부)에도 영향받지 않는다.

**남은 세부(착수 세션 몫, 임의로 결정하지 않고 실측/확인)**:
- 매직 넘버의 정확한 값 - 우연한 코드/데이터와 충돌하지 않을 만큼
  고유해야 한다(64비트 값 권장, 이 프로젝트의 다른 매직 넘버들 -
  multiboot2 매직 등 - 과도 겹치지 않는지 확인).
- 마커를 `.boot` 섹션의 정확히 어느 지점에 둘지 - boot.S 앞부분에
  두면 스캔 범위를 더 좁힐 수 있다(최적화 여지, 필수는 아님 -
  전체 PT_LOAD 범위 스캔도 이미지 크기가 작아 실용적으로 충분히
  빠를 것으로 보임).
- 8바이트 정렬 스캔만으로 충분한지(`.align 8`로 마커 자체는 8정렬이
  보장되므로 스캐너도 8바이트 단위로만 검사하면 되어 보이나, 링커가
  섹션을 재배치하며 정렬을 흐트러뜨리지 않는지는 실제 빌드 산출물로
  재확인).

이로써 §5의 다음 두 항목은 **해소**된 것으로 본다(아래 §5 갱신 참고):
"`higher_half_entry`의 목표 가상주소를 UEFI 로더가 어떻게 아는가"와
"`long_mode_entry`를 거쳐야 하는지" - 매직 넘버 마커 하나로 둘 다
답이 나온다.

## 3-old. [대체됨, 2026-09-24, QU-A2CABBC6 답변 반영 - 위 3-0~3-2가 정본]

이 절은 "UEFI 로더가 boot.S와 무관한 자기 자신만의 독립 트램폴린을
새로 만든다"는 원안이었으나, 설계자가 "기존 GRUB/PVH 부팅 코드
재활용"을 명시적으로 지시하면서 대체됐다(QU-A2CABBC6). 전체 원문은
`docs diff SP-CC2B18C6 <이 패치 이전 리비전> current`로 조회 가능 -
본문에는 남기지 않는다(3-0~3-2와 내용이 정면으로 배치돼 혼동을
일으키므로). 요지만 남기면: `minicore/boot-uefi/`(PE32+/COFF)와
`minicore/kernel/`(ELF)가 링크 단계에서 분리된 바이너리라는 사실
관찰은 여전히 유효하지만, 거기서 "그러니 완전히 독립적인 트램폴린이
필요하다"고 결론 내린 부분이 위 3-0~3-2로 뒤집혔다 - 페이지테이블
저장소 심볼과 `higher_half_entry` 진입점은 같은 커널 ELF이므로
"분리된 바이너리" 여부와 무관하게 그대로 재사용 가능하다는 게 이번
개정의 핵심.

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

- ~~**`higher_half_entry`의 목표 가상주소를 UEFI 로더가 어떻게
  아는가**~~ **[해소, 2026-09-24, 설계자 답변 QU-946C0859, §3-3
  참고]** 매직 넘버 마커를 boot.S에 데이터로 박아 두고 UEFI 로더가
  스캔해 찾는 방식으로 확정.
- ~~**`long_mode_entry`(세그먼트 재적재)를 거쳐야 하는지, 곧바로
  `higher_half_entry`로 점프해도 되는지**~~ **[해소, 2026-09-24,
  같은 답변, §3-3 참고]** `long_mode_entry` 경유로 확정 - 마커가
  가리키는 대상 자체가 `long_mode_entry`. 다만 그 진입점의
  재배치된 물리주소가 아니라 **가상주소**를 마커에 저장하므로(위
  §3-3 코드 예시), CR3 전환 후 곧바로 이 가상주소로 jmp하면 되고
  별도 물리주소 재계산은 불필요(§3-2 5-6번과 일치).
- **PT_LOAD 세그먼트 memcpy 시 파일 오프셋과 물리 오프셋의 관계** -
  `PN-7FBF255A`가 이미 파싱해 둔 `p_offset`/`p_filesz`/`p_memsz`를
  그대로 쓰면 될 것으로 보이나 착수 시 재확인.
- **`physicalBase + 커널이미지크기 ≤ 1GiB` 제약의 실제 강제** -
  §3-1이 요구하는 조건을 free-region 탐색 코드에 명시적 검증으로
  넣어야 한다(만족 못 하면 정직하게 실패 - 기존 실측상 항상 만족될
  걸로 보이지만 가정에 의존하면 안 됨).
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
- `QU-A2CABBC6`(resolved) - "기존 부팅 코드 재활용" 지시 - §3-0~3-2
  개정의 근거.
- `QU-946C0859`(resolved) - §5 미결 두 가지(진입점 발견 방법 + 경유
  경로)를 매직 넘버 마커로 해소한 답변 - §3-3 참고.
- `minicore/kernel/boot_info.h`/`multiboot2.h` - 이 설계가 재사용하는
  기존 BootInfo 통일 패턴.
- `minicore/boot/x86_64/boot.S`(구 `arch/x86_64/boot.S`) - §3-0~3-2가
  근거로 삼은 실제 부팅 코드(`common_32bit_entry`/`setup_page_tables`/
  `enable_long_mode`/`long_mode_entry`/`higher_half_entry`).
- `minicore/arch/x86_64/linker.ld` - `KERNEL_LMA`/`KERNEL_VMA`/
  `kernel_phys_start`/`kernel_phys_end` 정의부.
- `minicore/kernel/kmain.cpp:632-635` / `minicore/kernel/paging.cpp:593-595` -
  §1의 항목2/3이 가리키는 실제 코드 위치.

