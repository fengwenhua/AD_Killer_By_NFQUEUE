#!/usr/bin/env python3
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import merge_rules  # noqa: E402


def main():
    failures = []
    cases = ROOT / "tests" / "rule_cases.tsv"
    for raw in cases.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        source, accepted, kind, domain = raw.split("\t")
        parsed = merge_rules.parse_adguard_line(source)
        if (parsed is None) != (accepted == "0"):
            failures.append(f"acceptance mismatch: {source!r} -> {parsed!r}")
            continue
        if accepted == "1" and parsed[:2] != (kind, domain):
            failures.append(f"result mismatch: {source!r} -> {parsed!r}")

    if failures:
        print("\n".join(f"FAIL: {failure}" for failure in failures), file=sys.stderr)
        return 1
    print("merge rule tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
