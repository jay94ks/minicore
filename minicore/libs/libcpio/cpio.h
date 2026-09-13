#ifndef MINICORE_LIBS_LIBCPIO_CPIO_H
#define MINICORE_LIBS_LIBCPIO_CPIO_H

#include <stddef.h>

// libcpio: CPIO 아카이브("New ASCII Format", 매직 "070701") 최소
// 파서 - initrd/부트 모듈을 커널이 읽는 데 쓴다(QU-9DCDCE3E, 설계자
// 지시, 2026-09-14 - "CPIO는 반드시 구현되어야 한다. libs/libcpio로
// kernel/userland 공용으로 사용할 수 있도록 구현해야 한다"). 그래서
// kernel:: 네임스페이스나 libkenv 전용 타입을 쓰지 않고 표준 C++
// 타입만으로 작성한다 - 커널(freestanding)/유저랜드(hosted) 양쪽에서
// 그대로 컴파일된다.
//
// 포맷 선택: 여러 CPIO 변종(odc/newc/newc+crc 등) 중 리눅스 initramfs
// 등 커널 관련 용도의 사실상 표준인 "New ASCII Format"(매직 "070701")
// 만 지원한다 - 별도 지시가 없는 한 이게 가장 합리적인 기본값이라
// 판단했다(다른 변종이 필요해지면 그때 확장).
//
// 메모리에 이미 통째로 올라온 아카이브(예: multiboot2/PVH 모듈)를
// 그 자리에서 그대로 훑는다 - 동적 할당도, 파일시스템 접근도 하지
// 않는다(그래서 커널 부팅 극초반에도 쓸 수 있음).
namespace cpio {

struct Entry {
    const char* name;       // null-terminated 파일 이름(아카이브 안 원본 포인터, 복사 없음)
    unsigned long nameLength;  // name의 길이(널 제외)
    const unsigned char* data;  // 파일 내용 시작 - 디렉터리/특수 파일은 nullptr일 수 있음
    unsigned long dataSize;
    unsigned int mode;       // POSIX 모드 비트(st_mode) - 상위 비트에 파일 타입 포함
};

// archive[0..archiveSize)를 훑으며 엔트리(트레일러 "TRAILER!!!" 제외)
// 마다 callback(entry, userData)을 부른다. 매직이 "070701"이 아니거나
// 손상된 헤더를 만나면 그 지점에서 조용히 멈춘다(부분 결과까지는
// 유효). 반환값: 실제로 콜백을 호출한 엔트리 수.
using EntryCallback = void (*)(const Entry& entry, void* userData);
unsigned long forEachEntry(const void* archive, unsigned long archiveSize, EntryCallback callback, void* userData);

}  // namespace cpio

#endif  // MINICORE_LIBS_LIBCPIO_CPIO_H
