#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def require_all(text: str, values: tuple[str, ...], label: str) -> None:
    missing = [value for value in values if value not in text]
    assert not missing, f"{label} missing: {', '.join(missing)}"


def main() -> None:
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    require_all(readme, (
        "https://yhyun.asia/", "YH_Hosts", "Vibe Coding", "仅供个人学习",
        "不保证适用于任何生产环境", "不对误拦截",
    ), "README attribution and disclaimer")

    license_text = (ROOT / "LICENSE").read_text(encoding="utf-8")
    require_all(license_text, ("MIT License", "Copyright", "THE SOFTWARE IS PROVIDED"),
                "LICENSE")

    compatibility = (ROOT / "docs/兼容性与协议限制.md").read_text(encoding="utf-8")
    require_all(compatibility, (
        "arm64-v8a", "API 31", "NFQUEUE", "DoH", "DoT", "ECH", "QUIC",
        "IPv4/IPv6", "16 KiB", "2 MiB", "1024 流", "fail-open",
    ), "compatibility documentation")

    device_plan = (ROOT / "docs/真机上线审计与验收方案.md").read_text(
        encoding="utf-8"
    )
    require_all(device_plan, (
        "Linux NFQUEUE UAPI", "Android Bionic UAPI", "CTA_MARK_MASK",
        "PASS/BLOCK connmark", "KernelSU", "APatch", "Magisk",
        "故障注入", "24 小时", "回滚",
    ), "device release plan")

    third_party = (ROOT / "docs/第三方规则与许可.md").read_text(encoding="utf-8")
    require_all(third_party, (
        "AdGuard DNS filter", "AdGuard Mobile", "EasyList", "oisd", "HaGeZi",
        "AdRules", "CJX", "乘风广告过滤规则", "anti-AD", "AWAvenue", "hostsVN",
        "GPL-3.0", "CC BY-SA-3.0", "LGPL-3.0", "MIT", "unknown",
    ), "third-party license inventory")

    documentation = [ROOT / "README.md", ROOT / "frontend/README.md",
                     ROOT / "tests/README.md", *(ROOT / "docs").glob("*.md")]
    for path in documentation:
        text = path.read_text(encoding="utf-8")
        for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", text):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            local = target.split("#", 1)[0]
            assert (path.parent / local).exists(), f"broken documentation link: {path}: {target}"

    print("release contract tests passed")


if __name__ == "__main__":
    main()
