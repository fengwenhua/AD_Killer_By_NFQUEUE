#!/usr/bin/env python3
import hashlib
import os
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from package_module import FILES, create_module_zip, write_checksum  # noqa: E402
from verify_checksum import verify_checksum  # noqa: E402
from validate_android_elf import validate_android_elf  # noqa: E402
from release_metadata import generate  # noqa: E402


def test_android_elf() -> None:
    data = bytearray(256)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHI", data, 16, 3, 183, 1)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HHH", data, 52, 64, 56, 2)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, 256, 256, 16384)
    interpreter = b"/system/bin/linker64\0"
    struct.pack_into("<IIQQQQQQ", data, 120, 3, 4, 200, 200, 0,
                     len(interpreter), len(interpreter), 1)
    data[200:200 + len(interpreter)] = interpreter
    validate_android_elf(data)
    for label, offset, fmt, value in (
        ("4 KiB-only LOAD", 112, "<Q", 4096),
        ("misaligned LOAD", 80, "<Q", 1),
        ("wrong ABI", 18, "<H", 62),
        ("non-PIE executable", 16, "<H", 2),
        ("out-of-bounds program headers", 32, "<Q", 10000),
        ("LOAD beyond file", 96, "<Q", 10000),
    ):
        invalid = bytearray(data)
        struct.pack_into(fmt, invalid, offset, value)
        try:
            validate_android_elf(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"must reject {label}")


def main() -> None:
    test_android_elf()
    module_prop = (ROOT / "module/module.prop").read_text(encoding="utf-8")
    properties = dict(
        line.split("=", 1) for line in module_prop.splitlines() if "=" in line
    )
    version = properties["version"].removeprefix("v")
    assert version.count(".") in (1, 2)
    assert properties["versionCode"].isdigit()

    common = (ROOT / "src/common.h").read_text(encoding="utf-8")
    build = (ROOT / "build.sh").read_text(encoding="utf-8")
    assert f'"{version}"' not in common, "C++ header must not duplicate module version"
    assert "-DMODULE_VERSION=" in build and "-DMODULE_VERSION_CODE=" in build

    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        package = base / "module"
        package.mkdir()
        for name, _ in FILES:
            path = package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if name == "AD_Killer_By_NFQUEUE":
                content = b"\x7fELF" + version.encode("ascii")
            elif name == "module.prop":
                content = module_prop.encode("utf-8")
            elif name.endswith(".sh"):
                content = b"#!/system/bin/sh\nexit 0\n"
            elif name == "LICENSE":
                content = (ROOT / "LICENSE").read_bytes()
            else:
                content = b"<!doctype html>\n"
            path.write_bytes(content)

        first = base / "first.zip"
        second = base / "second.zip"
        create_module_zip(package, first, 315532800)
        release_zip = base / f"AD_Killer_By_NFQUEUE-v{version}.zip"
        create_module_zip(package, release_zip, 315532800)
        write_checksum(release_zip)
        notes = base / "notes.md"
        notes.write_text("# Release notes\n")
        metadata = generate(release_zip, notes)
        assert metadata["versionCode"] == int(properties["versionCode"])
        assert metadata["zipUrl"].endswith(release_zip.name)
        assert metadata["sha256"] == hashlib.sha256(release_zip.read_bytes()).hexdigest()
        assert (base / "changelog.md").read_text() == notes.read_text()
        release_zip.with_name(release_zip.name+".sha256").write_text("bad checksum")
        try:
            generate(release_zip, notes)
        except ValueError:
            pass
        else:
            raise AssertionError("metadata must reject package/checksum mismatch")
        for name, _ in FILES:
            os.utime(package / name, (1900000000, 1900000000))
        create_module_zip(package, second, 315532800)
        assert hashlib.sha256(first.read_bytes()).digest() == \
               hashlib.sha256(second.read_bytes()).digest(), \
               "package must ignore source mtimes"
        with zipfile.ZipFile(first) as archive:
            assert archive.namelist() == [name for name, _ in FILES]
            for name, mode in FILES:
                stored_mode = (archive.getinfo(name).external_attr >> 16) & 0o777
                assert stored_mode == mode

        sidecar = write_checksum(first)
        fields = sidecar.read_text(encoding="ascii").strip().split()
        assert fields == [hashlib.sha256(first.read_bytes()).hexdigest(), first.name]
        verify_checksum(first)
        first.write_bytes(first.read_bytes() + b"tampered")
        try:
            verify_checksum(first)
        except ValueError as error:
            assert str(error) == "SHA-256 mismatch"
        else:
            raise AssertionError("tampered package checksum must fail")

    print("package reproducibility tests passed")


if __name__ == "__main__":
    main()
