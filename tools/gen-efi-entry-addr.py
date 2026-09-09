#!/usr/bin/env python3
"""tools/gen-efi-entry-addr.py — 커널 ELF에서 EFI 스텁(kernel/arch/
x86_64/efi_stub/)이 알아야 하는 심볼들의 물리주소를 뽑아 헤더로 만든다
(docs/design/boot-and-drivers.md ADR-174).

이 커널은 ASLR/PIE가 전혀 없어(고정 링크 주소, ADR-131 등과 같은
전제) 이 심볼들의 물리주소는 boot.S가 바뀌지 않는 한 빌드마다
결정적이다 — 런타임에 ELF 심볼 테이블을 파싱하는 대신 빌드 시점에
한 번 뽑아 상수로 굽는 쪽이 EFI 스텁 쪽 코드를 훨씬 단순하게 한다.

사용법: gen-efi-entry-addr.py <커널 ELF 경로> <출력 헤더 경로>
"""
import re
import subprocess
import sys

# (심볼 이름, 헤더에 낼 상수 이름) — 전부 boot.S가 내보내는 .global
# 심볼이다.
SYMBOLS = [
    ("_efi_entry", "k_efi_entry_phys_addr"),
    ("efi_acpi_rsdp_phys", "k_efi_acpi_rsdp_phys_addr"),
]


def find_nm() -> str:
    for candidate in ("llvm-nm", "nm"):
        try:
            subprocess.run([candidate, "--version"], capture_output=True, check=True)
            return candidate
        except (OSError, subprocess.CalledProcessError):
            continue
    print("llvm-nm(또는 nm)을 찾을 수 없다", file=sys.stderr)
    sys.exit(1)


def main(argv):
    if len(argv) != 3:
        print(f"사용법: {argv[0]} <커널 ELF 경로> <출력 헤더 경로>", file=sys.stderr)
        return 1

    kernel_elf, out_header = argv[1], argv[2]
    nm = find_nm()
    result = subprocess.run([nm, kernel_elf], capture_output=True, text=True, check=True)

    addrs = {}
    for line in result.stdout.splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+\S+\s+(\S+)$", line.strip())
        if m:
            addrs[m.group(2)] = int(m.group(1), 16)

    lines = [
        "// gen-efi-entry-addr.py가 생성했다 — 손으로 고치지 않는다.\n",
        "#pragma once\n",
        "#include <cstdint>\n",
    ]
    for symbol, const_name in SYMBOLS:
        if symbol not in addrs:
            print(f"'{kernel_elf}'에서 '{symbol}' 심볼을 찾지 못했다", file=sys.stderr)
            return 1
        lines.append(f"constexpr uint64_t {const_name} = 0x{addrs[symbol]:x}ull;\n")

    with open(out_header, "w", encoding="utf-8") as f:
        f.writelines(lines)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
