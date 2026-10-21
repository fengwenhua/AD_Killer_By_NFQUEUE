#!/usr/bin/env python3
import hashlib
import sys
from pathlib import Path


def verify_checksum(archive: Path) -> None:
    sidecar = archive.with_name(archive.name + ".sha256")
    fields = sidecar.read_text(encoding="ascii").strip().split()
    if len(fields) != 2 or fields[1] != archive.name:
        raise ValueError("invalid checksum sidecar format")
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if fields[0].lower() != actual:
        raise ValueError("SHA-256 mismatch")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_checksum.py PATH.zip")
    try:
        verify_checksum(Path(sys.argv[1]))
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
    print("release checksum verified")


if __name__ == "__main__":
    main()
