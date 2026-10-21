#!/usr/bin/env python3
"""Independent Python-set oracle for native Pro-minus-Fake, fixtures or real inputs."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("reference", ROOT / "scripts/merge_rules.py")
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)
with tempfile.TemporaryDirectory(prefix="ad_killer_by_nfqueue_hagezi_parity_") as directory:
    base = Path(directory)
    if len(sys.argv) == 4:
        pro, fake = map(Path, sys.argv[2:])
    else:
        pro, fake = base / "pro", base / "fake"
        pro.write_text("# Pro\n*.A.example.com\n*.remove.example.com\na.example.com\n"
                       "0.0.0.0 b.example.com\n*.child.remove.example.com\ninvalid/path\n")
        fake.write_text("*.remove.example.com\nnot-in-pro.example.com\n")
    output = base / "native.txt"
    subprocess.run([sys.argv[1], str(pro), str(fake), str(output)], check=True)
    expected = reference.parse_hosts_text(pro.read_text()) - reference.parse_hosts_text(fake.read_text())
    actual = output.read_text().splitlines()
    assert actual == sorted(expected), "native/Python full sets differ"
    print(f"native HaGeZi full-set Python parity passed: {len(actual)} exact domains")
