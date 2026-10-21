#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
common = (ROOT / "src/common.h").read_text()
assert '#define DATA_DIR              "/data/adb/AD_Killer_By_NFQUEUE"' in common
assert "DATA_DIR_ROOT" not in common
main = (ROOT / "src/main.cpp").read_text()
assert 'ensure_dir("/data/adb"' not in main, "must not chmod the shared root-manager directory"
print("runtime data path isolation tests passed")
