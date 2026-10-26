#!/usr/bin/env python3
from pathlib import Path
import json
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

    workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    assert 'contents: write' not in workflow and 'gh release create' not in workflow, (
        'ordinary CI must only build/test; it must never consume a release tag'
    )
    publication = (ROOT / '.github/workflows/release.yml').read_text()
    require_all(publication, (
        'workflow_dispatch:', "github.ref == 'refs/heads/main'", 'contents: write',
        'actions: read', 'ci_run_id:', 'KERNELSU_IMMUTABLE_RELEASES',
        'scripts/publish_release.py prepare', 'scripts/publish_release.py publish',
        'cancel-in-progress: false', 'persist-credentials: false',
        'options: [publish, inspect, resume]', 'confirm_manifest:',
        'scripts/publish_release.py inspect', 'scripts/publish_release.py resume',
        'release/release-state.json', 'if: always()',
    ), 'explicit gated immutable publication')
    assert '\n  push:' not in publication, 'a push must not publish automatically'
    require_all(workflow, (
        "RELEASE_CHANNEL=preview", "tests/test_release_channels.py",
        "tests/test_webui_update_channels.py", "name: AD_Killer_By_NFQUEUE-module-preview",
    ), "separate stable/preview build gates")
    assert workflow.count("name: AD_Killer_By_NFQUEUE-module-preview") == 1, (
        "CI must retain the marked preview artifact without publishing it"
    )
    assert workflow.count('KernelSU-Modules/tools/actions/module-release@b6d340ddcd562ad482568b6e66b8d7a9c34d645e') == 2
    assert workflow.count("dry-run: 'true'") == 2
    assert 'module.yaml' in (ROOT / 'scripts/validate_listing.py').read_text()
    require_all(workflow, (
        "tests/validate_manager_update.py dist/AD_Killer_By_NFQUEUE-v*.zip",
        "tests/validate_manager_update.py dist/preview/AD_Killer_By_NFQUEUE-v*.zip",
    ), "native manager update gates for both channels")
    # Reviewed full SHAs bind the action runtime; setup-node's node-version
    # controls only the project build. Digest PRs require updating this record
    # after reviewing the new immutable action.yml, not relaxing the check.
    lock = json.loads((ROOT / '.github/actions.lock.json').read_text())
    assert lock['schema'] == 1
    all_workflows = '\n'.join(path.read_text() for path in (ROOT / '.github/workflows').glob('*.yml'))
    for action, minimum in (
        ("checkout", 6), ("setup-node", 6), ("setup-python", 6),
        ("upload-artifact", 6),
    ):
        refs = re.findall(rf"uses:\s*actions/{action}@([^\s#]+)", all_workflows)
        assert refs, f"workflow must exercise actions/{action}"
        reviewed = lock['actions']['actions/' + action]
        version = re.fullmatch(r'v(\d+)(?:\.\d+)*', reviewed['reviewed_ref'])
        assert version and int(version[1]) >= minimum and reviewed['runtime'] == 'node24'
        assert re.fullmatch(r'[0-9a-f]{64}', reviewed['action_yaml_sha256'])
        for ref in refs:
            assert re.fullmatch(r'[0-9a-f]{40}',ref) and ref == reviewed['sha'], f'actions/{action} must match the reviewed full SHA'
    external = re.findall(r'uses:\s*([^\s@]+)@([^\s#]+)',all_workflows)
    for action, ref in external:
        assert re.fullmatch(r'[0-9a-f]{40}',ref), f'external action must use a full SHA: {action}'
        assert action in lock['actions'] or action == 'KernelSU-Modules/tools/actions/module-release', f'unreviewed action: {action}'
        if action == 'KernelSU-Modules/tools/actions/module-release':
            assert ref == 'b6d340ddcd562ad482568b6e66b8d7a9c34d645e', 'KernelSU action upgrade requires a separate review'
    assert 'package-ecosystem: github-actions' in (ROOT / '.github/dependabot.yml').read_text()

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
