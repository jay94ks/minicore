#ifndef MINICORE_LIBS_X86_64_IO_PORT_H
#define MINICORE_LIBS_X86_64_IO_PORT_H

// minicore/libs/x86_64 - x86-64 아키텍처별 공통 코드 라이브러리
// (SP-8B6B8D25 §3.0) - 커널(및 나중에 커널 서비스)이 직접 링크해
// 재사용할 수 있는 아키텍처 종속 프리미티브. 포트 I/O는 x86 계열
// ISA 명령어라 다른 아키텍처엔 이 형태로 존재하지 않는다 - 그래서
// libkenv(아키텍처 무관 early 런타임)가 아니라 여기 있다.
//
// namespace를 kernel로 둔 건 지금 유일한 소비자가 커널뿐이라서다 -
// 나중에 유저랜드 코드가 이 라이브러리를 직접 링크하게 되면 이
// 네임스페이스 선택을 다시 봐야 한다(임시 결정, 관련 질의 등록됨).

namespace kernel {

inline void kOutB(unsigned short port, unsigned char value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline unsigned char kInB(unsigned short port) {
    unsigned char value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

}  // namespace kernel

#endif  // MINICORE_LIBS_X86_64_IO_PORT_H
