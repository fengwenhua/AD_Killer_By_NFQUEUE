#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require_all(text: str, values: tuple[str, ...], label: str) -> None:
    missing = [value for value in values if value not in text]
    assert not missing, f"{label} missing: {', '.join(missing)}"


def main() -> None:
    main_source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    require_all(main_source, (
        'write_conf_value(FILE_STATE_CONF, "enabled", "true")',
        'write_conf_value(FILE_STATE_CONF, "manual_stop", "false")',
        'write_conf_value(FILE_STATE_CONF, "enabled", "false")',
        'write_conf_value(FILE_STATE_CONF, "manual_stop", "true")',
        'error_storage',
    ), "CLI lifecycle")

    service = (ROOT / "module/service.sh").read_text(encoding="utf-8")
    disabled_check = service.index("should_autostart || exit 0")
    start = service.index('"$MODDIR/AD_Killer_By_NFQUEUE" --start')
    assert disabled_check < start, "boot service must honor persisted pause before start"
    assert '"$i" -ge 180 ] && exit 0' in service, "incomplete boot must skip startup"
    assert '"autostart=false\\n"' in main_source, "installation must default to no autostart"
    assert '"enabled=false\\n"' in main_source, "first installation must be paused"
    assert "--autostart" in main_source
    manual_start = main_source.split("static int cmd_start()", 1)[1].split(
        "static int cmd_daemon()", 1
    )[0]
    assert 'write_conf_value(FILE_STATE_CONF, "autostart"' not in manual_start, (
        "manual start must not opt in to boot startup"
    )

    uninstall = (ROOT / "module/uninstall.sh").read_text(encoding="utf-8")
    assert "delete_all()" in uninstall
    assert "ADK_NFQ_ENTRY" in uninstall
    assert 'delete_all "$tb" -w 5 -t mangle -D OUTPUT -j "$ch"' in uninstall
    for source in (uninstall, (ROOT / "src/iptables.cpp").read_text(encoding="utf-8")):
        for line in source.splitlines():
            if "-D OUTPUT" in line:
                assert "--queue-num" not in line and "-j DROP" not in line, (
                    "cleanup must not remove anonymous rules owned by other modules"
                )

    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    require_all(workflow, ("shellcheck", "bash -n build.sh", "timeout-minutes"),
                "CI shell validation")

    print("module lifecycle contract tests passed")


if __name__ == "__main__":
    main()
