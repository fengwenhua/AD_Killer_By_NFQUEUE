#!/usr/bin/env python3
"""Validate the actual arm64 PIE and every LOAD segment for 4/16 KiB devices."""
import struct
import sys
from pathlib import Path


def validate_android_elf(data: bytes) -> None:
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected a little-endian ELF64 binary")
    elf_type, machine, version = struct.unpack_from("<HHI", data, 16)
    if (elf_type, machine, version) != (3, 183, 1):
        raise ValueError("expected an AArch64 PIE (ET_DYN)")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    ehsize, phsize, phnum = struct.unpack_from("<HHH", data, 52)
    if ehsize != 64 or phsize != 56 or phnum == 0 or phnum == 65535:
        raise ValueError("invalid ELF header or program header table")
    if phoff > len(data) or phnum * phsize > len(data) - phoff:
        raise ValueError("program header table extends beyond the file")
    loads = 0
    interpreters = 0
    for index in range(phnum):
        kind, _, offset, vaddr, _, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + index * phsize)
        if offset > len(data) or filesz > len(data) - offset:
            raise ValueError("segment extends beyond the file")
        if kind == 1:
            loads += 1
            if filesz > memsz:
                raise ValueError("LOAD file size exceeds memory size")
            if align < 16384 or align & (align - 1):
                raise ValueError("LOAD segment is not at least 16 KiB aligned")
            if vaddr % align != offset % align:
                raise ValueError("LOAD virtual address/file offset alignment mismatch")
        elif kind == 3:
            interpreters += 1
            if data[offset:offset + filesz] != b"/system/bin/linker64\0":
                raise ValueError("unexpected Android dynamic linker")
    if not loads or interpreters != 1:
        raise ValueError("missing LOAD segments or unique Android interpreter")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_android_elf.py PATH")
    try:
        validate_android_elf(Path(sys.argv[1]).read_bytes())
    except ValueError as error:
        raise SystemExit(f"Android ELF validation failed: {error}") from error
    print("Android arm64 PIE and 16 KiB ELF alignment validation passed")
