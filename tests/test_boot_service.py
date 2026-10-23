#!/usr/bin/env python3
"""Execute the real boot script with mock properties and no Android/root writes."""
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run_case(config, boot="1", changed_config=None, expect_start=False,
             expect_sleeps=0, prepare_fails=False):
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
            "AD_Killer_By_NFQUEUE": '#!/bin/sh\nprintf "%s\\n" "$*" >> "$BOOT_TEST_DIR/calls"\n'
                'if [ "$1" = --prepare-reboot ] && [ "$BOOT_TEST_PREPARE_FAILS" = 1 ]; then exit 1; fi\n',
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
                   BOOT_TEST_PREPARE_FAILS='1' if prepare_fails else '0',
                   PATH=str(base) + os.pathsep + os.environ.get("PATH", ""))
        result = subprocess.run(["sh", str(base / "service.sh")], env=env,
                                timeout=10, capture_output=True)
        assert result.returncode == (1 if prepare_fails else 0), result.stderr
        calls = (base / "calls").read_text().splitlines() if (base / "calls").exists() else []
        assert calls == ['--prepare-reboot'] + (["--start-boot"] if expect_start else []), calls
        sleeps = (base / "sleeps").read_text().splitlines() if (base / "sleeps").exists() else []
        assert len(sleeps) == expect_sleeps, len(sleeps)
        if changed_config is None:
            assert (conf.read_text() if conf.exists() else None) == config, 'boot changed persisted policy'


def main():
    for config in (None, "", "enabled=true\n", "autostart=false\nenabled=true\n",
                   "autostart=garbage\nenabled=true\n", "autostart=true\n",
                   "autostart=true\nenabled=false\n", "autostart=1\nenabled=0\n",
                   "autostart=true\nautostart=false\nenabled=true\n",
                   "autostart=true\nenabled=garbage\n",
                   "autostart=true\nenabled=true\nmanual_stop=true\n",
                   "autostart=true\nenabled=true\nmanual_stop=1\n",
                   "autostart=true\nenabled=true\nmanual_stop=garbage\n",
                   "autostart=true\nenabled=true\nmanual_stop=false\nmanual_stop=true\n"):
        run_case(config)
    run_case("autostart=true\nenabled=true\n", expect_start=True)
    run_case("autostart=1\nenabled=1\n", expect_start=True)
    run_case("autostart=true\nenabled=true\nmanual_stop=false\n", expect_start=True)
    for prop in ("0", ""):
        run_case("autostart=true\nenabled=true\n", boot=prop, expect_sleeps=180)
    for changed in ("autostart=false\nenabled=true\n",
                    "autostart=true\nenabled=false\n",
                    "autostart=true\nenabled=true\nmanual_stop=true\n"):
        run_case("autostart=true\nenabled=true\n", changed_config=changed,
                 expect_sleeps=1)
    run_case("autostart=true\nenabled=true\n",
             changed_config="autostart=true\nenabled=true\n",
             expect_sleeps=1, expect_start=True)
    run_case("autostart=true\nenabled=true\n", prepare_fails=True)
    run_case("autostart=false\nenabled=true\n", prepare_fails=True)
    print("boot service default-off, pause, timeout and recheck tests passed")


if __name__ == "__main__":
    main()
