#!/usr/bin/env python3
"""tools/gen-musl-syscall-bits.py — third_party/musl의 Makefile 규칙
(obj/include/bits/syscall.h)을 파이썬으로 재구현한다
(real-libc-syscall-layer.md §M28, tools/gen-musl-alltypes.py와 같은
이유 — sed 대신 파이썬으로 이 프로젝트의 생성 스텝 셸 의존성을
통일한다). 원본 규칙(third_party/musl/Makefile):

    cp arch/$(ARCH)/bits/syscall.h.in $@
    sed -n -e s/__NR_/SYS_/p < arch/$(ARCH)/bits/syscall.h.in >> $@

즉: 입력 파일 내용을 그대로 복사한 뒤, `__NR_`를 `SYS_`로 바꾼 줄만
다시 한 번 뒤에 이어 붙인다(`__NR_write 1` 줄이 있으면 그 줄을 그대로
싣고, 추가로 `SYS_write 1`도 싣는다 — `#define __NR_write 1`처럼 앞에
다른 텍스트가 있는 줄은 그 줄 전체에서 `__NR_`만 `SYS_`로 바뀐 채로
다시 실린다, sed `-n -e s/.../.../p`의 "치환 성공한 줄만 출력" 동작
그대로).

사용법: gen-musl-syscall-bits.py <출력경로> <입력 syscall.h.in>
"""
import sys


def main(argv):
    if len(argv) != 3:
        print(f"사용법: {argv[0]} <출력경로> <입력 syscall.h.in>", file=sys.stderr)
        return 1

    out_path, in_path = argv[1], argv[2]

    with open(in_path, "r") as f:
        lines = f.readlines()

    appended = []
    for line in lines:
        if "__NR_" in line:
            appended.append(line.replace("__NR_", "SYS_"))

    with open(out_path, "w", newline="\n") as out:
        out.writelines(lines)
        out.writelines(appended)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
