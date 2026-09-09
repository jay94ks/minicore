// 최소 legacy virtio-blk 클라이언트 (docs/plan/system-servers-bringup.md
// §M12, ADR-131/147). 블로킹, 읽기전용, 큐 1개, 요청 1개(부팅 시
// 부트 디바이스 전체를 한 번에 읽어들이는 용도)만 지원한다 —
// M14~M16의 "진짜" virtio-blk 서버(devmgr 산하)와는 완전히 별개의
// 코드다(ADR-131 §근거: 부트스트랩 1회성 전용이라 요구사항 자체가
// 다르다 — 에러 복구, 다중 요청 동시 처리, 인터럽트 기반 완료
// 통지를 전혀 다루지 않는다).
//
// legacy virtio는 BAR0(I/O 공간, 커널이 kernel_main.cpp에서 이미
// 배정 + TSS IOPB로 열어 둠, ADR-147)의 고정 오프셋 레지스터만으로
// 동작해 모던 virtio(MMIO BAR + capability list 파싱)보다 훨씬
// 단순하다.
#pragma once

#include <cstdint>

namespace virtio_blk {

// io_base(kernel이 boot_info.boot_device.io_port_base로 알려 준 값)의
// 장치를 리셋→feature 협상(0으로, 옵션 기능 없음)→큐 0 설정까지
// 마친다. dma_virt/dma_phys는 sys_alloc_dma_buffer(order>=10, 4MiB
// 이상)로 이미 확보해 둔 버퍼 — 이 함수가 그 앞부분(최대 16KiB)을
// vring 전용으로 쓴다. 성공하면 true.
bool init(uint16_t io_base, uint8_t* dma_virt, uint64_t dma_phys);

// 디바이스가 보고하는 용량(섹터 수, 512바이트/섹터)을 min(용량,
// max_bytes/512)로 제한해 섹터 0부터 그만큼을 dma_virt의 vring 뒤
// 영역으로 블로킹 읽기 한다. 성공하면 실제로 읽은 바이트 수를
// out_len에 채우고 true — out_data에 그 시작 주소(dma_virt 안의
// 오프셋)를 채운다. init() 이후에만 호출 가능. 완료 대기는 순수
// 폴링이다(인터럽트 없음) — 유한 횟수만 돌고 그래도 안 끝나면
// false.
bool read_all(uint16_t io_base, uint8_t* dma_virt, uint64_t dma_phys, uint64_t max_bytes,
              const uint8_t** out_data, uint64_t* out_len);

}  // namespace virtio_blk
