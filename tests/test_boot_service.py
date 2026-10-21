#!/usr/bin/env python3
"""Execute the real boot script with mock properties and no Android/root writes."""
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run_case(config, boot="1", changed_config=None, expect_start=False,
             expect_sleeps=0):
    with tempfile.TemporaryDirectory(prefix="ad_killer_by_nfqueue_boot_") as directory:
        base = Path(directory)
        conf = base / "state.conf"
        if config is not None:
            conf.write_text(config)
        # Only redirect the data file; all control flow is the shipped script.
        source = (ROOT / "module/service.sh").read_text()
        source = source.replace("STATE_CONF=/data/adb/AD_Killer_By_NFQUEUE/state.conf",
                                'STATE_CONF="$BOOT_TEST_DIR/state.conf"')
        (base / "service.sh").write_text(source)
        scripts = {
            "AD_Killer_By_NFQUEUE": '#!/bin/sh\nprintf "%s\\n" "$*" >> "$BOOT_TEST_DIR/calls"\n',
            "getprop": '#!/bin/sh\nprintf "%s\\n" "$BOOT_TEST_PROPERTY"\n',
            "sleep": '#!/bin/sh\necho sleep >> "$BOOT_TEST_DIR/sleeps"\n'
                     'if [ -f "$BOOT_TEST_DIR/change" ]; then\n'
                     '  cp "$BOOT_TEST_DIR/change" "$BOOT_TEST_DIR/state.conf"\n'
                     '  echo 1 > "$BOOT_TEST_DIR/completed"\nfi\n',
        }
        if changed_config is not None:
            (base / "change").write_text(changed_config)
            scripts["getprop"] = '#!/bin/sh\nif [ -f "$BOOT_TEST_DIR/completed" ]; then\n'
            scripts["getprop"] += ' echo 1\nelse\n echo 0\nfi\n'
        for name, text in scripts.items():
            path = base / name
            path.write_text(text)
            path.chmod(0o755)
        env = dict(os.environ, BOOT_TEST_DIR=str(base), BOOT_TEST_PROPERTY=boot,
                   PATH=str(base) + os.pathsep + os.environ.get("PATH", ""))
        subprocess.run(["sh", str(base / "service.sh")], env=env, check=True,
                       timeout=10, capture_output=True)
        calls = (base / "calls").read_text().splitlines() if (base / "calls").exists() else []
        assert calls == (["--start"] if expect_start else []), calls
        sleeps = (base / "sleeps").read_text().splitlines() if (base / "sleeps").exists() else []
        assert len(sleeps) == expect_sleeps, len(sleeps)


def main():
    for config in (None, "", "enabled=true\n", "autostart=false\nenabled=true\n",
                   "autostart=garbage\nenabled=true\n", "autostart=true\n",
                   "autostart=true\nenabled=false\n", "autostart=1\nenabled=0\n",
                   "autostart=true\nautostart=false\nenabled=true\n",
                   "autostart=true\nenabled=garbage\n"):
        run_case(config)
    run_case("autostart=true\nenabled=true\n", expect_start=True)
    run_case("autostart=1\nenabled=1\n", expect_start=True)
    for prop in ("0", ""):
        run_case("autostart=true\nenabled=true\n", boot=prop, expect_sleeps=180)
    for changed in ("autostart=false\nenabled=true\n",
                    "autostart=true\nenabled=false\n"):
        run_case("autostart=true\nenabled=true\n", changed_config=changed,
                 expect_sleeps=1)
    run_case("autostart=true\nenabled=true\n",
             changed_config="autostart=true\nenabled=true\n",
             expect_sleeps=1, expect_start=True)
    print("boot service default-off, pause, timeout and recheck tests passed")


if __name__ == "__main__":
    main()
