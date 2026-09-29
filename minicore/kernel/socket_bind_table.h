#ifndef MINICORE_KERNEL_SOCKET_BIND_TABLE_H
#define MINICORE_KERNEL_SOCKET_BIND_TABLE_H

#include "libkenv/types.h"

namespace kernel {

// [신규, PN-E310E23A, SP-231493CB §4-2(QU-0795993D 답변 "소켓을
// Channel용으로 설계된 테이블에서 분리해")] 소켓 Bind() 전용
// 이름공간 - named_object.h(Channel 전용, §4-1 자동 등록 경로와
// §4-2 이전엔 소켓 Bind()도 함께 공유하던 테이블)와 완전히 분리된
// 별도 슬롯 배열이다. 인터페이스는 named_object.h와 동일하게
// reserve/resolve/release/getByIndex 네 개를 그대로 복제하되(설계
// 문서가 명시적으로 지시한 대칭), 저장하는 값이 항상 소켓이 위임하는
// 익명 Channel의 ChannelId 하나뿐이라 NamedObjectTable처럼 종류
// (kind) 태그를 둘 필요는 없다.
//
// Bind()는(POSIX 관례상) 리슨 소켓 하나당 보통 한 번만 호출되는
// 저빈도 동작이라(§4-1의 매 연결 Accept()와 달리) NamedObjectTable과
// 같은 정적 상한을 그대로 적용해도 무방하다(§4-2 그대로).
constexpr uint32_t kMaxSocketBindings = 128;
constexpr uint32_t kMaxSocketBindNameLength = 64;

class SocketBindTable {
public:
    // name/nameLength로 channelId를 예약한다 - 이미 쓰이는 이름이면
    // 즉시 실패(NamedObjectTable::reserve와 동일한 정책).
    static bool reserve(const char* name, uint64_t nameLength, uint64_t channelId);

    // 이름으로 조회 - 없으면 false, 있으면 outChannelId를 채운다.
    static bool resolve(const char* name, uint64_t nameLength, uint64_t* outChannelId);

    // 이 이름을 반납해 재사용 가능하게 한다 - 없는 이름이면 아무 일도
    // 안 한다.
    static void release(const char* name, uint64_t nameLength);

    // [named_object.h getByIndex와 동일한 관례] 나열(Readdir) 전용 -
    // 인덱스는 "사용 중인 슬롯만 순서대로 센 몇 번째인지"를 뜻한다.
    // 현재 이 테이블을 나열하는 VFS 경로는 없다(§4-2는 listing
    // 요구사항을 명시하지 않음) - named_object.h와의 인터페이스
    // 대칭을 위해 미리 갖춰 둔다.
    static bool getByIndex(uint32_t index, char* outName, uint32_t* outNameLength);
};

}  // namespace kernel

#endif  // MINICORE_KERNEL_SOCKET_BIND_TABLE_H
