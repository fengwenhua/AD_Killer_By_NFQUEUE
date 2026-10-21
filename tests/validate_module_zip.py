#!/usr/bin/env python3
import sys
import zipfile
from pathlib import PurePosixPath
from validate_android_elf import validate_android_elf


REQUIRED = {
    "AD_Killer_By_NFQUEUE",
    "module.prop",
    "service.sh",
    "customize.sh",
    "uninstall.sh",
    "webroot/index.html",
    "LICENSE",
}


def fail(message: str) -> None:
    raise SystemExit(f"ZIP validation failed: {message}")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: validate_module_zip.py PATH")
    with zipfile.ZipFile(sys.argv[1]) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            fail("duplicate entries")
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or "\\" in name:
                fail(f"unsafe path: {name}")
        files = {name for name in names if not name.endswith("/")}
        if files != REQUIRED:
            fail(f"unexpected layout: {sorted(files)}")
        try:
            validate_android_elf(archive.read("AD_Killer_By_NFQUEUE"))
        except ValueError as error:
            fail(str(error))
        if b"MIT License" not in archive.read("LICENSE"):
            fail("missing project MIT license text")
        for script in ("service.sh", "customize.sh", "uninstall.sh"):
            if not archive.read(script).startswith(b"#!"):
                fail(f"{script} has no shebang")
        prop = archive.read("module.prop").decode("utf-8")
        for key in ("id=", "name=", "version=", "versionCode="):
            if not any(line.startswith(key) for line in prop.splitlines()):
                fail(f"module.prop missing {key[:-1]}")
        properties = dict(line.split("=", 1) for line in prop.splitlines()
                          if "=" in line)
        binary_version = properties["version"].removeprefix("v").encode("ascii")
        if binary_version not in archive.read("AD_Killer_By_NFQUEUE"):
            fail("binary version does not match module.prop")
    print("module ZIP validation passed")


if __name__ == "__main__":
    main()
