#ifndef USERLAND_LIBS_LIBMC_MC_TYPES_H
#define USERLAND_LIBS_LIBMC_MC_TYPES_H

// libmc: 유저랜드도 freestanding이라 <cstdint>가 없다(minicore/libs/
// libkenv/types.h와 같은 이유 - 유저/커널은 애초에 다른 툴체인이라
// 오브젝트도 공유 안 됨, PN-58501EAA 참고). 네임스페이스 규칙은 커널
// 앱에만 적용되므로(CLAUDE.md) kernel:: 대신 이 라이브러리 전용 mc::를
// 쓴다 - 커널의 kernel::uint32_t 등과 이름은 같지만 서로 다른 타입.

namespace mc {

using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long;

using int8_t = signed char;
using int16_t = short;
using int32_t = int;
using int64_t = long;

using size_t = decltype(sizeof(0));

}  // namespace mc

#endif  // USERLAND_LIBS_LIBMC_MC_TYPES_H
