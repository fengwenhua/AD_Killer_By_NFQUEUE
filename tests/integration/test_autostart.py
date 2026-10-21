#!/usr/bin/env python3
"""CLI-only checks in a disposable Linux container; never starts the engine."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


DATA = Path("/data/adb/AD_Killer_By_NFQUEUE")
BINARY = sys.argv[1]


def command(*args):
    return subprocess.check_output([BINARY, *args], text=True)


def command_json(*args):
    # CLI commands may print timestamped log lines before their final JSON
    # response, just as --status-fast and the WebUI's lastJson parser expect.
    return json.loads(command(*args).splitlines()[-1])


def status():
    return command_json("--status-fast")


def main():
    assert sys.platform == "linux" and os.geteuid() == 0
    assert not DATA.exists(), "refusing existing module data"
    DATA.parent.mkdir(parents=True, exist_ok=True)
    try:
        command("--install")
        conf = DATA / "state.conf"
        first = status()
        assert not first["autostart"] and not first["enabled"] and first["manual_stop"]
        assert first["block_quic"], "fresh installation must block QUIC by default"
        for argument, expected in (("off", False), ("on", True), ("off", False)):
            assert command_json("--quic", argument)["block_quic"] is expected
            saved = conf.read_text()
            command("--install")
            assert conf.read_text() == saved, "reinstall overwrote user QUIC policy"
            snapshot = status()
            assert snapshot["block_quic"] is expected and not snapshot["running"]
            assert not snapshot["enabled"] and snapshot["manual_stop"]
            assert not (DATA / "runtime.state").exists()
            assert not (DATA / "daemon.pid").exists()
        for argument, expected in (("on", True), ("1", True), ("off", False), ("0", False)):
            result = command_json("--autostart", argument)
            assert result["autostart"] is expected
            snapshot = status()
            assert snapshot["autostart"] is expected and not snapshot["running"]
            assert not snapshot["enabled"] and snapshot["manual_stop"]
            assert not (DATA / "runtime.state").exists()
            assert not (DATA / "daemon.pid").exists()
        command("--autostart", "on")
        saved = conf.read_text()
        command("--install")
        assert conf.read_text() == saved, "reinstall overwrote user boot policy"
        for args in (("--autostart", "invalid"), ("--autostart",)):
            result = subprocess.run([BINARY, *args], capture_output=True)
            assert result.returncode != 0 and conf.read_text() == saved
        # An old installation has no new key: it must not opt in on upgrade.
        conf.write_text("\n".join(line for line in saved.splitlines()
                                  if not line.startswith("autostart=")) + "\n")
        command("--install")
        assert not status()["autostart"]
        conf.write_text(conf.read_text() + "autostart=garbage\n")
        assert not status()["autostart"]
        print("autostart/QUIC defaults, persistence, old-config and no-engine-side-effect tests passed")
    finally:
        if DATA.exists():
            shutil.rmtree(DATA)


if __name__ == "__main__":
    main()
