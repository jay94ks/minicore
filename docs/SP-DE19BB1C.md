# 커널 영역 TLB 샷다운(IPI 기반) — 설계 제안

<!--
  이 파일은 자동 생성된 사본(캐시)입니다 - 손으로 편집하지 마세요.
  정본은 claude-native-workflow(CNW)의 DB에 있습니다.
  trackingCode: SP-DE19BB1C
  status: approved
  updatedAt: 2026-09-16T08:43:38.850Z
  갱신: docs cache sync cmtzsjm5c000fo401iozcc60t docs
-->

# 커널 영역 TLB 샷다운(IPI 기반) — 설계 제안

설계자 지시(2026-09-14, 메시지 2건) - "다른 코어에 invlpg를 전파하는
IPI 기반 샷다운 메커니즘은, 커널 영역에 변동을 유발하는 동작을 했을
때, 무효화해야 할 가상 주소 범위를 전파하고, IPI를 받은 코어 B와 C는
하던 일을 멈추고 인터럽트 서비스 루틴(ISR)으로 진입, 수신받은 주소
범위를 보고 있었다면 invlpg를 실행하고, 발신자에게 ACK를 보내면 됨.
수신받은 주소를 보고 있지 않았으면 그냥 바로 ACK.", "TLB 무효화 관련
하여 최적화 기법으로, Flush batching, Lazy TLB를 고려하거나 현재
진행중인 Task가 다른 Task로 넘어가기전에 잠깐 멈추도록 마킹해두고,
mmap을 수행하는 코어가 해당 마킹을 busy-wait 하는 방법도 있어."

이 문서는 mmap 서브시스템(SP-2AAD7C8D) §6-2/§7이 "`KernelAddressSpaceManager`
의 매핑 해제/변경 기능을 쓰려면 선행돼야 한다"고 별도 계획으로
미뤄 뒀던 IPI 기반 TLB 샷다운을 실제로 설계한다.

## 1. 배경 - 왜 필요한가 (SP-2AAD7C8D §6-2에서 이어짐)

`Paging::unmapPage`는 지금 "언제나 이 코어가 보고 있는 주소공간
기준으로만" `invlpg`를 실행한다(코드 자체 주석에 이미 명시돼 있음).
커널 영역(higher half)은 모든 프로세스의 PML4가 같은 PDPT/PD/PT를
공유하므로, 한 코어가 커널 영역 매핑을 바꾸면 **다른 코어의 TLB에는
그 변경이 반영되지 않은 채 남는다** - 다른 코어가 이미 사라진/바뀐
매핑으로 계속 접근하면 하드웨어적으로는 정상 동작(스테일 TLB
엔트리를 그대로 사용)하지만 논리적으로 잘못된 물리 주소를 읽고
쓰게 된다. 이 문제를 해결하려면 "다른 코어들에게 이 가상주소 범위의
TLB 엔트리를 지우라"고 능동적으로 알리는 수단(IPI)이 필요하다.

## 2. 설계 제안

### 2.1 우편함(Mailbox) 구조체 - IPI 자체는 데이터를 못 옮기므로

x86-64 IPI(ICR 경유)는 벡터 번호와 목적지만 실어 나른다 - 실제
무효화할 주소 범위는 미리 공유 메모리에 적어 두고, IPI는 "그 메모리를
확인하라"는 신호로만 쓴다:

```cpp
// minicore/kernel, 가칭 tlb_shootdown.h
struct TlbShootdownRequest {
    uint64_t virtStart;         // 무효화할 범위의 시작(페이지 정렬)
    uint64_t virtEnd;           // 끝(배타적) - 여러 페이지면 ISR이 페이지 수만큼 반복 invlpg
    uint64_t targetPml4Phys;    // 0 = 커널 영역(모든 CR3가 공유하므로 무조건 invlpg)
                                 // 0이 아니면 그 값과 현재 CR3가 같은 코어만 invlpg(§5-A, 유저 영역 확장 대비)
    AtomicU32 pendingAckCount;  // 발신 코어가 세팅, 수신 코어마다 ACK 시 감소(libkenv/spinlock.h)
};

// 코어 수만큼 여러 샷다운이 동시에 겹칠 수 있으므로(드물지만), 요청
// 슬롯을 여러 개 두는 것을 제안 - v1은 KernelAddressSpaceManager의
// 전역 Spinlock(SP-2AAD7C8D §2)이 이미 매핑 변경 자체를 직렬화하므로
// **슬롯 1개로 충분**하다(동시에 두 개의 커널 영역 매핑 변경이 진행
// 중일 수 없음).
extern TlbShootdownRequest g_tlbShootdownRequest;
```

### 2.2 Lapic 확장 - 고정 벡터 IPI 전송 (신규)

`Lapic`은 현재 `sendInitIpi`/`sendStartupIpi`(INIT-SIPI 전용, PL-65C20380)
만 있고, 일반 고정 벡터 IPI를 보내는 메서드가 없다 - 이 문서가
처음으로 요구한다:

```cpp
// lapic.h에 추가 제안 - ICR을 Fixed delivery mode + 물리 목적지로
// 채워 보낸다(INIT/SIPI와 같은 ICR 레지스터, delivery mode 필드만
// 다름 - sendInitIpi/sendStartupIpi 옆에 나란히 배치 자연스러움).
static void sendFixedIpi(uint32_t destApicId, uint8_t vector);
```

### 2.3 브로드캐스트 - 자신을 제외한 모든 온라인 코어에게

```cpp
// minicore/kernel/tlb_shootdown.cpp
void kBroadcastTlbShootdown(uint64_t virtStart, uint64_t virtEnd) {
    g_tlbShootdownRequest.virtStart = virtStart;
    g_tlbShootdownRequest.virtEnd = virtEnd;
    g_tlbShootdownRequest.targetPml4Phys = 0;  // 커널 영역 전용(v1)

    uint32_t selfApicId = Lapic::id();
    uint32_t otherCount = 0;
    for (uint32_t i = 0; i < Acpi::cpuCount(); ++i) {
        if (Acpi::cpuApicId(i) != selfApicId) ++otherCount;
    }
    g_tlbShootdownRequest.pendingAckCount.store(otherCount);

    // 이 코어 자신은 IPI 없이 바로 invlpg(§2.4의 ISR과 동일 절차).
    kInvalidateRange(virtStart, virtEnd);

    for (uint32_t i = 0; i < Acpi::cpuCount(); ++i) {
        uint32_t destApicId = Acpi::cpuApicId(i);
        if (destApicId == selfApicId) continue;
        Lapic::sendFixedIpi(destApicId, kTlbShootdownVector);
    }

    // busy-wait - PreemptionGuard(SP-D7013B26 §2.1 패턴 재사용)로
    // 이 대기 구간 자체가 선점/마이그레이션되지 않도록 보호한다.
    while (g_tlbShootdownRequest.pendingAckCount.load() != 0) {
        asm volatile("pause");
    }
}
```

### 2.4 ISR - 수신 측

```cpp
// idt.cpp에 kTlbShootdownVector로 등록.
extern "C" void kTlbShootdownIsr() {
    const TlbShootdownRequest& req = g_tlbShootdownRequest;
    bool shouldInvalidate = (req.targetPml4Phys == 0) ||
                             (req.targetPml4Phys == Paging::currentPml4Phys());
    if (shouldInvalidate) {
        kInvalidateRange(req.virtStart, req.virtEnd);
    }
    req.pendingAckCount.fetchSub(1);  // "그냥 바로 ACK"도 이 한 줄로 통일 - invalidate 여부와 무관하게 항상 감소
    Lapic::sendEoi();
}
```

`kInvalidateRange`는 `[virtStart, virtEnd)`를 페이지 크기(4KiB)만큼
순회하며 `invlpg`를 반복 호출하는 단순 헬퍼(이미 있는
`Paging::unmapPage`의 단일 `invlpg` 호출을 범위용으로 일반화한 것).

## 3. 동시성/정합성

- **`KernelAddressSpaceManager`의 전역 Spinlock**(SP-2AAD7C8D §2)이
  이미 "커널 영역 매핑 변경은 한 번에 하나씩만" 직렬화하므로, §2.1의
  요청 슬롯이 하나뿐이어도 경쟁이 생기지 않는다 - 매핑을 바꾸는
  코드 경로 자체가 이 락을 쥔 채로 `kBroadcastTlbShootdown`을
  호출해야 한다(락 해제 전에 샷다운까지 끝내야 다음 변경이 이전
  요청과 뒤섞이지 않는다).
- **ISR 재진입/인터럽트 우선순위**: 이 벡터는 다른 모든 코어에서
  "지금 하던 일을 멈추고 즉시 처리"해야 하므로, 마스킹 가능한 일반
  인터럽트보다 높은 우선순위 클래스에 배치하는 것을 제안(정확한
  벡터 번호/우선순위 값은 `idt.cpp`의 기존 벡터 배치 관례를 따라
  구현 시점에 배정).
- **부팅 초기(AP가 아직 하나도 안 뜬 시점, PL-65C20380 이전)**:
  `Acpi::cpuCount() == 1`이면 브로드캐스트 루프가 즉시 끝나고
  `pendingAckCount`도 0에서 시작하므로 자연히 안전 - SMP가 아직
  준비 안 된 상태에서도 별도 분기 없이 동작한다.

## 4. 추가 최적화 기법 (설계자 제시, 후속 검토 - v1 필수 아님)

설계자가 함께 제시한 대안/보완 기법들 - v1 IPI-ISR-ACK 메커니즘의
기본 골격은 위와 같이 확정하되, 아래는 실측 후 도입 여부를 판단할
후속 최적화로 기록해 둔다:

- **Flush batching**: 짧은 시간 안에 커널 영역 매핑 변경이 여러 번
  일어나면(예: 큰 영역을 여러 페이지로 나눠 매핑 해제) 매번 샷다운을
  하지 않고 변경분을 모았다가 한 번의 IPI 라운드로 처리 - §2.1의
  요청 구조체를 "범위 하나"에서 "범위 배열"로 확장하면 자연스럽게
  수용 가능.
- **Lazy TLB**: 정말 그 주소를 참조하기 전까지는 무효화를 미루는
  기법(Linux의 lazy TLB 모드처럼, 유휴 코어나 커널 스레드 컨텍스트가
  당장 그 매핑을 안 쓸 걸 알 때 유용) - 이 프로젝트는 아직 "코어가
  지금 어떤 매핑을 실제로 쓰고 있는지"를 정밀하게 추적하는 인프라가
  없어 v1 범위 밖.
- **선점 지점 마킹 + busy-wait 대안**: IPI/ISR 대신, 각 코어에
  "다음 Task 전환 전에 잠깐 멈춰서 스스로 invlpg하라"는 플래그를
  세우고 발신 코어가 그 플래그들이 전부 처리될 때까지 busy-wait하는
  방식 - 인터럽트 없이 스케줄러의 자연스러운 체크포인트(Task 전환
  시점)에 편승하는 장점이 있지만, **그 체크포인트가 오기까지의
  지연이 스케줄러 틱 주기에 종속**된다는 단점이 있다(IPI는 즉시,
  이 방식은 최악의 경우 한 스케줄러 퀀텀만큼 지연). 커널 영역 매핑
  변경은 빈도가 낮고 정합성이 중요하므로 v1은 즉시성이 보장되는
  IPI-ISR 방식을 기본으로 채택하고, 이 대안은 실측 후 IPI 오버헤드가
  실제로 문제가 되는 경우에만 재검토.

## 5. 아직 열려 있는 설계 영역

1. **유저 영역(프로세스별) 샷다운 확장**: §2.1의 `targetPml4Phys`
   필드는 이미 이 확장을 염두에 두고 설계했지만, "어느 코어가 지금
   이 프로세스를 실행 중인지" 추적하는 스케줄러 쪽 인프라
   (PL-2D3184BC)가 실측 가능한 수준으로 갖춰진 뒤에 실제로 연결.
   **[비판적 재검토, 발견]** §2.1의 "요청 슬롯 1개로 충분"이라는
   전제는 `KernelAddressSpaceManager`의 전역 Spinlock이 **유일한
   호출 경로**라는 가정에 의존한다 - 이 확장이 실제로 연결되면
   `ProcessAddressSpaceManager`(프로세스별, SP-2AAD7C8D §2)가 새로운
   두 번째 호출 경로가 되는데, 이건 커널 영역 Spinlock과 무관한 락이라
   서로 다른 프로세스의 유저 영역 매핑 변경이 서로 다른 코어에서
   동시에 `kBroadcastTlbShootdown`을 부를 수 있다 - 그러면 단일
   `g_tlbShootdownRequest` 슬롯이 두 요청 사이에서 경합해 깨진다.
   착수 시점에 "요청 슬롯을 여러 개로 늘릴지" 또는 "모든 샷다운
   호출 경로를 감싸는 별도의 전역 직렬화 락을 새로 둘지" 결정이
   필요하다(v1 커널 전용 범위에서는 무관 - 이 확장이 실제로 시작될
   때 재확인). **계획 PN-D132A1E9로 추적 중.**

   **[확정, 2026-09-16, QU-DE2828A1 답변]** (A) 요청 슬롯 다중화로
   확정. 설계자가 구체적인 두 가지 방향까지 함께 지정했다:
   1. **유저 영역 샷다운은 시스템 전체 브로드캐스트가 아니다** -
      커널 영역과 달리, 대상 프로세스가 **지금 실제로 스케줄링되어
      실행 중인 코어들**(Active CPU Mask, `gCurrentTask[]` 스캔으로
      구함 - PN-D132A1E9가 이미 확인)에게만 IPI를 보내도록 범위를
      좁힌다.
   2. **수신측 다중 슬롯 처리**: 요청자별 슬롯(Per-Requester Slot) +
      수신자별 대기 마스크(Target Pending Mask)를 결합한다.
      - 하나의 물리 코어는 커널 모드에서 동기적으로 TLB Shootdown을
        요청하면 그 스레드가 블로킹되므로, **한 번에 하나의 요청만
        발생**시킬 수 있다.
      - 따라서 별도의 글로벌 큐 없이, **시스템 코어 수만큼 배열**을
        두고 각 코어가 **자신의 로컬 코어 ID에 해당하는 슬롯에만
        쓴다** - 슬롯 쓰기 자체에 락이 전혀 필요 없다(요청자 코어가
        곧 슬롯 인덱스이므로 쓰기 경합이 구조적으로 불가능).
      - 수신측(IPI 핸들러)은 자신을 깨운 슬롯(들)의 "이 코어를 향한
        대기 중" 표시(수신자별 Target Pending Mask)를 확인해 처리할
        요청을 식별한다.
2. **정확한 벡터 번호/인터럽트 우선순위 배정**: **[확정, 2026-09-16]**
   `0xE0`으로 배정 - 전용 현황판 RM-28225668("Minicore 인터럽트 벡터
   목록") 신설에 맞춰 등재(우선순위 클래스 자체는 여전히 구현 착수
   시점에 `idt.cpp` 관례대로 확정).
3. **§4의 최적화 기법 도입 여부**: 실측 후 재검토(위 §4 참고).

## 6. 선행 조건

- `Lapic::sendFixedIpi`(§2.2, 신규) - 기존 `sendInitIpi`와 같은 ICR
  경로를 재사용하는 확장.
- `AtomicU32`(`libkenv/spinlock.h`, 이미 구현 완료) - `pendingAckCount`.
- `Acpi::cpuCount()`/`cpuApicId()`(이미 구현 완료, PL-65C20380) - 온라인
  코어 목록.
- `KernelAddressSpaceManager`(SP-2AAD7C8D) - 이 샷다운의 유일한 v1
  소비자(커널 영역 매핑 변경 시 호출).
- `Paging::currentPml4Phys()`(이미 구현 완료) - §5-A 확장 시 필요.

