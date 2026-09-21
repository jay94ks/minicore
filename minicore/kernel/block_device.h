#ifndef MINICORE_KERNEL_BLOCK_DEVICE_H
#define MINICORE_KERNEL_BLOCK_DEVICE_H

#include "libkenv/types.h"

// [SP-2BCE5D60 §3.0, 확정 2026-09-18] fs 서비스 내부 - 파일시스템
// 드라이버(§3.1)가 소비하는 공통 블록 장치 인터페이스. AHCI/USB/향후
// NVMe 등 실제 전송 프로토콜과 무관하게 LBA(논리 블록 주소) 단위로만
// 이야기한다. **[뒤집힘, 2026-09-20, QU-1FB6A7A4 답변 - "블록
// 디바이스는 그냥 아예 fs한테 던져버려. 인식/인식 해제까지 전부."]**
// 이 인터페이스의 구현체(예: AhciBlockDevice, ahci.h)는 devmgr이
// 아니라 fs 프로세스 자신 안에서 직접 인식/구동된다 - 별도 프로세스
// 간 Channel 핸드오프가 필요 없다(같은 주소공간).
// **[전환, 2026-09-20, SP-43331889/QU-5FC58B06 - fs를 유저랜드
// 프로세스에서 Process 없는 순수 커널 Task로 완전 흡수]** `mc::` 타입
// (유저랜드 전용, libmc)을 `kernel::` 타입(libkenv)으로 교체 - 이제
// 커널 자신과 같은 툴체인/네임스페이스를 쓴다(devmgr/main.cpp와
// 동일한 전환).
// **[재설계, 2026-09-22, PN-A401DDF9, SP-C2670F69 §3.5 NCQ,
// QU-47203076 답변(옵션 B)]** readBlocks/writeBlocks가 동기 단일요청
// 이던 것을 kernel::AsyncTask 기반 비동기 제출 API로 재설계 - 설계자가
// "readBlocks/writeBlocks 자체를 비동기식으로 재설계"(2026-09-20)를
// 확정한 뒤, 정확한 API 모양(단일 제출만 vs 배치 API+동기 래퍼까지)을
// 재질의해 옵션 B(배치 API + 기존 동기 시그니처를 편의 래퍼로 유지)로
// 확정받았다.

namespace kernel {
struct AsyncTask;
}  // namespace kernel

namespace fs {

// submitReadBlocks/submitWriteBlocks 및 배치 변형의 완료 결과 - 호출부가
// 소유하며, 반환된 kernel::AsyncTask가 완료(Completed/Failed/Cancelled)
// 상태에 도달할 때까지 유효해야 한다(vfs_syscall.cpp의 `KernelFsOpenArgs`
// 류 "제출 시점의 args는 호출부가 그 완료를 관측할 때까지 살려 둔다"는
// 기존 관례를 이 인터페이스 경계로 그대로 옮긴 것).
struct BlockIoResult {
    bool ok = false;
};

// submitReadBlocksBatch에 넘기는 배치 항목 하나.
struct BlockReadRequest {
    kernel::uint64_t lba = 0;
    void* buffer = nullptr;
    kernel::uint32_t count = 0;
};

// submitWriteBlocksBatch에 넘기는 배치 항목 하나.
struct BlockWriteRequest {
    kernel::uint64_t lba = 0;
    const void* buffer = nullptr;
    kernel::uint32_t count = 0;
};

// 가상 소멸자를 일부러 안 둔다 - 이 인터페이스의 어떤 구현체도(현재
// 유일한 `AhciBlockDevice` 포함) `new`로 힙 할당되거나 `delete
// BlockDevice*`로 해제되지 않는다(이 코드베이스 전역 관례 - 정적/
// 전역 인스턴스만 사용) - 가상 소멸자를 선언하면 컴파일러가 파생
// 클래스마다 "deleting destructor" thunk를 생성해 `operator delete`
// 심볼을 링크 시점에 요구하게 돼(유저랜드 시절 실측으로 발견 - 링커
// 에러), 실제로 안 쓰는 기능 때문에 빌드가 깨진다. SP-2BCE5D60 §3.0
// 원안 자체도 가상 소멸자를 선언하지 않았다.
class BlockDevice {
public:
    virtual kernel::uint32_t blockSize() const = 0;   // 보통 512 또는 4096
    virtual kernel::uint64_t blockCount() const = 0;  // 용량 = blockSize() * blockCount()

    // [PN-A401DDF9] 비동기 단일 제출 - 즉시 kernel::AsyncTask*를
    // 반환한다(autoFree=false, 완료 후 그 AsyncTask 구조체 자체의
    // 반납은 호출부 책임 - kernel::AsyncTaskWaitGroup/AsyncTaskGroup/
    // AsyncTaskAwaiter에 넘기면 그 유틸리티들이 완료 감지와 함께 대신
    // 반납한다, async_task.h 참고). outResult는 호출부 소유 - 반환된
    // AsyncTask가 끝날 때까지 유효해야 한다. 슬롯 고갈 등으로 제출
    // 자체가 불가능하면 nullptr(outResult는 건드리지 않는다).
    virtual kernel::AsyncTask* submitReadBlocks(kernel::uint64_t lba, void* buf, kernel::uint32_t count,
                                                 BlockIoResult* outResult) = 0;
    virtual kernel::AsyncTask* submitWriteBlocks(kernel::uint64_t lba, const void* buf, kernel::uint32_t count,
                                                  BlockIoResult* outResult) = 0;

    // [PN-A401DDF9] 배치 제출 - requests/outTasks/outResults 세 배열은
    // 전부 호출부가 소유하며 count 크기로 미리 준비해 둬야 한다(같은
    // 인덱스끼리 대응). 각 항목을 submitReadBlocks/submitWriteBlocks로
    // 개별 제출한 것과 동일하게 동작하되(각자 독립된 AsyncTask), 여러
    // 개를 반복문 없이 한 번에 밀어넣는 편의를 준다 - 파생 클래스가
    // 재정의할 필요 없는 순수 편의 계층(제출 자체는 두 가상함수를
    // 그대로 재사용, block_device.cpp). 개별 제출이 실패한(슬롯 고갈
    // 등) 자리는 outTasks[i]=nullptr로 남는다.
    void submitReadBlocksBatch(const BlockReadRequest* requests, kernel::uint32_t count, kernel::AsyncTask** outTasks,
                                BlockIoResult* outResults);
    void submitWriteBlocksBatch(const BlockWriteRequest* requests, kernel::uint32_t count,
                                 kernel::AsyncTask** outTasks, BlockIoResult* outResults);

    // [PN-A401DDF9] 동기 편의 래퍼 - submitReadBlocks/submitWriteBlocks
    // 제출 후 그 자리에서 완료까지 기다린다(제출 자체가 실패하면 즉시
    // false). 새 하드웨어 경로가 아니라 위 비동기 API 위에 얹힌 순수
    // 편의 계층(block_device.cpp) - 호출부 마이그레이션 부담을 줄이기
    // 위해 옛 동기 시그니처를 그대로 유지한다(QU-47203076 답변).
    bool readBlocks(kernel::uint64_t lba, kernel::uint32_t count, void* buf);
    bool writeBlocks(kernel::uint64_t lba, kernel::uint32_t count, const void* buf);

    // 컨트롤러/장치 자체 쓰기 캐시를 안정 매체까지 밀어낸다(AHCI의
    // FLUSH CACHE류 ATA 명령에 대응) - 저널링 파일시스템(ext4)의
    // 배리어/커밋 지점에서 필수. 드물게 불리는 경로라 당분간 동기로
    // 남긴다(QU-47203076 - 비동기화는 실사용 패턴이 드러난 뒤 재검토).
    virtual bool flush() = 0;
    // 최적화용 힌트 - 미지원 장치/드라이버는 항상 true(아무것도 안
    // 하고 성공 처리)를 반환해도 무방하다(SSD TRIM처럼 데이터 정확성엔
    // 영향 없음).
    virtual bool trim(kernel::uint64_t lba, kernel::uint32_t count) = 0;
};

}  // namespace fs

#endif  // MINICORE_KERNEL_BLOCK_DEVICE_H
