#!/usr/bin/env python3
"""tools/mkbootdisk.py — initrun이 virtio-blk로 마운트하는 부트 디스크
이미지를 cpio(newc) 아카이브로 만든다(docs/plan/system-servers-bringup.md
§M12, ADR-131). tools/mkinitrd.py가 만드는 MCPACK initrd(커널이 자신에
심는 것)와는 완전히 별개다 — 이건 virtio-blk 장치로 붙는 진짜 디스크
이미지이고, initrun 자신의 임베디드 cpio 리더(init/initrun/cpio_reader.cpp)
가 읽는 포맷을 그대로 따라야 한다.

사용법:
    mkbootdisk.py <출력경로> --service=<이름>=<elf경로> [...]

각 --service마다 두 엔트리를 담는다:
    bin/<이름>              — ELF 바이너리 그대로
    lib/NNN-<이름>.ini      — [<이름>]\\nexec=bin/<이름>\\n
(NNN은 나열 순서대로 000, 001, ... — ADR-131 §결정5: "파일명순=실행순"
이므로 --service 인자 순서가 곧 기동 순서다.)

cpio(newc) 헤더 레이아웃은 init/initrun/main.cpp의 write_cpio_entry()
(자체 self-test용으로 손으로 쓴 것과 동일)와 필드 하나까지 맞춘다 —
서드파티 cpio/tar 라이브러리 대신 이 최소 자체 구현을 쓴다(ADR-006).
"""
import struct
import sys

CPIO_MAGIC = b"070701"
NUM_FIELDS = 13
FILESIZE_FIELD_INDEX = 6
NAMESIZE_FIELD_INDEX = 11


def _hex8(v: int) -> bytes:
    return f"{v:08x}".encode("ascii")


def _align4(n: int) -> int:
    return (n + 3) & ~3


def _cpio_entry(name: bytes, data: bytes) -> bytes:
    name_with_nul = name + b"\x00"
    fields = [0] * NUM_FIELDS
    fields[FILESIZE_FIELD_INDEX] = len(data)
    fields[NAMESIZE_FIELD_INDEX] = len(name_with_nul)

    out = bytearray()
    out += CPIO_MAGIC
    for f in fields:
        out += _hex8(f)
    out += name_with_nul
    while len(out) % 4 != 0:
        out += b"\x00"
    out += data
    while len(out) % 4 != 0:
        out += b"\x00"
    return bytes(out)


def _trailer_entry() -> bytes:
    return _cpio_entry(b"TRAILER!!!", b"")


def main(argv):
    if len(argv) < 3:
        print(f"사용법: {argv[0]} <출력경로> --service=<이름>=<elf경로> [...]",
              file=sys.stderr)
        return 1

    out_path = argv[1]
    services = []
    for arg in argv[2:]:
        if not arg.startswith("--service="):
            print(f"알 수 없는 인자: {arg}", file=sys.stderr)
            return 1
        spec = arg[len("--service="):]
        if "=" not in spec:
            print(f"잘못된 --service 형식(이름=경로여야 함): {spec}", file=sys.stderr)
            return 1
        name, path = spec.split("=", 1)
        services.append((name, path))

    chunks = []
    for index, (name, path) in enumerate(services):
        with open(path, "rb") as f:
            elf_data = f.read()
        chunks.append(_cpio_entry(f"bin/{name}".encode("utf-8"), elf_data))

        ini_text = f"[{name}]\nexec=bin/{name}\n".encode("utf-8")
        ini_name = f"lib/{index:03d}-{name}.ini".encode("utf-8")
        chunks.append(_cpio_entry(ini_name, ini_text))

    chunks.append(_trailer_entry())

    with open(out_path, "wb") as f:
        for chunk in chunks:
            f.write(chunk)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
