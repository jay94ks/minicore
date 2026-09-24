#include "diag_ring.h"

#include "acpi.h"
#include "logger.h"

namespace kernel {

namespace {

// [갱신, 2026-09-23, PN-E4C6AF72 실측] 48개는 너무 작았다 - HPET(벡터
// 22)/LAPIC 타이머(벡터 24) 같은 고빈도 벡터가 아주 짧은 시간에 버퍼
// 전체를 여러 번 덮어써, 정작 원인 규명에 필요한 StackfulBegin/End나
// 드문 벡터 진입이 크래시 시점 이전에 이미 밀려나 사라졌다(실측
// 확인 - core0 크래시 덤프에 벡터22/24 반복만 가득하고 StackfulBegin/
// End가 전혀 안 보임). 훨씬 더 긴 창을 남기기 위해 크게 늘린다(정적
// BSS 배열, 코어당 4096*24바이트 ≈ 98KiB - 이 정도 메모리 여유는
// 이 프로젝트 규모에서 문제 없음).
constexpr uint32_t kDiagRingCapacity = 4096;

struct DiagRingEntry {
    uint64_t seq = 0;
    uint64_t rsp = 0;
    uint32_t vector = 0;
    uint8_t event = 0;
    // [신규, 2026-09-23, PN-E4C6AF72 6차 "남은 것" 1번] 이 이벤트가
    // 찍힌 순간의 RFLAGS.IF(비트9) - 일반 벡터는 절대 중첩 안 된다는
    // 전제가 성립하려면 EnterIsr~LeaveIsr 사이는 항상 0이어야 한다.
    // 코드 리뷰만으로 IF가 다시 켜지는 지점을 못 찾아 직접 실측하기
    // 위해 추가 - 호출부 시그니처는 안 바꾸고 이 함수 안에서 직접
    // 읽는다(모든 기존 kDiagRingLog 호출부가 자동으로 덕을 본다).
    bool ifFlag = false;
    // [신규, 2026-09-24, PN-61D908EB/PN-E4C6AF72] diag_ring.h 주석
    // 참고 - Enter/LeaveInterruptStack만 InterruptFrame::cs를 채운다.
    uint64_t extra = 0;
};

struct DiagRingBuffer {
    DiagRingEntry entries[kDiagRingCapacity];
    uint64_t nextSeq = 0;
};

// 코어별 독립 슬롯 - 각 코어는 항상 자기 슬롯만 쓰므로(호출부가 전부
// Scheduler::currentCoreIndex()로 coreIndex를 정함) 락이 필요 없다.
DiagRingBuffer gDiagRings[kAcpiMaxCpus];

const char* kEventName(uint8_t event) {
    switch (static_cast<DiagRingEvent>(event)) {
        case DiagRingEvent::EnterInterruptStack:
            return "EnterIsr";
        case DiagRingEvent::LeaveInterruptStack:
            return "LeaveIsr";
        case DiagRingEvent::StackfulDispatchBegin:
            return "StackfulBegin";
        case DiagRingEvent::StackfulDispatchEnd:
            return "StackfulEnd";
        case DiagRingEvent::DynamicDispatchEnter:
            return "DynDispatchEnter";
        case DiagRingEvent::DynamicDispatchExit:
            return "DynDispatchExit";
        case DiagRingEvent::DrainOnceTaskFound:
            return "DrainTaskFound";
        case DiagRingEvent::DrainOnceCoroBranch:
            return "DrainCoroBranch";
        case DiagRingEvent::DrainOnceStackfulBranch:
            return "DrainStackfulBranch";
        case DiagRingEvent::BootGdtInitDone:
            return "BootGdtInitDone";
        case DiagRingEvent::BootTssLoadDone:
            return "BootTssLoadDone";
        case DiagRingEvent::BootBeforeSti:
            return "BootBeforeSti";
        default:
            return "?";
    }
}

}  // namespace

void kDiagRingLog(DiagRingEvent event, uint32_t coreIndex, uint32_t vector, uint64_t rsp, uint64_t extra) {
    if (coreIndex >= kAcpiMaxCpus) {
        return;
    }
    uint64_t rflags = 0;
    asm volatile("pushfq; pop %0" : "=r"(rflags)::"memory");

    DiagRingBuffer& ring = gDiagRings[coreIndex];
    const uint64_t seq = ring.nextSeq++;
    DiagRingEntry& e = ring.entries[seq % kDiagRingCapacity];
    e.seq = seq;
    e.rsp = rsp;
    e.vector = vector;
    e.event = static_cast<uint8_t>(event);
    e.ifFlag = (rflags & (1ULL << 9)) != 0;
    e.extra = extra;
}

void kDiagRingDump(uint32_t coreIndex) {
    if (coreIndex >= kAcpiMaxCpus) {
        return;
    }
    const DiagRingBuffer& ring = gDiagRings[coreIndex];
    const uint64_t total = ring.nextSeq;
    const uint64_t count = total < kDiagRingCapacity ? total : kDiagRingCapacity;
    const uint64_t start = total < kDiagRingCapacity ? 0 : total - kDiagRingCapacity;
    Logger::info("minicore: diag ring dump (core=%u, total=%llu, showing last %llu)", coreIndex, total, count);
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t seq = start + i;
        const DiagRingEntry& e = ring.entries[seq % kDiagRingCapacity];
        Logger::info("  [%llu] %s vector=%x rsp=%llx if=%u cs=%llx", e.seq, kEventName(e.event), e.vector, e.rsp,
                     e.ifFlag ? 1u : 0u, e.extra);
    }
}

}  // namespace kernel
