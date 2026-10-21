#!/usr/bin/env python3
"""Compare two offline merger outputs by complete canonical sets and counts."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path


def stats(directory):
    values = dict(line.split("=", 1) for line in (directory / "rule_stats.conf").read_text().splitlines())
    return {key: int(values[key]) for key in ("black", "white")}


def compare(left, right):
    result = {"left": str(left), "right": str(right), "left_counts": stats(left), "right_counts": stats(right)}
    result["equal"] = result["left_counts"] == result["right_counts"]
    result["files"] = {}
    for name in ("hosts_rules", "adguard_rules"):
        a = gzip.decompress((left / (name + ".gz")).read_bytes())
        b = gzip.decompress((right / (name + ".gz")).read_bytes())
        aset = {line for line in a.decode().splitlines() if line and not line.startswith("#")}
        bset = {line for line in b.decode().splitlines() if line and not line.startswith("#")}
        details = {"left_unique": len(aset), "right_unique": len(bset),
                   "left_only": len(aset - bset), "right_only": len(bset - aset),
                   "left_only_examples": sorted(aset - bset)[:10],
                   "right_only_examples": sorted(bset - aset)[:10],
                   "canonical_bytes_equal": a == b,
                   "left_sha256": hashlib.sha256(a).hexdigest(),
                   "right_sha256": hashlib.sha256(b).hexdigest()}
        result["files"][name] = details
        result["equal"] &= aset == bset
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    result = compare(args.left, args.right)
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    print(text, end="")
    if args.report:
        args.report.write_text(text)
    return 0 if result["equal"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
