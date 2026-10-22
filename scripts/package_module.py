#!/usr/bin/env python3
"""Create a canonical module ZIP with stable order, timestamps and modes."""

from __future__ import annotations

import hashlib
import os
import sys
import time
import zipfile
from pathlib import Path


FILES = (
    ("AD_Killer_By_NFQUEUE", 0o755),
    ("module.prop", 0o644),
    ("service.sh", 0o755),
    ("emulated-soft-reboot.sh", 0o755),
    ("customize.sh", 0o755),
    ("uninstall.sh", 0o755),
    ("webroot/index.html", 0o644),
    ("LICENSE", 0o644),
)


def zip_timestamp(epoch: int) -> tuple[int, int, int, int, int, int]:
    epoch = min(max(epoch, 315532800), 4354819198)
    value = time.gmtime(epoch)
    return (value.tm_year, value.tm_mon, value.tm_mday,
            value.tm_hour, value.tm_min, value.tm_sec - value.tm_sec % 2)


def create_module_zip(package_dir: Path, output_path: Path, epoch: int) -> None:
    package_dir = package_dir.resolve()
    output_path = output_path.resolve()
    missing = [name for name, _ in FILES if not (package_dir / name).is_file()]
    if missing:
        raise ValueError(f"missing module files: {', '.join(missing)}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=9, strict_timestamps=True) as archive:
            for name, mode in FILES:
                info = zipfile.ZipInfo(name, zip_timestamp(epoch))
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = ((0o100000 | mode) << 16)
                archive.writestr(info, (package_dir / name).read_bytes(),
                                 compress_type=zipfile.ZIP_DEFLATED,
                                 compresslevel=9)
        os.replace(temporary, output_path)
    finally:
        temporary.unlink(missing_ok=True)


def write_checksum(path: Path) -> Path:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    checksum = path.with_name(path.name + ".sha256")
    temporary = checksum.with_name(checksum.name + ".tmp")
    temporary.write_text(f"{digest}  {path.name}\n", encoding="ascii")
    os.replace(temporary, checksum)
    return checksum


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: package_module.py PACKAGE_DIR OUTPUT_ZIP")
    try:
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "315532800"))
    except ValueError as error:
        raise SystemExit("SOURCE_DATE_EPOCH must be an integer") from error
    create_module_zip(Path(sys.argv[1]), Path(sys.argv[2]), epoch)
    write_checksum(Path(sys.argv[2]).resolve())


if __name__ == "__main__":
    main()
