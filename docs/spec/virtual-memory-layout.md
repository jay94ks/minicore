# 가상메모리 레이아웃 스펙

**관련 결정**: ADR-009, ADR-078
**관련 설계**: [repo-layout.md](../design/repo-layout.md) (`kernel/core/mm`, `kernel/arch/*`), [kernel-memory.md](../design/kernel-memory.md)
**관련 스펙**: [boot.md](boot.md) §1(진입점, 항등 매핑 트램폴린), [memory.md](memory.md) §2(물리 할당자, `phys_to_virt` 사용처)

이 문서는 ADR-078이 정한 "물리 다이렉트맵(physmap) + 별도 스택/이미지
영역" 정책을 아키텍처별 구체적 주소·페이지 크기·권한 비트로 구현
가능한 수준까지 정의한다.

## 1. 공통 원칙

- 4-level 페이지테이블, 48비트 가상주소(ADR-009) — v1 범위. 5-level
  (57비트)로 확장되어도 이 레이아웃은 그대로 유지된다(§4의 여유
  공간 참고, ADR-078 근거).
- 세 영역: physmap(512GiB, RW+NX), 커널 스택 영역(2GiB, 슬롯+가드
  페이지), 커널 이미지(최대 2GiB, 섹션별 권한).
- `kernel/core/mm`이 `phys_to_virt`/`virt_to_phys` 헬퍼(§5)를
  제공하고, arch 계층은 `k_physmap_base`/`k_physmap_size` 상수만
  정의한다(ADR-002 HAL 경계 유지).

## 2. x86_64

| 영역 | 시작 주소 | 크기 | 매핑 방식 | 권한 |
|---|---|---|---|---|
| 유저 공간 | `0x0000000000000000` | 128 TiB | 프로세스별 4-level 트리 | 프로세스 설계에 따름 |
| (canonical hole) | — | — | 매핑 없음(non-canonical, 접근 시 `#GP`) | — |
| physmap | `0xFFFF800000000000` | 512 GiB | PDPT 1GiB 페이지 512개(테이블 1개) | RW, NX |
| 커널 스택 | `0xFFFFFFFF00000000` | 2 GiB | 4KiB 페이지, 슬롯당 가드 1페이지(§6) | RW, NX |
| 커널 이미지 | `0xFFFFFFFF80000000` | ≤2 GiB | 2MiB/4KiB 페이지, 섹션별(§3) | §3 |

### 2.1 부팅 순서 (boot.md §1.1 확장)

1. `_start32`: 최소 GDT + **임시 항등 매핑**(트램폴린 자신이 실행
   중인 물리주소 범위만 커버 — 첫 몇 MiB, 2MiB 페이지 1~2개로
   충분) 구성 → long mode 진입.
2. `_start64`: Multiboot2 태그 파싱 → `boot_info` 구성(boot.md
   §2~3) 이후, **본 문서 §2 표의 physmap/스택/이미지 페이지테이블**을
   구성한다. 커널 이미지는 링커 스크립트에서 이미 `0xFFFFFFFF80000000`
   기준으로 링크되므로, 물리 적재 주소와 이 가상 링크 주소 사이의
   오프셋만 페이지테이블에 반영하면 된다.
3. `CR3`를 새 페이지테이블 루트로 교체하고, higher-half
   (`0xFFFFFFFF80000000` 기준) `kernel_main`으로 롱점프한다. 1단계의
   임시 항등 매핑은 정리하지 않고 그대로 둔다(ADR-078 결정 3).

## 3. 커널 이미지 섹션 권한

| 섹션 | 권한 |
|---|---|
| `.text` | R + X (쓰기 금지) |
| `.rodata` | R (쓰기·실행 금지) |
| `.data`, `.bss` | R + W (실행 금지, NX/XN) |

링커 스크립트(`kernel/arch/x86_64/link.ld`, `kernel/arch/aarch64/link.ld` —
구현 시 작성)가 각 섹션을 페이지 경계(4KiB)에 정렬해야 섹션별로
서로 다른 권한을 페이지 단위로 적용할 수 있다.

## 4. aarch64

TTBR0(유저)/TTBR1(커널)을 분리해서 쓴다(x86_64의 canonical hole
방식과 달리 전용 레지스터로 상하위 절반을 나눈다). 4KiB 그래뉼,
4-level, 48비트는 x86_64와 동일하게 맞춘다(ADR-078 근거 — 문서·구현
양쪽의 아키텍처별 특수 케이스 최소화).

| 영역 | 시작 주소 | 크기 | 매핑 방식 | 권한 |
|---|---|---|---|---|
| 유저 공간(TTBR0) | `0x0000000000000000` | 256 TiB | 프로세스별 트리 | 프로세스 설계에 따름 |
| physmap(TTBR1) | `0xFFFF000000000000` | 512 GiB | 레벨1 1GiB 블록 512개 | RW, XN |
| 커널 스택(TTBR1) | `0xFFFFFFFF00000000` | 2 GiB | 4KiB 페이지, 슬롯당 가드 1페이지(§6) | RW, XN |
| 커널 이미지(TTBR1) | `0xFFFFFFFF80000000` | ≤2 GiB | 2MiB/4KiB 페이지, 섹션별(§3, XN 대신 실행 허용 섹션만 예외) | §3 |

x86_64와 절대 주소 프리픽스(`0xFFFF0...` vs `0xFFFF8...`)가 다른
것은 canonical/TTBR 분할 규칙 차이 때문이며, "맨 위에서부터의
상대 배치"는 두 아키텍처가 동일하다(ADR-078).

### 4.1 부팅 순서 (boot.md §1.3 확장)

1. `_start`(EL1 진입, `x0` = FDT 물리주소): 스택 설정 후 **임시
   항등 매핑**(TTBR0, 트램폴린 자신의 물리주소 범위만)을 구성하고
   MMU를 활성화한다.
2. FDT 파싱 → `boot_info` 구성(boot.md §2~3) 이후, 본 문서 §4 표의
   physmap/스택/이미지를 TTBR1에 구성한다.
3. `TTBR1_EL1`을 새 테이블로 교체하고 higher-half `kernel_main`으로
   분기한다. 이 시점에는 아직 유저 프로세스가 없으므로 `TTBR0_EL1`은
   그대로 두거나 0으로 둔다(무해) — 실제 유저 주소공간은 initrun
   로딩 시점(boot.md §4)에 처음 설정된다.

## 5. `phys_to_virt`/`virt_to_phys` (kernel/core/mm)

```cpp
inline void* phys_to_virt(uint64_t phys) {
    // phys >= k_physmap_size(512GiB)이면 LIBK_PANIC
    // (ADR-078의 "512GiB 초과는 이 시점에서 미지원" 제약, §아래 참고).
    return reinterpret_cast<void*>(k_physmap_base + phys);
}

inline uint64_t virt_to_phys(const void* virtual_addr) {
    auto addr = reinterpret_cast<uint64_t>(virtual_addr);
    // addr가 [k_physmap_base, k_physmap_base + k_physmap_size) 밖이면 LIBK_PANIC.
    return addr - k_physmap_base;
}
```

- `k_physmap_base`/`k_physmap_size`는 아키텍처별 값이 다른
  `constexpr`이며, arch 계층 헤더에서 정의되고 `kernel/core/mm`은
  그 값만 참조한다.
- [memory.md](memory.md) §2의 `page_frame`, §6의 슬랩 힙이 실제
  메모리 내용을 읽고 쓸 때 이 헬퍼를 거친다 — `page_frame` 자체에
  가상주소 필드를 별도로 두지 않는다(물리주소만 저장하고 필요할
  때 변환한다).

## 6. 커널 스택 슬롯

- 슬롯 크기: 스택 4페이지(16KiB, 두 아키텍처 동일 가정) + 가드
  1페이지(미매핑) = 슬롯당 5페이지 stride.
- 슬롯 할당 알고리즘(비트맵/free-list)과 정확한 스택 크기 최종값은
  [scheduler.md](scheduler.md)가 스레드 생성 API를 구체화하는 M5
  시점에 정한다 — 이 문서는 "영역 위치·가드 페이지 사용"까지만
  고정한다.

## 아직 정하지 않은 것

- 512GiB를 넘는 물리 메모리 지원 방식(physmap 확장 vs 온디맨드
  매핑) — 실제로 필요해지는 시점까지 미룬다(ADR-078 영향 참고).
- 5-level 페이지테이블(57비트) 전환 시 정확한 마이그레이션 절차 —
  ADR-009가 이미 "추후 고려"로 미뤄둔 상태를 유지한다.
- 커널 스택 정확한 크기(현재 16KiB 가정)와 슬롯 할당 알고리즘.
