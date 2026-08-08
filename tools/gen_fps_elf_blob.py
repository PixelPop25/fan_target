#!/usr/bin/env python3
"""Embed an ELF as a C byte array. Usage:
  python3 tools/gen_fps_elf_blob.py <elf_path> <out_dir> [name]
  name defaults to fps_elf; use overlay_elf for the ShellUI payload.
"""
import sys
from pathlib import Path

def emit(elf_path: Path, out_dir: Path, name: str) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    h = out_dir / f"{name}_blob.h"
    c = out_dir / f"{name}_blob.c"
    data = b""
    if elf_path.is_file():
        data = elf_path.read_bytes()
        print(f"embedding {elf_path} ({len(data)} bytes) as {name}_blob")
    else:
        print(f"WARNING: {elf_path} not found; writing empty {name}_blob", file=sys.stderr)

    h.write_text(
        f"/* Generated — do not edit. */\n#pragma once\n#include <stddef.h>\n\n"
        f"extern const unsigned char {name}_blob[];\n"
        f"extern const unsigned int {name}_blob_len;\n"
    )
    lines = [f"/* Generated — do not edit. */\n#include \"{name}_blob.h\"\n\n",
             f"const unsigned char {name}_blob[] = {{\n"]
    if data:
        for i in range(0, len(data), 12):
            chunk = data[i:i+12]
            lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    else:
        lines.append("  0x00,\n")
    lines.append("};\n")
    lines.append(f"const unsigned int {name}_blob_len = {len(data)};\n")
    c.write_text("".join(lines))

def main():
    if len(sys.argv) < 3:
        print("usage: gen_fps_elf_blob.py <elf> <out_dir> [name]", file=sys.stderr)
        sys.exit(1)
    name = sys.argv[3] if len(sys.argv) > 3 else "fps_elf"
    emit(Path(sys.argv[1]), Path(sys.argv[2]), name)

if __name__ == "__main__":
    main()
