#!/usr/bin/env python3
"""tools/gen-musl-alltypes.py — third_party/musl의 tools/mkalltypes.sed를
파이썬으로 재구현한다(docs/design/foundations.md, general-purpose-completion.md
§M26). musl 소스 자체는 무수정으로 유지하되(ADR-022), CMake 빌드
단계에서 `sed`에 의존하는 대신(이 프로젝트의 다른 생성 스텝들 —
tools/mkinitrd.py, tools/gen-efi-entry-addr.py — 이 전부 파이썬을 쓰는
것과 같은 이유, 플랫폼 셸 의존성을 하나로 통일한다) 이 스크립트가
arch/<ARCH>/bits/alltypes.h.in + include/alltypes.h.in을 합쳐
bits/alltypes.h를 만든다. 변환 규칙은 mkalltypes.sed와 정확히 동일해야
한다(줄 단위 TYPEDEF/STRUCT/UNION 매크로화).

사용법: gen-musl-alltypes.py <출력경로> <입력.h.in> [<입력.h.in> ...]
"""
import re
import sys


def transform_line(line):
    m = re.match(r'^TYPEDEF (.*) ([^ ]*);$', line)
    if m:
        ty, name = m.group(1), m.group(2)
        return (f"#if defined(__NEED_{name}) && !defined(__DEFINED_{name})\n"
                f"typedef {ty} {name};\n"
                f"#define __DEFINED_{name}\n"
                f"#endif\n")
    m = re.match(r'^STRUCT *(\S+) (.*);$', line)
    if m:
        name, body = m.group(1), m.group(2)
        return (f"#if defined(__NEED_struct_{name}) && !defined(__DEFINED_struct_{name})\n"
                f"struct {name} {body};\n"
                f"#define __DEFINED_struct_{name}\n"
                f"#endif\n")
    m = re.match(r'^UNION *(\S+) (.*);$', line)
    if m:
        name, body = m.group(1), m.group(2)
        return (f"#if defined(__NEED_union_{name}) && !defined(__DEFINED_union_{name})\n"
                f"union {name} {body};\n"
                f"#define __DEFINED_union_{name}\n"
                f"#endif\n")
    return line + "\n"


def main(argv):
    if len(argv) < 3:
        print(f"사용법: {argv[0]} <출력경로> <입력.h.in> [...]", file=sys.stderr)
        return 1

    out_path = argv[1]
    in_paths = argv[2:]

    with open(out_path, "w", newline="\n") as out:
        for in_path in in_paths:
            with open(in_path, "r") as f:
                for raw_line in f:
                    out.write(transform_line(raw_line.rstrip("\n")))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
