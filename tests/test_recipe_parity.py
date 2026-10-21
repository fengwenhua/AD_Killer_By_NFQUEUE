#!/usr/bin/env python3
"""Offline C++/Python parity: same bytes, typed inputs and complete sets."""
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import merge_rules as reference
from compare_rule_results import compare


def main():
    with tempfile.TemporaryDirectory(prefix="ad_killer_by_nfqueue_parity_") as temporary:
        base = Path(temporary)
        snapshot = base / "input"
        snapshot.mkdir()
        manifest = []
        for i, (name, url, _, fmt) in enumerate(reference.ADGUARD_SOURCES):
            kind = "hosts-as-adguard" if fmt == "hosts" else "adguard"
            file = f"{i:02d}.txt"
            if i == 3:
                file = "-"  # Reproducible download failure, not a live network.
            elif i == 4:
                (snapshot / file).write_bytes(b"<!doctype html><html><body>Error</body></html>")
            elif kind == "hosts-as-adguard":
                (snapshot / file).write_text(
                    "0.0.0.0 Ads.Example\n127.0.0.1\tduplicate.example\n"
                    "*.wild.example\nroot.example.\nroot.example..\n"
                    "0.0.0.0 ads.example # comment\n127.0.0.1 localhost\n"
                    "0.0.0.0 comment.example # ## not a cosmetic rule\n"
                    ".invalid.example\n" + "a" * 64 + ".example\n")
            else:
                text = ("! title\n||Ads.Example^\n@@||allow.example^\n"
                        "duplicate.example\n||duplicate.example^\n"
                        "||adservice.google.\n||ads.example^$third-party\n"
                        "||" + "a" * 64 + ".example^\n"
                        "||" + ".".join(["a" * 63] * 4) + "^\n"
                        "||ads.example##.ad\n||Kelvin.example^\n")
                # A legitimate browser list can have <5% supported rules.
                text += "example.org##.cosmetic\n" * 100
                payload = text.encode()
                (snapshot / file).write_bytes(gzip.compress(payload, mtime=0) if i == 1 else payload)
            manifest.append(f"{kind}\t{file}\t{url}\n")
        (snapshot / "sources.tsv").write_text("".join(manifest))
        pyout, cppout = base / "python", base / "cpp"
        subprocess.run([sys.executable, str(ROOT / "scripts/merge_rules.py"),
                        "--from-snapshot", str(snapshot), "--out-dir", str(pyout)], check=True,
                       stdout=subprocess.PIPE, text=True)
        subprocess.run([sys.argv[1], str(snapshot), str(cppout)], check=True,
                       stdout=subprocess.PIPE, text=True)
        for stem in ("hosts_rules", "adguard_rules"):
            expected = gzip.decompress((pyout / f"{stem}.gz").read_bytes())
            actual = gzip.decompress((cppout / f"{stem}.gz").read_bytes())
            assert actual == expected, (stem, actual, expected)
        assert (pyout / "rule_stats.conf").read_bytes() == (cppout / "rule_stats.conf").read_bytes()
        assert compare(pyout, cppout)["equal"]
        artifact = cppout / "adguard_rules.gz"
        original = artifact.read_bytes()
        artifact.write_bytes(gzip.compress(gzip.decompress(original) + b"extra.example\n", mtime=0))
        # Matching counters alone must never hide a different rule set.
        assert not compare(pyout, cppout)["equal"]
        artifact.write_bytes(original)
        report = (cppout / "source_stats.conf").read_text()
        assert "accepted_sources=11\n" in report and "rejected_sources=2\n" in report
        assert "publish_status=snapshot_only\n" in report
        # Existing output directories and traversal must not overwrite files.
        sentinel = cppout / "sentinel"
        sentinel.write_text("preserve")
        assert subprocess.run([sys.argv[1], str(snapshot), str(cppout)],
                              capture_output=True).returncode != 0
        assert sentinel.read_text() == "preserve"
        (snapshot / "sources.tsv").write_text("adguard\t../secret\thttps://example.org/a\n")
        assert subprocess.run([sys.argv[1], str(snapshot), str(base / "bad")],
                              capture_output=True).returncode != 0
        # No HaGeZi: the dedicated Hosts output is intentionally comment-only.
        assert (cppout / "hosts_rules.txt").read_text() == "# AD_Killer_By_NFQUEUE generated Hosts rules\n"
    print("13-source native/Python complete-set parity, typed Hosts, gzip, failures, limits and offline isolation passed")


if __name__ == "__main__":
    main()
