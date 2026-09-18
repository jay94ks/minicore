#ifndef MINICORE_KERNEL_NAMED_OBJECT_H
#define MINICORE_KERNEL_NAMED_OBJECT_H

#include "libkenv/types.h"

namespace kernel {

// SP-1FBC0EEB "이름 있는 오브젝트 = /sys/live/named/ 가상 파일" -
// 메시징 채널/향후 큐/공유메모리 등 커널이 제공하는 모든 "이름 붙은
// IPC성 오브젝트"가 공유하는 단일 네임스페이스. 실제 VFS/fs 서비스가
// 아직 없어(설계 문서 그대로) 지금은 커널 자체 내부 테이블로
// 구현하고, 경로 표현(/sys/live/named/<name>)만 미리 맞춰 둔다 -
// procfs(SP-8B6B8D25 §2 13번)와 같은 성격.
enum class NamedObjectKind : uint32_t {
    Channel = 1,
    // 향후: Queue, SharedMemory 등 - 새 종류가 생겨도 이 테이블
    // 자체(reserve/resolve/release)는 손대지 않는다.
};

constexpr uint32_t kMaxNamedObjects = 128;       // v1 상한 - 필요해지면 늘림
constexpr uint32_t kMaxNamedObjectNameLength = 64;  // v1 상한

class NamedObjectTable {
public:
    // name/nameLength로 이 오브젝트를 예약한다 - 이미 쓰이는 이름이면
    // (종류 불문) 즉시 실패. **보안 정책(설계 문서 그대로)**: 실패
    // 사유는 항상 "이름 사용 불가" 하나뿐이다 - 기존 오브젝트가 어떤
    // 종류인지 호출부가 이 반환값만으로는 알 수 없다(이름 공간을
    // 훑어 종류를 추측하는 공격 방지). objectId는 보통 그 오브젝트
    // 구조체 자체의 포인터 값(Syscall 서브시스템의 토큰=포인터
    // 관례와 동일 - PL-21344323 참고, 별도 전역 ID 테이블 불필요).
    static bool reserve(const char* name, uint64_t nameLength, NamedObjectKind kind, uint64_t objectId);

    // 이름으로 조회 - 없으면 false. 있으면 kind/objectId를 채운다
    // (kind까지 알려주는 건 이 조회 자체가 "이 이름을 열겠다"가 아니라
    // "이미 아는 이름의 실체를 찾겠다"는 내부 호출이라 종류 은닉
    // 정책과 무관 - open 계열 API가 이름 충돌을 검사할 때만 종류를
    // 감춘 실패를 반환하면 된다).
    static bool resolve(const char* name, uint64_t nameLength, NamedObjectKind* outKind, uint64_t* outObjectId);

    // 이 이름을 반납해 재사용 가능하게 한다 - 없는 이름이면 아무 일도
    // 안 한다.
    static void release(const char* name, uint64_t nameLength);

    // [신규, 2026-09-19, PN-770A28FB] `/sys/live/named/` 나열(Readdir)
    // 전용 - 인덱스는 "사용 중인 슬롯만 순서대로 센 몇 번째인지"를
    // 뜻한다(빈 슬롯은 건너뜀, `KernelFsReaddirArgs::index`와 동일한
    // 관례). 있으면 이름/길이를 채우고 true, 범위를 벗어나면 false
    // (Readdir의 EOF 신호로 그대로 이어짐).
    static bool getByIndex(uint32_t index, char* outName, uint32_t* outNameLength);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_NAMED_OBJECT_H
