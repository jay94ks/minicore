// mc/lifecycle_client.h — 서비스 생명주기 신호(준비완료 등)를 위한
// 얇은 래퍼(user-service-manager.md §M40,
// docs/design/boot-and-drivers.md ADR-193, OPEN-52 해소). 새 커널
// 프리미티브를 추가하지 않는다 — 이미 있는 "spawn 시점 전용
// endpoint"(create_endpoint=true, ADR-151/152) 위의 Call/Reply를
// "준비완료"라는 특정 의미로 재사용할 뿐이다(ADR-178/M22의 wait
// 회수와 같은 모양, 방향만 반대 — wait은 부모가 Call/자식이 Reply,
// 준비완료는 자식이 Call/부모가 Reply).
#pragma once

#include <stdint.h>

// C++ 소비자(servers/svcmgr 등)를 위한 extern "C" — vfs_client.h의
// 같은 주석 참고(M40 실행 중 발견한 일반 패턴).
#ifdef __cplusplus
extern "C" {
#endif

// ADR-152의 고정 순서(create_endpoint=true면 언제나 handle 1)와
// ADR-193이 예약한 label 0.
#define MC_SERVICE_READY_OWN_HANDLE 1u
#define MC_SERVICE_READY_LABEL 0u

// 스폰된 서비스 쪽에서 부른다 — 자기 초기화가 끝났다는 뜻으로 자신의
// handle 1(owning endpoint)에 label=MC_SERVICE_READY_LABEL Call을
// 보내고, 스폰한 쪽의 mc_wait_ready()가 Reply할 때까지 블록한다.
// 이 함수를 부르려면 그 프로세스가 spawn 시점에 실제로
// create_endpoint=true로 만들어졌어야 한다(handle 1이 존재해야
// 함) — 그렇지 않으면 정의되지 않은 핸들을 참조하게 된다(호출자
// 책임, 이 계층은 검증하지 않는다).
void mc_signal_ready(void);

// 스폰한 쪽에서 부른다 — sys_process_spawn(create_endpoint=true)이
// 돌려준 out_endpoint_proxy_handle로 그 서비스의 준비완료 Call을
// 블로킹 대기하다가 받으면 즉시 Reply해 그 서비스가 계속 진행하게
// 한다.
void mc_wait_ready(uint32_t endpoint_proxy_handle);

#ifdef __cplusplus
}  // extern "C"
#endif
