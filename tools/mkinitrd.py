#!/usr/bin/env python3
"""tools/mkinitrd.py — initrun 등을 MCPACK v1 initrd 이미지로 패키징한다
(docs/spec/boot.md §5). 서드파티 tar/cpio 대신 이 최소 자체 포맷을 쓴다
(ADR-006) — 호스트 빌드 도구라 커널 자체(freestanding C++)와 달리
파이썬으로 작성해도 ADR-010 제약(예외/RTTI 금지 등)의 대상이 아니다.

사용법:
    mkinitrd.py <출력경로> [--disk-cfg=BUS,DEVICE,FUNCTION,FS_TAG] <name1>=<path1> [...]

각 <path>의 파일 내용을 그대로 담고, mcpack_header/mcpack_entry(§5)
그대로의 바이너리 레이아웃으로 출력한다.

--disk-cfg는 "disk.cfg" 엔트리를 추가한다(docs/spec/boot.md §3의
boot::boot_device_descriptor 그대로의 바이트, ADR-131/146) — 실제
OS 설치 절차가 아직 없는 이 프로젝트에서, 그 절차가 채울 값을 이
개발 도구가 대신 고정으로 써 넣는다. FS_TAG는 지금 0(cpio/newc)
하나뿐이다.
"""
import struct
import sys

MCPACK_MAGIC = 0x4D43504B  # "MCPK"
MCPACK_VERSION = 1
NAME_LEN = 60

# struct boot::boot_device_descriptor { uint32 valid, pci_bus, pci_device,
# pci_function, fs_tag; } — 전부 uint32_t라 정렬 패딩이 없다.
DISK_CFG_FMT = "<5I"

# struct mcpack_header { uint32 magic; uint32 version; uint32 entry_count; uint32 _pad; };
HEADER_FMT = "<IIII"
# struct mcpack_entry { char name[60]; uint64 offset; uint64 size; };
# name[60] 뒤에 4바이트 패딩이 필요하다 — uint64_t 필드는 8바이트
# 정렬을 요구하는데(x86_64 System V ABI) 60은 8의 배수가 아니라서,
# 커널이 이 파일을 읽을 때 쓰는 평범한(패킹하지 않은) C++ 구조체가
# 자연스럽게 이 위치에 패딩을 둔다 — 여기서 "4x"로 그 패딩을 미리
# 맞춰 주지 않으면 offset/size 필드가 4바이트씩 밀려 읽힌다.
ENTRY_FMT = "<60s4xQQ"


def main(argv):
    if len(argv) < 3:
        print(f"사용법: {argv[0]} <출력경로> <name1>=<path1> [...]", file=sys.stderr)
        return 1

    out_path = argv[1]
    specs = []
    for arg in argv[2:]:
        if arg.startswith("--disk-cfg="):
            parts = arg[len("--disk-cfg="):].split(",")
            if len(parts) != 4:
                print("--disk-cfg는 BUS,DEVICE,FUNCTION,FS_TAG 4개 값이 필요하다",
                      file=sys.stderr)
                return 1
            bus, device, function, fs_tag = (int(p, 0) for p in parts)
            disk_cfg = struct.pack(DISK_CFG_FMT, 1, bus, device, function, fs_tag)
            specs.append((b"disk.cfg", disk_cfg))
            continue
        if "=" not in arg:
            print(f"잘못된 인자(형식은 name=path): {arg}", file=sys.stderr)
            return 1
        name, path = arg.split("=", 1)
        name_bytes = name.encode("utf-8")
        if len(name_bytes) >= NAME_LEN:
            print(f"이름이 너무 길다({NAME_LEN}바이트 미만이어야 함): {name}", file=sys.stderr)
            return 1
        with open(path, "rb") as f:
            data = f.read()
        specs.append((name_bytes, data))

    header = struct.pack(HEADER_FMT, MCPACK_MAGIC, MCPACK_VERSION, len(specs), 0)
    entries_size = len(specs) * struct.calcsize(ENTRY_FMT)
    data_offset = struct.calcsize(HEADER_FMT) + entries_size

    entries = b""
    payload = b""
    offset = data_offset
    for name_bytes, data in specs:
        entries += struct.pack(ENTRY_FMT, name_bytes, offset, len(data))
        payload += data
        offset += len(data)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(entries)
        f.write(payload)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
