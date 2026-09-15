#ifndef MINICORE_KERNEL_PERCPU_H
#define MINICORE_KERNEL_PERCPU_H

#include "acpi.h"
#include "libkenv/types.h"
#include "scheduler.h"

namespace kernel {

// SP-0666DB3C §12 - "코어 개수만큼의 배열 + currentCoreIndex()로
// 인덱싱" 패턴(scheduler.cpp의 gCurrentTask[]/gPreemptDisableCount[]류,
// SP-D7013B26의 코어별 매거진)을 재사용 가능한 프리미티브로 공식화.
//
// **문서(§12.2)는 이 클래스를 minicore/libs/libkenv에 두자고 제안했지만,
// get()이 Scheduler::currentCoreIndex()를 호출해야 해서 실제로는
// kernel:: 의존성이 생긴다** - libkenv는 이 프로젝트 관례상(RM-23F4B687,
// CLAUDE.md 디렉터리 규칙) 아키텍처 무관 early 런타임 전용이라 kernel/
// 코드를 참조하면 안 된다. §12.2 자체가 "가칭"으로만 명시한 배치라
// 설계 결정이 아니라 구현 세부라고 판단해, 기존 레이어링을 지키는
// minicore/kernel로 대신 두었다(RM-23F4B687 §4 - 임의 설계는 질의로
// 등록하는 원칙과 달리, 이건 순수 파일 위치 조정이라 별도 질의 없이
// 진행).
template <typename T>
class PerCpu {
public:
    // 이 코어(호출 시점의 Scheduler::currentCoreIndex())의 값에 대한
    // 참조.
    T& get() { return _values[Scheduler::currentCoreIndex()]; }

    // 다른 코어의 값을 들여다봐야 할 때(로드밸런싱 판단, 진단 등) 쓴다
    // - 동시성 보호는 T 자신의 책임(PerCpu는 배열 인덱싱만 제공).
    T& forCore(uint32_t coreIndex) { return _values[coreIndex]; }

private:
    T _values[kAcpiMaxCpus];
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_PERCPU_H
