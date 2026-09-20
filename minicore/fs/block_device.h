#ifndef MINICORE_FS_BLOCK_DEVICE_H
#define MINICORE_FS_BLOCK_DEVICE_H

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

namespace fs {

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
    virtual bool readBlocks(kernel::uint64_t lba, kernel::uint32_t count, void* buf) = 0;
    virtual bool writeBlocks(kernel::uint64_t lba, kernel::uint32_t count, const void* buf) = 0;
    // 컨트롤러/장치 자체 쓰기 캐시를 안정 매체까지 밀어낸다(AHCI의
    // FLUSH CACHE류 ATA 명령에 대응) - 저널링 파일시스템(ext4)의
    // 배리어/커밋 지점에서 필수.
    virtual bool flush() = 0;
    // 최적화용 힌트 - 미지원 장치/드라이버는 항상 true(아무것도 안
    // 하고 성공 처리)를 반환해도 무방하다(SSD TRIM처럼 데이터 정확성엔
    // 영향 없음).
    virtual bool trim(kernel::uint64_t lba, kernel::uint32_t count) = 0;
};

}  // namespace fs

#endif  // MINICORE_FS_BLOCK_DEVICE_H
