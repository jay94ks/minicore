#ifndef MINICORE_LIBS_X86_64_IO_PORT_H
#define MINICORE_LIBS_X86_64_IO_PORT_H

// minicore/libs/x86_64 - x86-64 아키텍처별 공통 코드 라이브러리
// (SP-8B6B8D25 §3.0) - 커널(및 나중에 커널 서비스)이 직접 링크해
// 재사용할 수 있는 아키텍처 종속 프리미티브. 포트 I/O는 x86 계열
// ISA 명령어라 다른 아키텍처엔 이 형태로 존재하지 않는다 - 그래서
// libkenv(아키텍처 무관 early 런타임)가 아니라 여기 있다.
//
// kernel::arch 네임스페이스(2026-09-14, 설계자 확정 - QU-4606360C):
// 유저랜드 프로세스가 커널과의 통신/권한 처리를 거쳐 IO 권한을 얻은
// 뒤에는 이 라이브러리를 그대로 링크해 직접 포트 IO를 할 수 있게
// 하려는 의도다 - 그래도 이름은 kernel::arch를 그대로 쓴다(누가
// 링크하느냐와 무관하게 네임스페이스 경로는 고정).

namespace kernel {
namespace arch {

inline void kOutB(unsigned short port, unsigned char value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline unsigned char kInB(unsigned short port) {
    unsigned char value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void kOutW(unsigned short port, unsigned short value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline unsigned short kInW(unsigned short port) {
    unsigned short value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

// PCI 설정 공간(포트 0xCF8/0xCFC)의 CONFIG_ADDRESS 자체는 항상 32비트
// 단위로 쓴다 - CONFIG_DATA(0xCFC)는 오프셋의 하위 2비트를 더한
// 포트로 8/16/32비트 폭에 맞춰 접근하면 칩셋이 알아서 해당 바이트
// 레인만 골라준다(레지스터 하나를 통째로 읽어 마스킹하는 것보다
// 이 방식이 더 정확함 - pci.cpp가 이렇게 쓴다).
inline void kOutL(unsigned short port, unsigned int value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline unsigned int kInL(unsigned short port) {
    unsigned int value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

}  // namespace arch
}  // namespace kernel

#endif  // MINICORE_LIBS_X86_64_IO_PORT_H
