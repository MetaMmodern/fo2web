#!/usr/bin/env python3
"""Write HIGHLOW relocation-slot virtual addresses from a conventional PE32 file.

NFSU2's copy had a zeroed relocation directory and needs its custom recovery
tool. FlatOut 2 has a normal PE base-relocation directory, so this deliberately
small extractor is the appropriate input producer for the shared lifter.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pefile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("--out", type=Path, default=Path("out/relocs.txt"))
    args = parser.parse_args()

    pe = pefile.PE(str(args.exe))
    if pe.FILE_HEADER.Machine != 0x14C:
        raise SystemExit("expected a 32-bit x86 PE")
    if not hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        raise SystemExit("the executable has no parsed base-relocation directory")

    kinds = {
        entry.type
        for block in pe.DIRECTORY_ENTRY_BASERELOC
        for entry in block.entries
    }
    unsupported = kinds - {0, 3}  # ABSOLUTE padding; HIGHLOW 32-bit relocation
    if unsupported:
        raise SystemExit(f"unsupported relocation types: {sorted(unsupported)}")

    image_base = pe.OPTIONAL_HEADER.ImageBase
    slots = sorted(
        image_base + entry.rva
        for block in pe.DIRECTORY_ENTRY_BASERELOC
        for entry in block.entries
        if entry.type == 3
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("".join(f"{address:08x}\n" for address in slots))
    print(
        f"{args.exe}: image base {image_base:#x}; "
        f"{len(slots)} HIGHLOW relocation slots -> {args.out}"
    )


if __name__ == "__main__":
    main()
